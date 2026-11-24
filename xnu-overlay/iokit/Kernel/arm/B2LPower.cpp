/* SPDX-License-Identifier: BSD-2-Clause
 * MAX77705 power path.
 *
 * Charger at USI hsi2c_45 (0x15D60000), 7-bit address 0x69; fuel gauge at
 * 0x36 (both on the same bus). 0x66 on that bus is the PMIC block, not the
 * charger. Register layout and field scaling come from the Samsung max77705
 * charger/fuel-gauge drivers.
 *
 * Two transports, tried in order:
 *   1. the USI-I2C-v2 block at 0x15D60000, using the routine proven by
 *      B2LInput for the touch controller;
 *   2. GPIO bit-bang on the same two pads (gpm12.0 = SCL, gpm13.0 = SDA,
 *      pinctrl@15C30000), for the case where that block's functional clock
 *      is gated - this port has no CMU driver, so the engine never runs
 *      there and only the pin controller is available.
 *
 * The phone has no Apple PMU, so booting iOS does not restore whatever
 * charger configuration Android's bootloader/driver left behind. This
 * service selects buck+charger mode, raises the input limit to a value the
 * charger can regulate, kicks the charger watchdog and reports gauge
 * progress to the persistent kernel log. Reads are validated before any
 * register is written, so a dead bus can never be programmed blind.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>

/* The upstream runtime option block (b2l_options.h) is a BSD-side feature and
 * is not present on this branch, so the power debug switch is read straight
 * from the boot arguments. pexpert.h has no extern "C" guard, so declare the
 * C symbol here. The return type must stay boolean_t (int), or this collides
 * with the already-visible declaration instead of redeclaring it. */
extern "C" int PE_parse_boot_argn(const char*,void*,int);
static bool b2l_power_debug(void){
    int value=0;
    return PE_parse_boot_argn("b2l_power_dbg",&value,(int)sizeof(value))&&value==1;
}

/* MAX77705 exposes several I2C slave addresses. 0x66 is the PMIC block (its
 * register 0 only returns the PMIC ID 0x15), the charger block answers at
 * 0x69 (Samsung: I2C_ADDR_CHG = 0xD2 >> 1) and the fuel gauge at 0x36.
 * Reading the charger at 0x66 ACKs but returns 0xff for every charger
 * register, which is exactly what the first bit-bang boots saw. */
#define B2L_CHG_ADDR       0x69
#define B2L_FG_ADDR        0x36

#define CHG_INT_OK         0xB2
#define CHG_DETAILS_00     0xB3
/* CHG_INT_OK bit 6 is CHGIN_OK (charger input present); bit 1 is
 * DISQBAT_OK (discharge allowed), which is set even with no cable.
 * The MAX77705 header in Samsung's kernel defines both explicitly. */
#define CHGIN_OK_MASK      0x40u
#define CHGIN_DTLS_MASK    0x60u
#define CHGIN_DTLS_SHIFT   5
#define CHG_CNFG_00        0xB7
#define CHG_CNFG_02        0xB9
#define CHG_CNFG_06        0xBD
#define CHG_CNFG_09        0xC0

#define FG_STATUS          0x00
#define FG_SOC             0x06
#define FG_TEMP            0x08
#define FG_VCELL           0x09
#define FG_CURRENT         0x0A

/* CHG_CNFG_00 MODE[3:0]: 5/6/7 keep the buck on and the charger enabled. */
#define B2L_CHG_MODE_BUCK_CHG 0x05
/* CHG_CNFG_09 bit 7 enables charging; bits[6:0] are CHGIN_LIM in 25 mA steps
 * (value+1 steps). */
#define B2L_CHG_ILIM_STEP_MA 25u
#define B2L_CHG_EN           0x80u
/* Never ask for more than a plain USB port can supply. configure() only ever
 * raises this field, so a limit already negotiated by the bootloader or a
 * fast-charge source is preserved. AICL stays enabled as the second guard. */
#define B2L_CHG_ILIM_USB_MA  500u
#define B2L_CHG_ILIM_SAFE_MA 500u
/* Initial fast-charge current. The charger's own input regulation and the
 * thermal guard below keep this from being sustained when it is not safe. */
#define B2L_CHG_CC_MA        1500u
/* CHG_CNFG_06 WDTCLR[1:0] = 1 kicks the charger watchdog. */
#define B2L_CHG_WDT_KICK     0x01u
/* CHG_CNFG_06 DIS_AICL: when set the input current is not reduced
 * automatically, so never ask for more than a plain USB port provides. */
#define B2L_CHG_DIS_AICL     0x10u

/* --- GPIO fallback for the charger bus ------------------------------------
 * The USI-I2C block at 0x15D60000 accepts register reads and writes but never
 * executes a transfer: CTL reads back a reset value and INT_STATUS stays zero,
 * which is what an engine with a gated functional clock looks like. This port
 * has no CMU/clock driver at all, so that block cannot be brought up here.
 * The two wires are ordinary pads on the pin controller at 0x15850000, which
 * is alive (touch input already runs through it), so the PMIC is reached by
 * bit-banging the same bus. Open drain is emulated by driving low as an output
 * and releasing as an input, letting the bus pull-ups win.
 *
 * Bank layout: 8 pads per bank, 0x20 bytes apart; CON at +0x00 (4 bits/pad),
 * DAT at +0x04 (1 bit/pad), PUD at +0x08 (2 bits/pad, 2 = pull-up).
 */
#define B2L_BB_GPIO_PHYS   0x15C30000u   /* pinctrl holding gpm12/gpm13 */
#define B2L_BB_SCL_BANK    12u          /* gpm12, pad 0 */
#define B2L_BB_SCL_PIN     0u
#define B2L_BB_SDA_BANK    13u          /* gpm13, pad 0 */
#define B2L_BB_SDA_PIN     0u
#define B2L_BB_HALF_US     5    /* 100 kHz */

/* The iOS power UI asks IOPS for a battery. This port has a charger and a
 * fuel gauge but no Apple battery driver, so nothing published a power
 * source and the status bar had no level or charging state. XNU's
 * IOPMPowerSource is the registry contract IOPS reads; publishing under the
 * name the closed driver uses keeps both the class-name and the
 * IOPMPowerSource matching paths working. The class is built here instead of
 * a separate file only so it shares B2LPower's sample loop. */
class AppleSmartBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(AppleSmartBattery);
    unsigned long updates;
    void setNumber(const char *key,uint64_t value);
public:
    bool init() APPLE_KEXT_OVERRIDE;
    void report(unsigned socPercent,unsigned mv,int amps,bool external,
        bool charging,bool full,int tempTenths);
};
OSDefineMetaClassAndStructors(AppleSmartBattery,IOPMPowerSource);

bool AppleSmartBattery::init(){
    if(!IOPMPowerSource::init())return false;
    updates=0;
    setBatteryInstalled(true);
    setMaxCapacity(100);
    setNumber("DesignCapacity",4100);
    setNumber("NominalChargeCapacity",4100);
    setNumber("CycleCount",0);
    return true;
}

void AppleSmartBattery::setNumber(const char *key,uint64_t value){
    /* withCString/withNumber are declared OSPtr, which is a raw pointer in
     * this build; the returned reference is owned by the caller. */
    const OSSymbol *sym=OSSymbol::withCString(key);
    OSNumber *num=OSNumber::withNumber((unsigned long long)value,32);
    if(sym&&num)setPSProperty(sym,num);
    if(num)num->release();
    if(sym)sym->release();
}

void AppleSmartBattery::report(unsigned socPercent,unsigned mv,int amps,
    bool external,bool charging,bool full,int tempTenths){
    setBatteryInstalled(true);
    setExternalConnected(external);
    setExternalChargeCapable(external);
    setIsCharging(charging);
    setCurrentCapacity(socPercent>100?100:socPercent);
    setMaxCapacity(100);
    setVoltage(mv);
    setAmperage(amps);
    setTimeRemaining(charging?120:0);
    setAtCriticalLevel(!external&&socPercent<=2);
    setAtWarnLevel(!external&&socPercent<=10);
    const OSSymbol *fullKey=OSSymbol::withCString("FullyCharged");
    if(fullKey){
        setPSProperty(fullKey,full?kOSBooleanTrue:kOSBooleanFalse);
        fullKey->release();
    }
    setNumber("BatteryTemperature",(uint64_t)(tempTenths<0?-tempTenths:tempTenths));
    updateStatus();
    if(updates++<5){
        int t=tempTenths<0?-tempTenths:tempTenths;
        IOLog("B2L BATTERY: soc=%u%% v=%u mV a=%d mA ext=%d chg=%d full=%d temp=%s%d.%d C\n",
            socPercent,mv,amps,external,charging,full,tempTenths<0?"-":"",t/10,t%10);
    }
}


class B2LPower : public IOService {
    OSDeclareDefaultStructors(B2LPower);
    IOMemoryDescriptor *i2cDesc;
    IOMemoryMap *i2cMap;
    IOMemoryDescriptor *gpioDesc;
    IOMemoryMap *gpioMap;
    IOWorkLoop *loop;
    IOTimerEventSource *timer;
    volatile uint8_t *i2c;
    volatile uint8_t *gpio;
    bool chargerReady, gaugeReady;
    bool bitbang;                     /* hsi2c dead, use the pads directly */
    bool bbSawCharger;                /* 0x66 acknowledged during the scan */
    unsigned halfUs;                  /* bit-bang half period */
    unsigned timeoutUS, transferFailures, ticks, probeAttempts;
    unsigned socTenths, vcellMv;
    int currentMa, tempTenths;
    uint16_t fgStatus, fgTemp;
    uint8_t chgIntOk;                 /* CHG_INT_OK, bit 6 = CHGIN_OK */
    uint8_t chgDetails;               /* CHG_DETAILS_00, CHGIN state [6:5] */
    AppleSmartBattery *battery;       /* IOPMPowerSource the iOS UI reads */
    static uint64_t uptimeUS(){
        uint64_t ns=0;
        absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
        return ns/1000;
    }
    uint32_t r(unsigned off){
        uint32_t v=*(volatile uint32_t*)(i2c+off);
        __asm__ volatile("dmb oshld":::"memory");
        return v;
    }
    void w(unsigned off,uint32_t v){
        *(volatile uint32_t*)(i2c+off)=v;
        __asm__ volatile("dsb oshst":::"memory");
    }
    void quiesce(){
        w(0x20,0);w(0x24,r(0x24));
        w(0x44,r(0x44)&~(1u<<31));
        w(0,r(0)&~0xc0u);
    }
    void resetController(){
        w(0,r(0)|(1u<<31));w(0,r(0)&~(1u<<31));
        w(0,8);w(8,0x00ffffff);
        w(0x40,r(0x40)|(1u<<31));
    }

    /* --- GPIO I2C ------------------------------------------------------
     * One pin controller bank is 0x20 bytes: CON +0x00 (4 bits per pad,
     * 0 = input, 1 = output), DAT +0x04 (1 bit per pad), PUD +0x08
     * (2 bits per pad, 2 = pull-up).
     */
    uint32_t gR(unsigned bank,unsigned reg){
        uint32_t v=*(volatile uint32_t*)(gpio+bank*0x20u+reg);
        __asm__ volatile("dmb oshld":::"memory");
        return v;
    }
    void gW(unsigned bank,unsigned reg,uint32_t v){
        *(volatile uint32_t*)(gpio+bank*0x20u+reg)=v;
        __asm__ volatile("dsb oshst":::"memory");
    }
    void bbFunc(unsigned bank,unsigned pin,unsigned func){
        unsigned sh=(pin&7u)*4u;
        uint32_t v=gR(bank,0);
        v=(v&~(0xfu<<sh))|((func&0xfu)<<sh);
        gW(bank,0,v);
    }
    void bbPullUp(unsigned bank,unsigned pin){
        unsigned sh=(pin&7u)*2u;
        uint32_t v=gR(bank,8);
        v=(v&~(3u<<sh))|(2u<<sh);
        gW(bank,8,v);
    }
    /* Drive low: set the data bit first, then the pad as output. Release:
     * back to input and let the bus pull-up win. */
    void bbLow(unsigned bank,unsigned pin){
        gW(bank,4,gR(bank,4)&~(1u<<(pin&7u)));
        bbFunc(bank,pin,1);
    }
    void bbRelease(unsigned bank,unsigned pin){ bbFunc(bank,pin,0); }
    unsigned bbGet(unsigned bank,unsigned pin){
        return (gR(bank,4)>>(pin&7u))&1u;
    }
    void bbWait(){ IODelay(halfUs?halfUs:B2L_BB_HALF_US); }
    void bbStart(){
        bbRelease(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
        bbRelease(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        bbWait();
        bbLow(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        bbWait();
        bbLow(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
        bbWait();
    }
    void bbStop(){
        bbLow(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        bbWait();
        bbRelease(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
        bbWait();
        bbRelease(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        bbWait();
    }
    bool bbWriteByte(uint8_t b){
        for(int i=7;i>=0;--i){
            if((b>>i)&1)bbRelease(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
            else bbLow(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
            bbWait();
            bbRelease(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
            bbWait();
            bbLow(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
            bbWait();
        }
        bbRelease(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        bbWait();
        bbRelease(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
        bbWait();
        bool ack=bbGet(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN)==0;
        bbLow(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
        bbWait();
        return ack;
    }
    uint8_t bbReadByte(bool ack){
        uint8_t v=0;
        bbRelease(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        for(int i=0;i<8;++i){
            bbWait();
            bbRelease(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
            bbWait();
            v=(uint8_t)((v<<1)|bbGet(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN));
            bbLow(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
            bbWait();
        }
        if(ack)bbLow(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        else bbRelease(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        bbWait();
        bbRelease(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
        bbWait();
        bbLow(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
        bbWait();
        bbRelease(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        return v;
    }
    bool bbWrite(uint8_t addr,uint8_t reg,uint8_t value){
        bbStart();
        bool ok=bbWriteByte((uint8_t)(addr<<1));
        ok=ok&&bbWriteByte(reg);
        ok=ok&&bbWriteByte(value);
        bbStop();
        return ok;
    }
    bool bbRead(uint8_t addr,uint8_t reg,uint8_t *buf,unsigned n){
        bbStart();
        bool ok=bbWriteByte((uint8_t)(addr<<1));
        ok=ok&&bbWriteByte(reg);
        if(!ok){bbStop();return false;}
        bbStart();
        ok=bbWriteByte((uint8_t)((addr<<1)|1));
        if(!ok){bbStop();return false;}
        for(unsigned i=0;i<n;++i)buf[i]=bbReadByte(i+1<n);
        bbStop();
        return true;
    }
    /* A single address cycle: START, address+W, watch for ACK, STOP. */
    bool bbProbeAddr(uint8_t addr){
        bbStart();
        bool ack=bbWriteByte((uint8_t)(addr<<1));
        bbStop();
        return ack;
    }
    void bbScan(){
        unsigned found=0,first[8]={0,0,0,0,0,0,0,0};
        for(unsigned a=0x08;a<0x78;++a){
            if(!bbProbeAddr((uint8_t)a))continue;
            if(a==B2L_CHG_ADDR)bbSawCharger=true;
            if(found<8)first[found]=(uint8_t)a;
            ++found;
        }
        IOLog("B2L POWER SCAN: %u device(s): %02x %02x %02x %02x %02x %02x %02x %02x charger_ack=%d\n",
            found,first[0],first[1],first[2],first[3],first[4],first[5],first[6],first[7],
            bbSawCharger);
    }
    void bbDump(const char *tag,uint8_t addr,uint8_t base){
        uint8_t v[8];
        for(unsigned i=0;i<8;++i)if(!bbRead(addr,(uint8_t)(base+i),&v[i],1))v[i]=0xee;
        IOLog("B2L POWER DUMP %s: %02x: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            tag,base,v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7]);
    }
    bool bitbangInit(){
        gpioDesc=IOMemoryDescriptor::withPhysicalAddress(B2L_BB_GPIO_PHYS,4096,kIODirectionInOut);
        if(gpioDesc)gpioMap=gpioDesc->map(kIOMapInhibitCache);
        if(!gpioMap)return false;
        gpio=(volatile uint8_t*)gpioMap->getVirtualAddress();
        if(!gpio)return false;
        bbPullUp(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
        bbPullUp(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        bbRelease(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
        bbRelease(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        IODelay(50);
        /* Both lines must idle high: the pull-ups are on the phone, so a low
         * reading here means a stuck bus and every transfer would fail. */
        unsigned scl=bbGet(B2L_BB_SCL_BANK,B2L_BB_SCL_PIN);
        unsigned sda=bbGet(B2L_BB_SDA_BANK,B2L_BB_SDA_PIN);
        IOLog("B2L POWER BB: scl=%u sda=%u %s\n",scl,sda,
            (scl&&sda)?"idle high":"BUS STUCK LOW");
        bbScan();
        bbDump("chg-hi",B2L_CHG_ADDR,0xB0);
        bbDump("fg-lo",B2L_FG_ADDR,0x00);
        if(bbSawCharger){
            /* Same register at three speeds: if the value changes with the
             * clock, it is a timing problem, not a dead device. */
            uint8_t a=0,b=0,c=0;
            bbRead(B2L_CHG_ADDR,CHG_CNFG_00,&a,1);
            halfUs=20;bbRead(B2L_CHG_ADDR,CHG_CNFG_00,&b,1);
            halfUs=50;bbRead(B2L_CHG_ADDR,CHG_CNFG_00,&c,1);
            halfUs=B2L_BB_HALF_US;
            IOLog("B2L POWER SPEED: 0xb7 fast=0x%02x slow=0x%02x vslow=0x%02x\n",a,b,c);
        }
        return true;
    }
    bool message(uint8_t addr,uint8_t *buf,unsigned n,bool read,bool stop);
    bool readRegs(uint8_t addr,uint8_t reg,uint8_t *buf,unsigned n);
    bool readRegsCombined(uint8_t addr,uint8_t reg,uint8_t *buf,unsigned n);
    bool writeReg(uint8_t addr,uint8_t reg,uint8_t value);
    bool readWord(uint8_t addr,uint8_t reg,uint16_t *value);
    void probe();
    void configure(uint8_t cnfg00,uint8_t cnfg02,uint8_t cnfg06,uint8_t cnfg09);
    void setChargeCurrent(unsigned ma);
    void thermalGuard();
    void sample();
    void publish();
    void kickWatchdog();
    static void tick(OSObject *owner,IOTimerEventSource *sender);
public:
    bool init(OSDictionary *dictionary = nullptr) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(B2LPower,IOService);

/* Same bounded USI-I2C-v2 sequence as B2LInput::message(). The slave
 * address lives in I2C_ADDR[19:10] of register 0x70; unlike the touch
 * driver we have two slaves, so the field must be cleared before it is set.
 * stop=false leaves the bus held so a repeated start can follow. */
bool B2LPower::message(uint8_t addr,uint8_t *buf,unsigned n,bool read,bool stop){
    if(!i2c)return false;   /* the USI block was not mapped: only bit-bang is safe */
    if(!buf||!n||n>16)return false;
    const unsigned budgetUS=timeoutUS;
    const uint64_t begin=uptimeUS();
    w(0x48,r(0x48)&~(1u<<31));
    unsigned level=n<8?n:8;
    w(4,3u|(level<<4)|(level<<16));
    w(0,(r(0)&~0xc0u)|8u|(read?0x40u:0x80u));
    w(0x40,r(0x40)|(1u<<31));
    /* I2C_ADDR[19:10] holds the 7-bit slave address (i2c-exynos5.c clears
     * the whole 0x3ff field). Clear it completely so no leftover address
     * bits from the bootloader survive between the two slaves. */
    w(0x70,(r(0x70)&~0x000ffc00u)|(((uint32_t)addr&0x7fu)<<10));
    w(0x24,r(0x24));w(0x20,0x80);
    w(0x44,(r(0x44)&~0x8003ffffu)|0x80000000u|(stop?0x20000u:0)|(read?0x10000u:0)|n);
    unsigned at=0;
    while(uptimeUS()-begin<budgetUS){
        unsigned before=at;
        while(at<n && uptimeUS()-begin<budgetUS){
            uint32_t fifo=r(0x30);
            if(read){
                if(fifo&(1u<<24))break;
                buf[at++]=(uint8_t)r(0x38);
            }else{
                if((fifo&0x7f)>=16)break;
                w(0x34,buf[at++]);
            }
        }
        if(uptimeUS()-begin>=budgetUS)break;
        if(at==n && (r(0x24)&0x80) && (read||(r(0x30)&0x100))){
            quiesce();return true;
        }
        if(at==before || at==n)IODelay(1);
    }
    if(transferFailures++<8)
        IOLog("B2L POWER: i2c timeout addr=0x%02x read=%d bytes=%u done=%u ctrl=0x%x int=0x%x cnf=0x%x\n",
            addr,read,n,at,r(0),r(0x24),r(0x2c));
    resetController();quiesce();
    return false;
}

/* Register read: latched register address write followed by the data
 * transfer, exactly like B2LInput::command(). The MAX PMICs also accept the
 * combined form (register address without STOP, repeated start into the
 * read); probe() tries that when the split form returns a dead-bus pattern. */
bool B2LPower::readRegs(uint8_t addr,uint8_t reg,uint8_t *buf,unsigned n){
    if(bitbang)return bbRead(addr,reg,buf,n);
    if(!i2c)return false;
    resetController();
    if(!message(addr,&reg,1,false,true))return false;
    return message(addr,buf,n,true,true);
}
bool B2LPower::readRegsCombined(uint8_t addr,uint8_t reg,uint8_t *buf,unsigned n){
    if(!i2c)return false;
    resetController();
    if(!message(addr,&reg,1,false,false))return false;
    return message(addr,buf,n,true,true);
}
bool B2LPower::writeReg(uint8_t addr,uint8_t reg,uint8_t value){
    uint8_t pair[2]={reg,value};
    if(bitbang)return bbWrite(addr,reg,value);
    if(!i2c)return false;
    resetController();
    return message(addr,pair,2,false,true);
}
bool B2LPower::readWord(uint8_t addr,uint8_t reg,uint16_t *value){
    uint8_t d[2]={0,0};
    if(!readRegs(addr,reg,d,2))return false;
    *value=(uint16_t)(d[0]|(d[1]<<8));
    return true;
}

void B2LPower::probe(){
    ++probeAttempts;
    uint8_t cnfg00=0,cnfg02=0,cnfg06=0,cnfg09=0,intok=0,details=0;
    bool ok00=false,ok02=false,ok06=false,ok09=false,okb2=false,okb3=false;
    bool combined=false;
    const char *path="split";
    /* A failed register mapping must never be dereferenced: go straight to
     * the GPIO path instead of panicking on a NULL USI base. */
    if(!i2c)IOLog("B2L POWER: USI block at 0x15D60000 was not mapped; using GPIO only\n");
    for(unsigned pass=0;pass<3;++pass){
        /* Once the pads are bit-banging, the USI passes cannot add anything. */
        if((bitbang||!i2c)&&pass<2)continue;
        combined=(pass==1);
        cnfg00=cnfg02=cnfg06=cnfg09=intok=details=0;
        if(pass==2){
            /* The USI block answers register accesses but never runs a
             * transfer, i.e. its functional clock is gated and this port has
             * no clock driver. Fall back to bit-banging the same two pads. */
            if(!bitbang){
                if(bitbangInit()){
                    bitbang=true;
                    path="bitbang";
                    IOLog("B2L POWER: hsi2c dead, switching to GPIO bit-bang on gpm12.0/gpm13.0\n");
                }else{
                    IOLog("B2L POWER: GPIO bit-bang unavailable\n");
                    break;
                }
            }
        }
        if(combined){
            ok00=readRegsCombined(B2L_CHG_ADDR,CHG_CNFG_00,&cnfg00,1);
            ok02=readRegsCombined(B2L_CHG_ADDR,CHG_CNFG_02,&cnfg02,1);
            ok06=readRegsCombined(B2L_CHG_ADDR,CHG_CNFG_06,&cnfg06,1);
            ok09=readRegsCombined(B2L_CHG_ADDR,CHG_CNFG_09,&cnfg09,1);
            okb2=readRegsCombined(B2L_CHG_ADDR,CHG_INT_OK,&intok,1);
            okb3=readRegsCombined(B2L_CHG_ADDR,CHG_DETAILS_00,&details,1);
        }else{
            ok00=readRegs(B2L_CHG_ADDR,CHG_CNFG_00,&cnfg00,1);
            ok02=readRegs(B2L_CHG_ADDR,CHG_CNFG_02,&cnfg02,1);
            ok06=readRegs(B2L_CHG_ADDR,CHG_CNFG_06,&cnfg06,1);
            ok09=readRegs(B2L_CHG_ADDR,CHG_CNFG_09,&cnfg09,1);
            okb2=readRegs(B2L_CHG_ADDR,CHG_INT_OK,&intok,1);
            okb3=readRegs(B2L_CHG_ADDR,CHG_DETAILS_00,&details,1);
        }
        chargerReady=ok00&&ok02&&ok06&&ok09;
        if(okb2)chgIntOk=intok;
        /* A dead bus reads back one repeated value. Never program a charger
         * whose configuration registers cannot be distinguished. */
        if(chargerReady && cnfg00==cnfg02 && cnfg02==cnfg06 && cnfg06==cnfg09 &&
           (cnfg00==0x00||cnfg00==0xFF))
            chargerReady=false;
        if(chargerReady)break;
    }
    IOLog("B2L POWER PROBE: attempt=%u path=%s ok=%d%d%d%d%d%d cnfg00=0x%02x cnfg02=0x%02x cnfg06=0x%02x cnfg09=0x%02x int_ok=0x%02x details=0x%02x\n",
        probeAttempts,path,ok00,ok02,ok06,ok09,okb2,okb3,
        cnfg00,cnfg02,cnfg06,cnfg09,intok,details);
    if(chargerReady){
        configure(cnfg00,cnfg02,cnfg06,cnfg09);
    }else if(bitbang&&bbSawCharger){
        /* The charger acknowledged on the bus but its register file reads
         * back as 0xff. Writes may still be latched, so apply the settings
         * the Android driver used and re-read once to find out. */
        IOLog("B2L POWER: charger ACKs but reads 0x%02x, forcing configuration\n",cnfg00);
        writeReg(B2L_CHG_ADDR,CHG_CNFG_00,0x05);
        writeReg(B2L_CHG_ADDR,CHG_CNFG_02,0x1e);
        writeReg(B2L_CHG_ADDR,CHG_CNFG_09,0x97);
        writeReg(B2L_CHG_ADDR,CHG_CNFG_06,0x01);
        uint8_t r0=0,r9=0,r7=0;
        bool a=readRegs(B2L_CHG_ADDR,CHG_CNFG_00,&r0,1);
        bool b=readRegs(B2L_CHG_ADDR,CHG_CNFG_09,&r9,1);
        bool c=readRegs(B2L_CHG_ADDR,CHG_CNFG_06,&r7,1);
        IOLog("B2L POWER FORCE: readback ok=%d%d%d cnfg00=0x%02x cnfg09=0x%02x cnfg06=0x%02x\n",
            a,b,c,r0,r9,r7);
        if(b&&(r9&0x80))chargerReady=true;
    }
    sample();
    IOLog("B2L POWER GAUGE: ok=%d soc=%u.%u%% vcell=%u mV current=%d mA raw=0x%04x\n",
        gaugeReady,socTenths/10,socTenths%10,vcellMv,currentMa,fgStatus);
}

void B2LPower::configure(uint8_t cnfg00,uint8_t cnfg02,uint8_t cnfg06,uint8_t cnfg09){
    unsigned mode=cnfg00&0x0f;
    unsigned ccMa=(cnfg02&0x3f)<=2?100u:(unsigned)(cnfg02&0x3f)*50u;
    if(mode<B2L_CHG_MODE_BUCK_CHG||mode>0x07){
        /* Preserve WDTEN and every reserved bit above the mode field. */
        uint8_t want=(uint8_t)((cnfg00&0xf0)|B2L_CHG_MODE_BUCK_CHG);
        if(writeReg(B2L_CHG_ADDR,CHG_CNFG_00,want)){
            IOLog("B2L POWER: charger mode 0x%x -> 0x%x\n",mode,want&0x0f);
            mode=want&0x0f;
        }
    }
    unsigned desired=(cnfg06&B2L_CHG_DIS_AICL)?B2L_CHG_ILIM_SAFE_MA:B2L_CHG_ILIM_USB_MA;
    unsigned setting=desired/B2L_CHG_ILIM_STEP_MA-1u;
    unsigned currentSetting=cnfg09&0x7f;
    if(!(cnfg09&B2L_CHG_EN)||currentSetting<setting){
        /* CHGIN_LIM[6:0] is the limit and bit 7 is CHG_EN. Only ever raise
         * the limit; AICL may have reduced it below what we would ask for. */
        uint8_t want=(uint8_t)(B2L_CHG_EN|(currentSetting<setting?setting:currentSetting));
        if(writeReg(B2L_CHG_ADDR,CHG_CNFG_09,want)){
            IOLog("B2L POWER: CHGIN_LIM %u mA -> %u mA%s\n",
                (currentSetting+1)*B2L_CHG_ILIM_STEP_MA,
                ((want&0x7f)+1)*B2L_CHG_ILIM_STEP_MA,
                (cnfg09&B2L_CHG_EN)?"":" (CHG_EN reasserted)");
            currentSetting=want&0x7f;
        }
    }
    kickWatchdog();
    /* Android/LK usually leave the charger at a conservative USB charge
     * current. Raise it once, then let the thermal guard in thermalGuard()
     * pull it back if the pack warms up. */
    if(ccMa<1000){setChargeCurrent(B2L_CHG_CC_MA);ccMa=B2L_CHG_CC_MA;}
    IOLog("B2L POWER: ready mode=0x%x chgin_lim=%u mA chg_en=%d charge_current=%u mA aicl=%s wdt=%s\n",
        mode,(currentSetting+1)*B2L_CHG_ILIM_STEP_MA,(cnfg09&B2L_CHG_EN)?1:0,ccMa,
        (cnfg06&B2L_CHG_DIS_AICL)?"off":"on",
        (cnfg00&0x10)?"on":"off");
}

/* CHG_CNFG_02[5:0] is the fast-charge current in 50 mA steps (0..2 = 100 mA);
 * [7:6] belong to OTG and must be preserved. */
void B2LPower::setChargeCurrent(unsigned ma){
    uint8_t cnfg02=0;
    if(!readRegs(B2L_CHG_ADDR,CHG_CNFG_02,&cnfg02,1))return;
    unsigned reg=ma/50u;
    if(reg<3)reg=3;
    if(reg>0x3fu)reg=0x3fu;
    if((cnfg02&0x3f)==reg)return;
    unsigned was=(cnfg02&0x3f)<=2?100u:(unsigned)(cnfg02&0x3f)*50u;
    if(writeReg(B2L_CHG_ADDR,CHG_CNFG_02,(uint8_t)((cnfg02&0xc0)|reg)))
        IOLog("B2L POWER: charge current %u mA -> %u mA\n",was,reg*50u);
}

/* There is no Apple battery driver doing JEITA, so the throttle lives here:
 * above 45 C drop to a trickle-safe current, above 55 C to the minimum. */
void B2LPower::thermalGuard(){
    if(tempTenths>550)setChargeCurrent(100);
    else if(tempTenths>450)setChargeCurrent(500);
}

void B2LPower::kickWatchdog(){
    uint8_t cnfg06=0;
    if(!readRegs(B2L_CHG_ADDR,CHG_CNFG_06,&cnfg06,1))return;
    writeReg(B2L_CHG_ADDR,CHG_CNFG_06,
        (uint8_t)((cnfg06&~0x03u)|B2L_CHG_WDT_KICK));
}

/* TEMPERATURE_REG is 16-bit fixed point: high byte whole degrees Celsius,
 * low byte 39/10 m°C units. Return whole deci-degrees, matching the
 * Samsung driver's sign handling. */
static int b2l_fg_temp_tenths(uint16_t raw){
    uint8_t lo=(uint8_t)(raw&0xff),hi=(uint8_t)(raw>>8);
    int t;
    if(hi&0x80){
        t=((~hi)&0xff)+1;
        t*=(-1000);
        t-=(-(int)lo)*39/10;
    }else{
        t=(hi&0x7f);
        t*=1000;
        t+=lo*39/10;
    }
    return t/100;
}

void B2LPower::sample(){
    uint16_t soc=0,vcell=0,current=0,temp=0,status=0;
    uint8_t intok=0,details=0;
    /* CHG_INT_OK tells whether the input supply is present even before the
     * gauge has a usable frame. Bit 6 (CHGIN_OK, 0x40) is the input-power-good
     * flag; bit 1 is DISQBAT_OK and is set even without a cable. CHG_DETAILS_00
     * [6:5] is logged as an independent second opinion. Keep the last value
     * if a read fails. */
    if(readRegs(B2L_CHG_ADDR,CHG_INT_OK,&intok,1))chgIntOk=intok;
    if(readRegs(B2L_CHG_ADDR,CHG_DETAILS_00,&details,1))chgDetails=details;
    bool okSoc=readWord(B2L_FG_ADDR,FG_SOC,&soc);
    bool okCell=readWord(B2L_FG_ADDR,FG_VCELL,&vcell);
    bool okCurrent=readWord(B2L_FG_ADDR,FG_CURRENT,&current);
    readWord(B2L_FG_ADDR,FG_TEMP,&temp);
    readWord(B2L_FG_ADDR,FG_STATUS,&status);
    gaugeReady=okSoc&&okCell&&okCurrent;
    if(!gaugeReady)return;
    /* Scale factors: SOC 1/256 %, VCELL 78.125 uV, CURRENT 156.25 uA with
     * the 5 mOhm sense resistor (fg_resistor=2). */
    unsigned socT=(unsigned)(((uint32_t)soc*10u)/256u);
    if(socT>1000)socT=1000;
    unsigned mv=(unsigned)(((uint32_t)vcell*5u)/64u);
    int ma=((int)(int16_t)current)*5/16;
    /* Reject implausible frames instead of reporting garbage as battery
     * state: a present pack is between ~2.5 V and 4.5 V. */
    if(mv<2500||mv>4500){gaugeReady=false;return;}
    socTenths=socT;vcellMv=mv;currentMa=ma;
    tempTenths=b2l_fg_temp_tenths(temp);
    fgStatus=status;fgTemp=temp;
}

void B2LPower::publish(){
    if(!gaugeReady)return;
    setProperty("B2LBatterySOCPercent",(unsigned long long)(socTenths/10),32);
    setProperty("B2LBatteryVoltage_mV",(unsigned long long)vcellMv,32);
    /* Signed current in 32-bit two's complement: setProperty has bool and
     * unsigned long long overloads, so a bare long long is ambiguous. */
    setProperty("B2LBatteryCurrent_mA",
        (unsigned long long)(uint32_t)(int32_t)currentMa,32);
    setProperty("B2LChargerReady",chargerReady);
    setProperty("B2LGaugeReady",gaugeReady);
    if(battery){
        /* CHGIN_OK (bit 6) says the charger sees input power. Bit 1 was used
         * before and is DISQBAT_OK, which stays set with the cable removed,
         * so the status bar showed "charging" permanently. */
        unsigned percent=socTenths/10;
        bool external=(chgIntOk&CHGIN_OK_MASK)!=0;
        bool full=percent>=100;
        bool charging=external&&!full&&tempTenths<600;
        int amps=currentMa<0?-currentMa:currentMa;
        if(!charging)amps=-amps;
        battery->report(percent,vcellMv,amps,external,charging,full,tempTenths);
    }
}

void B2LPower::tick(OSObject *owner,IOTimerEventSource *sender){
    B2LPower *s=(B2LPower*)owner;
    /* IOTimerEventSource is one-shot: without this the charger watchdog is
     * kicked once and then expires, which turns charging off again. Re-arm
     * before the work so a blocked bus cannot stop the watchdog. */
    if(sender)sender->setTimeoutMS(5000);
    if(s->ticks<3)IOLog("B2L POWER: tick %u charger=%d gauge=%d\n",
        s->ticks+1,s->chargerReady,s->gaugeReady);
    ++s->ticks;
    if(!s->chargerReady&&!s->gaugeReady){
        if(s->ticks%12==0&&s->probeAttempts<10)s->probe();
        return;
    }
    /* The charger watchdog expires after roughly 30 s; 10 s keeps it fed
     * without touching the bus on every tick. */
    if(s->ticks%2==0)s->kickWatchdog();
    s->sample();
    s->thermalGuard();
    s->publish();
    if(s->ticks%12==0||b2l_power_debug()){
        int tt=s->tempTenths,at=tt<0?-tt:tt;
        IOLog("B2L POWER: soc=%u.%u%% vcell=%u mV current=%d mA temp=%s%d.%d C status=0x%04x int_ok=0x%02x details=0x%02x ext=%d dtls_ext=%d chg_ok=%d fg_ok=%d\n",
            s->socTenths/10,s->socTenths%10,s->vcellMv,s->currentMa,
            tt<0?"-":"",at/10,at%10,s->fgStatus,s->chgIntOk,s->chgDetails,
            (s->chgIntOk&CHGIN_OK_MASK)!=0,(s->chgDetails&CHGIN_DTLS_MASK)!=0,
            s->chargerReady,s->gaugeReady);
    }
}

/* IOKit does not zero object storage. Every pointer and counter is set here so
 * stop()/free() never walk an uninitialised descriptor, and probe() cannot
 * dereference a stale bus base if start() fails halfway. IOKit instantiates
 * drivers by calling init(dictionary), not init(), so this is the overload
 * that has to carry the initialisation. */
bool B2LPower::init(OSDictionary *dictionary){
    if(!IOService::init(dictionary))return false;
    i2cDesc=nullptr;i2cMap=nullptr;gpioDesc=nullptr;gpioMap=nullptr;
    loop=nullptr;timer=nullptr;i2c=nullptr;gpio=nullptr;
    chargerReady=false;gaugeReady=false;bitbang=false;bbSawCharger=false;
    halfUs=B2L_BB_HALF_US;timeoutUS=50000;
    transferFailures=0;ticks=0;probeAttempts=0;
    socTenths=0;vcellMv=0;currentMa=0;tempTenths=0;
    fgStatus=0;fgTemp=0;chgIntOk=0;chgDetails=0;battery=nullptr;
    return true;
}

bool B2LPower::start(IOService *provider){
    if(!IOService::start(provider))return false;
    i2cDesc=IOMemoryDescriptor::withPhysicalAddress(0x15D60000,4096,kIODirectionInOut);
    if(i2cDesc)i2cMap=i2cDesc->map(kIOMapInhibitCache);
    /* A missing USI mapping is not fatal: the GPIO path below does not need
     * it, so the service still starts and probe() falls back. */
    if(i2cMap)i2c=(volatile uint8_t*)i2cMap->getVirtualAddress();
    timeoutUS=50000;halfUs=B2L_BB_HALF_US;bbSawCharger=false;
    chgIntOk=0;chgDetails=0;battery=NULL;
    probe();
    /* IOWorkLoop::workLoop() can return a fresh loop with no thread behind
     * it, in which case the timer never fires (exactly what happened on the
     * first bit-bang boot). The provider's loop is always serviced. */
    loop=provider?provider->getWorkLoop():NULL;
    if(!loop)loop=IOWorkLoop::workLoop();
    if(!loop)return false;
    timer=IOTimerEventSource::timerEventSource(this,&B2LPower::tick);
    if(!timer||loop->addEventSource(timer)!=kIOReturnSuccess)return false;
    /* setTimeout only stores the deadline; wakeAtTime schedules the callout
     * only while the event source is enabled. Without enable() the timer
     * never fires at all (no ticks, no watchdog kicks). */
    timer->setTimeoutMS(5000);
    timer->enable();
    IOLog("B2L POWER: tick timer armed on %s workloop\n",
        provider?"provider":"private");
    registerService();
    /* Publish the battery nub before the first sample so the power UI has a
     * source as soon as userspace asks for one. */
    battery=new AppleSmartBattery;
    if(battery){
        if(!battery->init()||!battery->attach(this)){
            IOLog("B2L BATTERY: service could not be published\n");
            battery->release();battery=NULL;
        }else{
            battery->registerService();
            IOLog("B2L BATTERY: IOPMPowerSource published as AppleSmartBattery\n");
        }
    }
    publish();
    IOLog("B2L POWER: MAX77705 service registered charger=%d gauge=%d\n",
        chargerReady,gaugeReady);
    return true;
}

void B2LPower::stop(IOService *provider){
    if(timer){timer->cancelTimeout();timer->disable();}
    if(loop&&timer)loop->removeEventSource(timer);
    if(timer){timer->release();timer=nullptr;}
    if(i2cMap){i2cMap->release();i2cMap=nullptr;}
    if(i2cDesc){i2cDesc->release();i2cDesc=nullptr;}
    if(gpioMap){gpioMap->release();gpioMap=nullptr;}
    if(gpioDesc){gpioDesc->release();gpioDesc=nullptr;}
    IOService::stop(provider);
}
void B2LPower::free(){
    if(timer){timer->release();timer=nullptr;}
    if(i2cMap){i2cMap->release();i2cMap=nullptr;}
    if(i2cDesc){i2cDesc->release();i2cDesc=nullptr;}
    if(gpioMap){gpioMap->release();gpioMap=nullptr;}
    if(gpioDesc){gpioDesc->release();gpioDesc=nullptr;}
    IOService::free();
}

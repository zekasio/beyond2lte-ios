# MAX77705 charger and fuel gauge

`xnu-overlay/iokit/Kernel/arm/B2LPower.cpp` adds a `B2LPower` IOService that
keeps the phone charging and gives iOS a battery to display. It is compiled
into the kernel by `patches/0060-b2l-max77705-charger-service.patch`, which
registers the class in `iokit/conf/files` and `iokit/KernelConfigTables.cpp`
and is applied by `scripts/prepare-xnu.sh` after the existing 0001–0059
series.

This branch has no Apple PMU driver and no Android charger driver running
under XNU, so whatever configuration S-Boot/LK left in the MAX77705 is all the
hardware would otherwise get: the charger watchdog would expire, charging
would stop, and the power UI would have no power source to read.

## Hardware path

| Item | Value |
| --- | --- |
| Charger | MAX77705 charger block, 7-bit I2C `0x69` |
| Fuel gauge | MAX77705 FG block, 7-bit I2C `0x36` |
| Bus | USI-I2C-v2 `hsi2c_45` at `0x15D60000` |
| GPIO fallback | pinctrl `0x15C30000`, gpm12.0 = SCL, gpm13.0 = SDA |

`0x66` is *not* the charger: that address is the PMIC block and reads back
`0xff` for every charger register. The earlier bring-up used `0x66` and saw a
dead device until the address was corrected.

The same USI-I2C-v2 transfer sequence already used by `B2LInput` for the touch
controller is reused here. Unlike the touch bus, this instance's functional
clock is gated on this port (there is no CMU driver), so the engine may accept
register accesses but never complete a transfer. When that is detected — or
when the USI block cannot be mapped at all — the driver switches to
bit-banging the same two pads in open-drain emulation.

## What the service does

1. Maps the USI block (non-fatal if it fails) and probes the charger
   registers, trying split and repeated-start transfers.
2. Rejects a "ready" charger whose four configuration registers are
   indistinguishable `0x00`/`0xff` frames, so a dead bus is never programmed
   blind.
3. If the charger acknowledges but the register file reads `0xff`, writes the
   settings Samsung's driver used and re-reads to confirm.
4. Selects buck+charger mode (`CHG_CNFG_00` MODE 5), re-asserts `CHG_EN` and
   raises the input limit to 500 mA (`CHG_CNFG_09`), only ever increasing a
   limit already negotiated by the bootloader.
5. Raises the fast-charge current once to 1500 mA (`CHG_CNFG_02`) and applies
   a software thermal guard: 500 mA above 45 °C, 100 mA above 55 °C.
6. Kicks the charger watchdog every 10 s via `CHG_CNFG_06`.
7. Samples SOC, cell voltage, current, temperature and status from the fuel
   gauge every 5 s, rejecting frames outside 2.5–4.5 V.
8. Publishes `AppleSmartBattery` (an `IOPMPowerSource`) with external-power,
   charging, full and critical/warning state. `CHGIN_OK` is bit 6 of
   `CHG_INT_OK`; bit 1 is `DISQBAT_OK` and stays set with the cable removed.
9. Publishes `B2LBatterySOCPercent`, `B2LBatteryVoltage_mV`,
   `B2LBatteryCurrent_mA`, `B2LChargerReady` and `B2LGaugeReady` on the
   service for diagnostics.

The timer is armed with `enable()` and re-arms itself, because a one-shot
`IOTimerEventSource` without `enable()` never fires.

## Scaling

Taken from Samsung's `max77705` charger/FG drivers:

- SOC: 1/256 %, reported in 0.1 % units.
- VCELL: 78.125 µV/LSB, reported in mV.
- CURRENT: 156.25 µA/LSB with the 5 mΩ sense resistor (`fg_resistor=2`),
  reported in mA.
- TEMPERATURE: 16-bit fixed point, high byte whole °C, low byte 39/10 m°C.

## Debug

Pass `b2l_power_dbg=1` on the boot arguments (or add `power_dbg=1` where the
runtime option block is available) to log a full sample on every tick instead
of every 12th tick. `B2L POWER`, `B2L POWER PROBE`, `B2L POWER SCAN`,
`B2L POWER BB` and `B2L BATTERY` lines go to the persistent kernel log.

## Status and limitations

- The register map, address and scaling match Samsung's driver sources, but
  **this driver has not been validated on the physical S10+** in this tree.
  Treat the first boot as a probe: check the `B2L POWER PROBE` and
  `B2L BATTERY` lines before relying on charge state.
- There is no Apple PMU, JEITA table, battery authentication, cycle counting
  or percentage learning. `DesignCapacity`/`NominalChargeCapacity` are fixed
  at 4100 mAh and `CycleCount` at 0.
- The thermal guard is a simple two-step current reduction, not a full
  charging profile; it does not implement taper, recharge thresholds or
  temperature-based termination.
- Charging configuration is rewritten on every boot; nothing is persisted.
- Bit-bang timing is a fixed 5 µs half period (~100 kHz) and is not tuned per
  board revision.
- No suspend/resume integration: the timer stops being serviced while the
  system is asleep, and system sleep is not implemented anyway.

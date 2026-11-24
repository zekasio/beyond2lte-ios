/* SPDX-License-Identifier: BSD-2-Clause
 * Mali-G76 (Exynos 9820 G3D, Bifrost tNOx) bring-up probe.
 *
 * The GPU is the one block this port has never touched: no clock driver, no
 * DVFS, no kbase. Everything below is opt-in and defaults to doing nothing.
 *
 *   b2l_gpu_probe=0   (default) no MMIO at all
 *   b2l_gpu_probe=1   read-only: PMU power state, GPU_ID, feature and
 *                     readiness masks. No register write of any kind.
 *   b2l_gpu_probe=2   additionally try to ungate pd-embedded_g3d through the
 *                     PMU, soft-reset, power up the core groups and run one
 *                     NULL job through address space 0 and job slot 0.
 *
 * Safety rules, all deliberate:
 *
 *  - Never read 0x18500000 before the PMU reports G3D powered. A read of a
 *    gated block is a synchronous external abort on this SoC, not a bus error
 *    that can be caught.
 *  - Mask every GPU/JOB/MMU interrupt before touching the job manager. Nothing
 *    installs a handler for SPI 320/319/321 yet, so raw status bits are polled
 *    and the interrupt line is never asserted.
 *  - The job test uses its own address space and an uncached job chain, so it
 *    cannot disturb panel memory or the CPU presentation path, and needs no
 *    cache maintenance.
 *  - Level 1 never writes a register, so a device that reports a powered
 *    domain can be inspected without changing anything.
 *
 * Register offsets, bit layouts and page-table encodings come from
 * drivers/gpu/arm/b_r19p0 (mali_midg_regmap*.h, mali_kbase_mmu_mode_aarch64.c).
 * The pure encodings live in B2LGPU.h and are checked on the host by
 * tests/gpu_wire_test.c.
 *
 * This file does not publish IOAcceleratorES and must not: QuartzCore selects
 * the software renderer precisely because no accelerator exists, and an
 * incomplete accelerator makes it abandon the working CPU path.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSNumber.h>
#include <arm64/b2l_diag.h>
#include "B2LGPU.h"

/* pexpert.h has no extern "C" guard; boolean_t is int in this tree. */
extern "C" int PE_parse_boot_argn(const char*,void*,int);
/* Device mappings for the page tables and job chain. VM_WIMG_IO, so the CPU
 * alias is uncached and the GPU sees every store without cache maintenance. */
extern "C" vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern "C" void ml_io_unmap(vm_offset_t,vm_size_t);

/* --- Physical resources --------------------------------------------------- */
#define B2L_GPU_PHYS            0x18500000u   /* mali@18500000, 20 KiB */
#define B2L_GPU_SIZE            0x5000u
#define B2L_PMU_G3D_PHYS        0x15862000u   /* PMU page holding +0x2104 */
#define B2L_PMU_G3D_SIZE        0x1000u
#define B2L_PMU_G3D_CONFIG      0x100u        /* PMU + 0x2100 */
#define B2L_PMU_G3D_STATUS      0x104u        /* PMU + 0x2104 */
#define B2L_PMU_G3D_POWERED     0x1u

/* GPU control registers (GPU_CONTROL_BASE = 0) */
enum {
	B2L_GPU_REG_ID             = 0x000,
	B2L_GPU_REG_L2_FEATURES    = 0x004,
	B2L_GPU_REG_CORE_FEATURES  = 0x008,
	B2L_GPU_REG_MEM_FEATURES   = 0x010,
	B2L_GPU_REG_MMU_FEATURES   = 0x014,
	B2L_GPU_REG_AS_PRESENT     = 0x018,
	B2L_GPU_REG_JS_PRESENT     = 0x01C,
	B2L_GPU_REG_IRQ_RAWSTAT    = 0x020,
	B2L_GPU_REG_IRQ_CLEAR      = 0x024,
	B2L_GPU_REG_IRQ_MASK       = 0x028,
	B2L_GPU_REG_IRQ_STATUS     = 0x02C,
	B2L_GPU_REG_COMMAND        = 0x030,
	B2L_GPU_REG_STATUS         = 0x034,
	B2L_GPU_REG_FAULTSTATUS    = 0x03C,
	B2L_GPU_REG_FAULTADDR_LO   = 0x040,
	B2L_GPU_REG_FAULTADDR_HI   = 0x044,
	B2L_GPU_REG_SHADER_PRESENT = 0x100,
	B2L_GPU_REG_TILER_PRESENT  = 0x110,
	B2L_GPU_REG_L2_PRESENT     = 0x120,
	B2L_GPU_REG_SHADER_READY   = 0x140,
	B2L_GPU_REG_TILER_READY    = 0x150,
	B2L_GPU_REG_L2_READY       = 0x160,
	B2L_GPU_REG_SHADER_PWRON   = 0x180,
	B2L_GPU_REG_TILER_PWRON    = 0x190,
	B2L_GPU_REG_L2_PWRON       = 0x1A0,
	B2L_GPU_REG_STACK_PRESENT  = 0xE00,
	B2L_GPU_REG_STACK_READY    = 0xE10,
	B2L_GPU_REG_STACK_PWRON    = 0xE20,
	B2L_GPU_REG_JOB_RAWSTAT    = 0x1000,
	B2L_GPU_REG_JOB_CLEAR      = 0x1004,
	B2L_GPU_REG_JOB_MASK       = 0x1008,
	B2L_GPU_REG_MMU_RAWSTAT    = 0x2000,
	B2L_GPU_REG_MMU_CLEAR      = 0x2004,
	B2L_GPU_REG_MMU_MASK       = 0x2008,
	B2L_GPU_REG_AS0            = 0x2400,   /* 0x2000 + AS0(0x400) */
	B2L_GPU_REG_SLOT0          = 0x1800,   /* 0x1000 + JOB_SLOT0(0x800) */
};

/* JOB slot registers, relative to B2L_GPU_REG_SLOT0 */
enum {
	B2L_GPU_JS_HEAD_LO      = 0x00,
	B2L_GPU_JS_TAIL_LO      = 0x08,
	B2L_GPU_JS_COMMAND      = 0x20,
	B2L_GPU_JS_STATUS       = 0x24,
	B2L_GPU_JS_HEAD_NEXT_LO = 0x40,
	B2L_GPU_JS_HEAD_NEXT_HI = 0x44,
	B2L_GPU_JS_AFFIN_NEXT   = 0x50,
	B2L_GPU_JS_AFFIN_NEXT_HI = 0x54,
	B2L_GPU_JS_CONFIG_NEXT  = 0x58,
	B2L_GPU_JS_XAFFIN_NEXT  = 0x5C,
	B2L_GPU_JS_COMMAND_NEXT = 0x60,
};

/* Address-space registers, relative to B2L_GPU_REG_AS0 */
enum {
	B2L_GPU_AS_TRANSTAB_LO  = 0x00,
	B2L_GPU_AS_TRANSTAB_HI  = 0x04,
	B2L_GPU_AS_MEMATTR_LO   = 0x08,
	B2L_GPU_AS_MEMATTR_HI   = 0x0C,
	B2L_GPU_AS_COMMAND      = 0x18,
	B2L_GPU_AS_FAULTSTATUS  = 0x1C,
	B2L_GPU_AS_FAULTADDR_LO = 0x20,
	B2L_GPU_AS_FAULTADDR_HI = 0x24,
	B2L_GPU_AS_STATUS       = 0x28,
	B2L_GPU_AS_TRANSCFG_LO  = 0x30,
	B2L_GPU_AS_TRANSCFG_HI  = 0x34,
};

#define B2L_GPU_COMMAND_SOFT_RESET   0x01u   /* non-CSF: reset, payload in code */
#define B2L_GPU_IRQ_RESET_COMPLETED  (1u << 8)
#define B2L_GPU_AS_COMMAND_UPDATE    0x01u
#define B2L_GPU_AS_STATUS_ACTIVE     0x01u   /* command in flight, clears when done */
#define B2L_GPU_JS_COMMAND_START     0x01u
#define B2L_GPU_JS_COMMAND_HARD_STOP 0x03u
/* JS_CONFIG_NEXT bits 0..3 are the address space number, so AS0 is 0. The
 * remaining fields mirror kbase_job_hw_submit: clean/invalidate the L2 at
 * both ends of the job and run at thread priority 8. */
#define B2L_GPU_JS_CONFIG_AS(n)        ((uint32_t)((n) & 0xFu))
#define B2L_GPU_JS_CONFIG_FLUSH_START  (3u << 8)
#define B2L_GPU_JS_CONFIG_FLUSH_END    (3u << 12)
#define B2L_GPU_JS_CONFIG_THREAD_PRI   (8u << 16)

/* GPU_ID2 field shifts, from mali_kbase_gpu_id.h. */
#define B2L_GPU_ID_ARCH_MAJOR(id)     (((id) >> 28) & 0xFu)
#define B2L_GPU_ID_ARCH_MINOR(id)     (((id) >> 24) & 0xFu)
#define B2L_GPU_ID_PRODUCT_MAJOR(id)  (((id) >> 16) & 0xFu)
#define B2L_GPU_ID_VERSION_MAJOR(id)  (((id) >> 12) & 0xFu)
#define B2L_GPU_ID_VERSION_MINOR(id)  (((id) >> 4) & 0xFFu)
#define B2L_GPU_ID_VERSION_STATUS(id) ((id) & 0xFu)

/* One 4 KiB page of GPU-visible memory: a physically contiguous buffer plus an
 * uncached kernel mapping of the same bytes. */
struct B2LGPUPage {
	IOBufferMemoryDescriptor *buf;
	vm_offset_t               map;
	uint64_t                  pa;
	volatile void            *va;
};

class B2LGPU : public IOService {
	OSDeclareDefaultStructors(B2LGPU);

	IOMemoryDescriptor *fPmuDesc, *fGpuDesc;
	IOMemoryMap        *fPmuMap, *fGpuMap;
	volatile uint32_t  *fPmu, *fGpu;
	unsigned            fLevel;
	bool                fPowered, fResetOk, fProcessorsUp;

	B2LGPUPage          fTable[B2L_GPU_MMU_LEVELS];
	B2LGPUPage          fJob;

	uint32_t r(unsigned off) const {
		uint32_t v = *(volatile uint32_t *)((volatile uint8_t *)fGpu + off);
		__asm__ volatile("dmb oshld" ::: "memory");
		return v;
	}
	void w(unsigned off, uint32_t v) {
		*(volatile uint32_t *)((volatile uint8_t *)fGpu + off) = v;
		__asm__ volatile("dsb oshst" ::: "memory");
	}
	uint32_t pmuR(unsigned off) const {
		return *(volatile uint32_t *)((volatile uint8_t *)fPmu + off);
	}
	void pmuW(unsigned off, uint32_t v) {
		*(volatile uint32_t *)((volatile uint8_t *)fPmu + off) = v;
		__asm__ volatile("dsb sy" ::: "memory");
	}
	bool pollReg(unsigned off, uint32_t mask, uint32_t want, unsigned micros);
	bool waitAsIdle(void);
	bool allocPage(B2LGPUPage *page);
	void freePage(B2LGPUPage *page);
	bool mapRegions(void);
	bool probePower(void);
	void dumpRegisters(void);
	void maskInterrupts(void);
	bool bringUp(void);
	bool probeJob(void);
	void logFault(const char *what);
	void publish(const char *key, uint64_t value);

public:
	bool init(OSDictionary *dictionary = NULL) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(B2LGPU, IOService);

bool
B2LGPU::pollReg(unsigned off, uint32_t mask, uint32_t want, unsigned micros)
{
	for (unsigned i = 0; i < micros; i++) {
		if ((r(off) & mask) == want) {
			return true;
		}
		IODelay(1);
	}
	return (r(off) & mask) == want;
}

void
B2LGPU::publish(const char *key, uint64_t value)
{
	setProperty(key, (unsigned long long)value, 32);
}

/* AS_STATUS_AS_ACTIVE means a command is in flight. It has to be clear before
 * an AS_COMMAND write and it clears again when the command retires. */
bool
B2LGPU::waitAsIdle()
{
	const unsigned off = B2L_GPU_REG_AS0 + B2L_GPU_AS_STATUS;

	for (unsigned i = 0; i < 20000; i++) {
		if ((r(off) & B2L_GPU_AS_STATUS_ACTIVE) == 0) {
			return true;
		}
		IODelay(1);
	}
	return (r(off) & B2L_GPU_AS_STATUS_ACTIVE) == 0;
}

/* IOKit does not zero object storage, and it creates drivers through
 * init(dictionary), so this overload is the one that must clear every field a
 * later stop() or failed start() may touch. */
bool
B2LGPU::init(OSDictionary *dictionary)
{
	if (!IOService::init(dictionary)) {
		return false;
	}
	fPmuDesc = NULL;
	fGpuDesc = NULL;
	fPmuMap = NULL;
	fGpuMap = NULL;
	fPmu = NULL;
	fGpu = NULL;
	fLevel = 0;
	fPowered = false;
	fResetOk = false;
	fProcessorsUp = false;
	memset(fTable, 0, sizeof(fTable));
	memset(&fJob, 0, sizeof(fJob));
	return true;
}

bool
B2LGPU::allocPage(B2LGPUPage *page)
{
	IOBufferMemoryDescriptor *buf;
	IOByteCount length = 0;
	addr64_t phys;

	/* A single 4 KiB page, physically contiguous and page aligned, so the
	 * GPU can walk it and store into it without scatter-gather support. */
	buf = IOBufferMemoryDescriptor::withOptions(
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous,
	    B2L_GPU_MMU_PAGE_SIZE, B2L_GPU_MMU_PAGE_SIZE);
	if (buf == NULL) {
		return false;
	}
	phys = buf->getPhysicalSegment(0, &length);
	if (phys == 0 || length < B2L_GPU_MMU_PAGE_SIZE) {
		buf->release();
		return false;
	}
	page->map = ml_io_map((vm_offset_t)phys, B2L_GPU_MMU_PAGE_SIZE);
	if (page->map == 0) {
		buf->release();
		return false;
	}
	page->buf = buf;
	page->pa = (uint64_t)phys;
	page->va = (volatile void *)page->map;
	memset((void *)page->va, 0, B2L_GPU_MMU_PAGE_SIZE);
	return true;
}

void
B2LGPU::freePage(B2LGPUPage *page)
{
	if (page->map != 0) {
		ml_io_unmap(page->map, B2L_GPU_MMU_PAGE_SIZE);
		page->map = 0;
	}
	if (page->buf != NULL) {
		page->buf->release();
		page->buf = NULL;
	}
	page->va = NULL;
	page->pa = 0;
}

bool
B2LGPU::mapRegions()
{
	fPmuDesc = IOMemoryDescriptor::withPhysicalAddress(B2L_PMU_G3D_PHYS,
	    B2L_PMU_G3D_SIZE, kIODirectionInOut);
	if (fPmuDesc != NULL) {
		fPmuMap = fPmuDesc->map(kIOMapInhibitCache);
	}
	if (fPmuMap == NULL) {
		b2l_diag_record("B2L GPU: PMU page 0x%08x not mapped; probe aborted "
		    "before any GPU access\n", B2L_PMU_G3D_PHYS);
		return false;
	}
	fPmu = (volatile uint32_t *)fPmuMap->getVirtualAddress();
	return true;
}

/* Read the PMU power state and, at level 2, ask the PMU to ungate the domain.
 * True only when the domain reports powered afterwards. */
bool
B2LGPU::probePower()
{
	uint32_t status = pmuR(B2L_PMU_G3D_STATUS);

	b2l_diag_record("B2L GPU: PMU G3D_STATUS=0x%08x powered=%u\n",
	    status, (status & B2L_PMU_G3D_POWERED) ? 1u : 0u);
	if (status & B2L_PMU_G3D_POWERED) {
		fPowered = true;
		return true;
	}
	if (fLevel < 2) {
		b2l_diag_record("B2L GPU: G3D domain is off; no GPU MMIO performed "
		    "(b2l_gpu_probe=2 would ask the PMU)\n");
		return false;
	}

	b2l_diag_record("B2L GPU: setting PMU G3D_CONFIG=0x1\n");
	pmuW(B2L_PMU_G3D_CONFIG, 1);
	for (unsigned i = 0; i < 10000; i++) {
		status = pmuR(B2L_PMU_G3D_STATUS);
		if (status & B2L_PMU_G3D_POWERED) {
			break;
		}
		IODelay(1);
	}
	b2l_diag_record("B2L GPU: after PMU request G3D_STATUS=0x%08x\n", status);
	fPowered = (status & B2L_PMU_G3D_POWERED) != 0;
	if (!fPowered) {
		b2l_diag_record("B2L GPU: domain stayed off; the block is gated by "
		    "ACPM, not by this PMU bit, so boot firmware has to keep it on\n");
	}
	return fPowered;
}

/* Level 1: read everything, write nothing. */
void
B2LGPU::dumpRegisters()
{
	uint32_t id = r(B2L_GPU_REG_ID);
	uint32_t shaderPresent = r(B2L_GPU_REG_SHADER_PRESENT);
	uint32_t tilerPresent = r(B2L_GPU_REG_TILER_PRESENT);
	uint32_t l2Present = r(B2L_GPU_REG_L2_PRESENT);
	uint32_t stackPresent = r(B2L_GPU_REG_STACK_PRESENT);

	publish("gpu-id", id);
	publish("gpu-arch-major", B2L_GPU_ID_ARCH_MAJOR(id));
	publish("gpu-arch-minor", B2L_GPU_ID_ARCH_MINOR(id));
	publish("gpu-product-major", B2L_GPU_ID_PRODUCT_MAJOR(id));
	publish("gpu-version-major", B2L_GPU_ID_VERSION_MAJOR(id));
	publish("gpu-version-minor", B2L_GPU_ID_VERSION_MINOR(id));
	publish("gpu-version-status", B2L_GPU_ID_VERSION_STATUS(id));
	publish("gpu-shader-present", shaderPresent);
	publish("gpu-tiler-present", tilerPresent);
	publish("gpu-l2-present", l2Present);
	publish("gpu-stack-present", stackPresent);

	b2l_diag_record("B2L GPU: ID=0x%08x arch=%u.%u product=%u version=%u.%u "
	    "status=%u\n", id, B2L_GPU_ID_ARCH_MAJOR(id), B2L_GPU_ID_ARCH_MINOR(id),
	    B2L_GPU_ID_PRODUCT_MAJOR(id), B2L_GPU_ID_VERSION_MAJOR(id),
	    B2L_GPU_ID_VERSION_MINOR(id), B2L_GPU_ID_VERSION_STATUS(id));
	b2l_diag_record("B2L GPU: model=%s corefeat=0x%08x l2feat=0x%08x "
	    "memfeat=0x%08x mmufeatures=0x%08x as_present=0x%04x js_present=0x%04x\n",
	    (B2L_GPU_ID_ARCH_MAJOR(id) == 7 &&
	    B2L_GPU_ID_PRODUCT_MAJOR(id) == 1) ? "tNOx(Bifrost)" : "unexpected",
	    r(B2L_GPU_REG_CORE_FEATURES), r(B2L_GPU_REG_L2_FEATURES),
	    r(B2L_GPU_REG_MEM_FEATURES), r(B2L_GPU_REG_MMU_FEATURES),
	    r(B2L_GPU_REG_AS_PRESENT) & 0xffffu,
	    r(B2L_GPU_REG_JS_PRESENT) & 0xffffu);
	b2l_diag_record("B2L GPU: present shader=0x%08x tiler=0x%08x l2=0x%08x "
	    "stack=0x%08x ready shader=0x%08x tiler=0x%08x l2=0x%08x "
	    "gpu_status=0x%08x irq=0x%08x\n",
	    shaderPresent, tilerPresent, l2Present, stackPresent,
	    r(B2L_GPU_REG_SHADER_READY), r(B2L_GPU_REG_TILER_READY),
	    r(B2L_GPU_REG_L2_READY), r(B2L_GPU_REG_STATUS),
	    r(B2L_GPU_REG_IRQ_RAWSTAT));

	fProcessorsUp = shaderPresent != 0 &&
	    (r(B2L_GPU_REG_SHADER_READY) & shaderPresent) == shaderPresent &&
	    (r(B2L_GPU_REG_TILER_READY) & tilerPresent) == tilerPresent &&
	    (r(B2L_GPU_REG_L2_READY) & l2Present) == l2Present &&
	    (stackPresent == 0 ||
	    (r(B2L_GPU_REG_STACK_READY) & stackPresent) == stackPresent);
	publish("gpu-processors-up", fProcessorsUp ? 1 : 0);
}

/* No handler is installed for the GPU SPIs, so silence every source before the
 * job manager is touched. Raw status bits are still readable. */
void
B2LGPU::maskInterrupts()
{
	w(B2L_GPU_REG_IRQ_MASK, 0);
	w(B2L_GPU_REG_JOB_MASK, 0);
	w(B2L_GPU_REG_MMU_MASK, 0);
	w(B2L_GPU_REG_IRQ_CLEAR, 0xffffffffu);
	w(B2L_GPU_REG_JOB_CLEAR, 0xffffffffu);
	w(B2L_GPU_REG_MMU_CLEAR, 0xffffffffu);
}

/* Level 2: soft reset and power up a group only when it is present but not
 * already ready. Writing PWRON for a missing group is undefined. */
bool
B2LGPU::bringUp()
{
	uint32_t shaderPresent, tilerPresent, l2Present, stackPresent;

	maskInterrupts();
	w(B2L_GPU_REG_COMMAND, B2L_GPU_COMMAND_SOFT_RESET);
	if (!pollReg(B2L_GPU_REG_IRQ_RAWSTAT, B2L_GPU_IRQ_RESET_COMPLETED,
	    B2L_GPU_IRQ_RESET_COMPLETED, 10000)) {
		b2l_diag_record("B2L GPU: soft reset did not complete, "
		    "rawstat=0x%08x status=0x%08x\n",
		    r(B2L_GPU_REG_IRQ_RAWSTAT), r(B2L_GPU_REG_STATUS));
		return false;
	}
	w(B2L_GPU_REG_IRQ_CLEAR, B2L_GPU_IRQ_RESET_COMPLETED);
	fResetOk = true;
	publish("gpu-reset", 1);
	b2l_diag_record("B2L GPU: soft reset completed\n");
	/* A soft reset returns the interrupt mask registers to their default
	 * state, so mask every source again before power-up can raise one. No
	 * handler exists for SPI 319/320/321. */
	maskInterrupts();

	shaderPresent = r(B2L_GPU_REG_SHADER_PRESENT);
	tilerPresent = r(B2L_GPU_REG_TILER_PRESENT);
	l2Present = r(B2L_GPU_REG_L2_PRESENT);
	stackPresent = r(B2L_GPU_REG_STACK_PRESENT);

	/* Power-up order is not arbitrary. kbase powers the tiler first because
	 * that also powers L2 slice 0 (kbase_pm_l2_update_state), waits for the
	 * L2 group to be ready, then brings up the core stacks and only after
	 * those the shader cores (kbase_pm_shaders_update_state). The hardware
	 * power controller latches a request for a group whose parent is still
	 * off, so requesting shaders first can leave them never ready. */
	if (tilerPresent != 0 &&
	    (r(B2L_GPU_REG_TILER_READY) & tilerPresent) != tilerPresent) {
		w(B2L_GPU_REG_TILER_PWRON, tilerPresent);
	}
	/* Slice 0 of L2 comes up with the tiler; any further slices are
	 * requested explicitly, exactly as kbase does. If there is no tiler,
	 * ask for all of L2 instead. */
	uint32_t l2Request = tilerPresent != 0 ? (l2Present & ~1u) : l2Present;
	if (l2Request != 0 &&
	    (r(B2L_GPU_REG_L2_READY) & l2Present) != l2Present) {
		w(B2L_GPU_REG_L2_PWRON, l2Request);
	}
	pollReg(B2L_GPU_REG_TILER_READY, tilerPresent, tilerPresent, 10000);
	pollReg(B2L_GPU_REG_L2_READY, l2Present, l2Present, 10000);

	if (stackPresent != 0 &&
	    (r(B2L_GPU_REG_STACK_READY) & stackPresent) != stackPresent) {
		w(B2L_GPU_REG_STACK_PWRON, stackPresent);
		pollReg(B2L_GPU_REG_STACK_READY, stackPresent, stackPresent, 10000);
	}
	if (shaderPresent != 0 &&
	    (r(B2L_GPU_REG_SHADER_READY) & shaderPresent) != shaderPresent) {
		w(B2L_GPU_REG_SHADER_PWRON, shaderPresent);
		pollReg(B2L_GPU_REG_SHADER_READY, shaderPresent, shaderPresent,
		    10000);
	}

	b2l_diag_record("B2L GPU: ready shader=0x%08x tiler=0x%08x l2=0x%08x "
	    "stack=0x%08x\n", r(B2L_GPU_REG_SHADER_READY),
	    r(B2L_GPU_REG_TILER_READY), r(B2L_GPU_REG_L2_READY),
	    r(B2L_GPU_REG_STACK_READY));
	fProcessorsUp = shaderPresent != 0 &&
	    (r(B2L_GPU_REG_SHADER_READY) & shaderPresent) == shaderPresent &&
	    (r(B2L_GPU_REG_TILER_READY) & tilerPresent) == tilerPresent &&
	    (r(B2L_GPU_REG_L2_READY) & l2Present) == l2Present &&
	    (stackPresent == 0 ||
	    (r(B2L_GPU_REG_STACK_READY) & stackPresent) == stackPresent);
	publish("gpu-processors-up", fProcessorsUp ? 1 : 0);
	if (!fProcessorsUp) {
		b2l_diag_record("B2L GPU: core groups did not all become ready; "
		    "job test skipped\n");
		return false;
	}
	return true;
}

void
B2LGPU::logFault(const char *what)
{
	b2l_diag_record("B2L GPU: %s fault: gpu_faultstatus=0x%08x "
	    "gpu_faultaddr=0x%08x%08x js_status=0x%08x "
	    "as_faultstatus=0x%08x as_faultaddr=0x%08x%08x\n", what,
	    r(B2L_GPU_REG_FAULTSTATUS),
	    r(B2L_GPU_REG_FAULTADDR_HI), r(B2L_GPU_REG_FAULTADDR_LO),
	    r(B2L_GPU_REG_SLOT0 + B2L_GPU_JS_STATUS),
	    r(B2L_GPU_REG_AS0 + B2L_GPU_AS_FAULTSTATUS),
	    r(B2L_GPU_REG_AS0 + B2L_GPU_AS_FAULTADDR_HI),
	    r(B2L_GPU_REG_AS0 + B2L_GPU_AS_FAULTADDR_LO));
}

/* Build address space 0 with one mapped page and start a one-job chain on
 * slot 0. A NULL job still exercises the whole path: the descriptor itself is
 * fetched through the new page tables, so completion proves the MMU walked
 * them and the job manager accepted and retired the chain. */
bool
B2LGPU::probeJob()
{
	uint64_t *tables[B2L_GPU_MMU_LEVELS];
	uint64_t tablePa[B2L_GPU_MMU_LEVELS];
	const uint64_t jobVa = 0x20000000ull;
	uint32_t rawBefore, affinity;

	/* The bitmap registers are the only authoritative statement that this
	 * GPU has address space 0 and job slot 0 at all. */
	if ((r(B2L_GPU_REG_AS_PRESENT) & 1u) == 0 ||
	    (r(B2L_GPU_REG_JS_PRESENT) & 1u) == 0) {
		b2l_diag_record("B2L GPU: AS0 or job slot 0 not present "
		    "(as_present=0x%08x js_present=0x%08x); job test skipped\n",
		    r(B2L_GPU_REG_AS_PRESENT), r(B2L_GPU_REG_JS_PRESENT));
		return false;
	}

	for (unsigned i = 0; i < B2L_GPU_MMU_LEVELS; i++) {
		if (!allocPage(&fTable[i])) {
			b2l_diag_record("B2L GPU: page-table allocation failed at "
			    "level %u\n", i);
			return false;
		}
		tables[i] = (uint64_t *)(void *)fTable[i].va;
		tablePa[i] = fTable[i].pa;
	}
	if (!allocPage(&fJob)) {
		b2l_diag_record("B2L GPU: job chain allocation failed\n");
		return false;
	}

	if (b2l_gpu_mmu_map(tables, tablePa, jobVa, fJob.pa, 1,
	    B2L_GPU_MMU_MEMATTR_NON_CACHE) != 0) {
		b2l_diag_record("B2L GPU: page-table build rejected va=0x%llx "
		    "pa=0x%llx\n", (unsigned long long)jobVa,
		    (unsigned long long)fJob.pa);
		return false;
	}
	b2l_diag_record("B2L GPU: page tables l0..l3=0x%llx,0x%llx,0x%llx,0x%llx "
	    "map va=0x%llx -> pa=0x%llx\n",
	    (unsigned long long)tablePa[0], (unsigned long long)tablePa[1],
	    (unsigned long long)tablePa[2], (unsigned long long)tablePa[3],
	    (unsigned long long)jobVa, (unsigned long long)fJob.pa);

	/* One 64-bit NULL job with no successor ends the chain. */
	{
		struct b2l_gpu_job_header header;
		b2l_gpu_job_header_init(&header, B2L_GPU_JOB_NULL, 1, 0);
		memcpy((void *)fJob.va, &header, sizeof(header));
	}
	__asm__ volatile("dsb sy" ::: "memory");

	w(B2L_GPU_REG_AS0 + B2L_GPU_AS_TRANSTAB_LO,
	    (uint32_t)(tablePa[0] & 0xfffff000ull));
	w(B2L_GPU_REG_AS0 + B2L_GPU_AS_TRANSTAB_HI, (uint32_t)(tablePa[0] >> 32));
	w(B2L_GPU_REG_AS0 + B2L_GPU_AS_TRANSCFG_LO,
	    B2L_GPU_AS_TRANSCFG_VALUE);
	w(B2L_GPU_REG_AS0 + B2L_GPU_AS_TRANSCFG_HI, 0);
	w(B2L_GPU_REG_AS0 + B2L_GPU_AS_MEMATTR_LO, B2L_GPU_AS_MEMATTR_LO);
	w(B2L_GPU_REG_AS0 + B2L_GPU_AS_MEMATTR_HI, B2L_GPU_AS_MEMATTR_HI);
	/* kbase waits for AS_STATUS_AS_ACTIVE to clear before writing a command
	 * and polls it afterwards until the command has retired. The bit means
	 * "command in flight", not "address space usable", so waiting for it to
	 * become set would race the hardware. */
	if (!waitAsIdle()) {
		b2l_diag_record("B2L GPU: address space 0 busy before update, "
		    "status=0x%08x\n", r(B2L_GPU_REG_AS0 + B2L_GPU_AS_STATUS));
		return false;
	}
	w(B2L_GPU_REG_AS0 + B2L_GPU_AS_COMMAND, B2L_GPU_AS_COMMAND_UPDATE);
	if (!waitAsIdle()) {
		b2l_diag_record("B2L GPU: address space 0 update did not retire, "
		    "status=0x%08x faultstatus=0x%08x\n",
		    r(B2L_GPU_REG_AS0 + B2L_GPU_AS_STATUS),
		    r(B2L_GPU_REG_AS0 + B2L_GPU_AS_FAULTSTATUS));
		return false;
	}
	b2l_diag_record("B2L GPU: address space 0 updated, faultstatus=0x%08x\n",
	    r(B2L_GPU_REG_AS0 + B2L_GPU_AS_FAULTSTATUS));

	/* Affinity must name at least one shader core or the slot has nowhere
	 * to run. A NULL job has no tiler work, so no XAFFINITY is written and
	 * the address space stays 0 in JS_CONFIG_NEXT bits 3:0. */
	affinity = r(B2L_GPU_REG_SHADER_PRESENT);
	if (affinity == 0) {
		affinity = 1;
	}
	rawBefore = r(B2L_GPU_REG_JOB_RAWSTAT);
	/* Same order as kbase_job_hw_submit: head first, then affinity, then
	 * the per-slot configuration, and the command last. */
	w(B2L_GPU_REG_SLOT0 + B2L_GPU_JS_HEAD_NEXT_LO, (uint32_t)jobVa);
	w(B2L_GPU_REG_SLOT0 + B2L_GPU_JS_HEAD_NEXT_HI, (uint32_t)(jobVa >> 32));
	w(B2L_GPU_REG_SLOT0 + B2L_GPU_JS_AFFIN_NEXT, affinity);
	w(B2L_GPU_REG_SLOT0 + B2L_GPU_JS_AFFIN_NEXT_HI, 0);
	w(B2L_GPU_REG_SLOT0 + B2L_GPU_JS_CONFIG_NEXT,
	    B2L_GPU_JS_CONFIG_AS(0) | B2L_GPU_JS_CONFIG_FLUSH_START |
	    B2L_GPU_JS_CONFIG_FLUSH_END | B2L_GPU_JS_CONFIG_THREAD_PRI);
	w(B2L_GPU_REG_SLOT0 + B2L_GPU_JS_COMMAND_NEXT, B2L_GPU_JS_COMMAND_START);

	/* Any change in the raw job status is the completion edge. */
	for (unsigned i = 0; i < 100000; i++) {
		if (r(B2L_GPU_REG_JOB_RAWSTAT) != rawBefore) {
			break;
		}
		IODelay(1);
	}

	uint32_t raw = r(B2L_GPU_REG_JOB_RAWSTAT);
	uint32_t jsStatus = r(B2L_GPU_REG_SLOT0 + B2L_GPU_JS_STATUS);
	b2l_diag_record("B2L GPU: job chain rawstat 0x%08x -> 0x%08x "
	    "js_status=0x%08x head=0x%08x tail=0x%08x\n", rawBefore, raw,
	    jsStatus, r(B2L_GPU_REG_SLOT0 + B2L_GPU_JS_HEAD_LO),
	    r(B2L_GPU_REG_SLOT0 + B2L_GPU_JS_TAIL_LO));
	publish("gpu-job-rawstat", raw);
	publish("gpu-job-status", jsStatus);

	w(B2L_GPU_REG_JOB_CLEAR, raw);
	if (raw == rawBefore) {
		logFault("job timeout");
		/* Never leave a chain running on a slot nobody owns. */
		w(B2L_GPU_REG_SLOT0 + B2L_GPU_JS_COMMAND,
		    B2L_GPU_JS_COMMAND_HARD_STOP);
		publish("gpu-jobs", 0);
		return false;
	}
	if (jsStatus != 0) {
		logFault("job");
		publish("gpu-jobs", 0);
		return false;
	}
	publish("gpu-jobs", 1);
	b2l_diag_record("B2L GPU: NULL job completed through MMU address space 0\n");
	return true;
}

bool
B2LGPU::start(IOService *provider)
{
	int level = 0;

	if (!IOService::start(provider)) {
		return false;
	}
	if (PE_parse_boot_argn("b2l_gpu_probe", &level, (int)sizeof(level)) &&
	    level > 0) {
		fLevel = (unsigned)level;
	}
	publish("b2l-gpu-probe-level", fLevel);
	publish("gpu-jobs", 0);
	registerService();
	if (fLevel == 0) {
		return true;
	}

	b2l_diag_record("B2L GPU: probe level %u requested\n", fLevel);
	if (!mapRegions()) {
		return true;
	}
	if (!probePower()) {
		return true;
	}

	fGpuDesc = IOMemoryDescriptor::withPhysicalAddress(B2L_GPU_PHYS,
	    B2L_GPU_SIZE, kIODirectionInOut);
	if (fGpuDesc != NULL) {
		fGpuMap = fGpuDesc->map(kIOMapInhibitCache);
	}
	if (fGpuMap == NULL) {
		b2l_diag_record("B2L GPU: 0x%08x not mapped although the domain is "
		    "powered\n", B2L_GPU_PHYS);
		return true;
	}
	fGpu = (volatile uint32_t *)fGpuMap->getVirtualAddress();

	dumpRegisters();
	if (fLevel >= 2 && bringUp()) {
		probeJob();
	}
	return true;
}

void
B2LGPU::stop(IOService *provider)
{
	for (unsigned i = 0; i < B2L_GPU_MMU_LEVELS; i++) {
		freePage(&fTable[i]);
	}
	freePage(&fJob);
	OSSafeReleaseNULL(fGpuMap);
	OSSafeReleaseNULL(fGpuDesc);
	OSSafeReleaseNULL(fPmuMap);
	OSSafeReleaseNULL(fPmuDesc);
	IOService::stop(provider);
}

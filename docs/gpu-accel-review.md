# Mali-G76 bring-up: what this branch actually does

Status: **kernel-side probe only, never run on hardware yet.** Nothing here
draws a pixel. This document records why, what was verified against the pinned
kbase sources, and what a real acceleration path would still need.

The board patch is `patches/0061-b2l-mali-g3d-probe-service.patch`; the driver
is `xnu-overlay/iokit/Kernel/arm/B2LGPU.cpp` and its pure encoders are in
`B2LGPU.h`, checked on the host by `tests/gpu_wire_test.c`.

## What it does

`B2LGPU` is an `IOService` on `IOResources`. It is inert unless a boot argument
asks otherwise:

| boot arg | behaviour |
| :--- | :--- |
| `b2l_gpu_probe=0` (default) | no MMIO of any kind |
| `b2l_gpu_probe=1` | read-only: PMU G3D status, `GPU_ID`, feature/readiness/present masks |
| `b2l_gpu_probe=2` | additionally request `pd-embedded_g3d` through the PMU, soft-reset, power up the core groups, program address space 0 and run one NULL job on slot 0 |

Level 1 writes no register at all, so a device whose G3D domain is already
powered can be inspected without changing state.

The probe publishes what it found as IORegistry properties (`gpu-id`,
`gpu-arch-major`, `gpu-arch-minor`, `gpu-product-major`, `gpu-version-*`,
`gpu-shader-present`, `gpu-tiler-present`, `gpu-l2-present`,
`gpu-stack-present`, `gpu-processors-up`, `gpu-reset`, `gpu-job-rawstat`,
`gpu-job-status`, `gpu-jobs`) and writes the same values to the persistent
`b2l_diag` log, so the result survives a reboot even when the device later
hangs.

### Safety rules in the code

1. **No GPU MMIO before the PMU says the domain is up.** Reading
   `0x18500000..0x18504fff` with `0x15862104 & 1 == 0` is a synchronous
   external abort on this SoC, not a catchable bus error.
2. **All GPU/JOB/MMU interrupt sources are masked before the job manager is
   touched.** No handler is installed for SPI 319/320/321 yet, so the code
   only polls raw status bits and never lets the interrupt line assert. The
   masks are written again after the soft reset, because a reset returns those
   registers to their defaults.
3. **The job test owns a private address space and its own pages.** It cannot
   disturb the scanout buffers or the CPU presentation path, and every page it
   touches is mapped uncached, so there is no cache maintenance to get wrong.
4. **A failed or timed-out chain is hard-stopped** before the function returns.
5. **`IOAcceleratorES` is never published.** QuartzCore drops `SWContext` and
   the working CPU renderer when an accelerator exists but is not complete, so
   an early stub would break the display for no gain.

## Facts verified against `drivers/gpu/arm/b_r19p0`

These correct several assumptions in `docs/mali-g76-re-roadmap.md`:

| Topic | Roadmap said | b_r19p0 actually does |
| :--- | :--- | :--- |
| MMU mode | stage-1 LPAE | `AS_TRANSCFG_ADRMODE_AARCH64_4K` (4-level AArch64 4 KiB), `GPU_ID2_PRODUCT_TNOX` path |
| Job header | ~24 bytes | `struct job_descriptor_header` is 32 bytes; `next_job` at `+0x18` |
| G3D clock | enable in CMU | `g3d_cmu_cal_id` is an ACPM/DVFS clock ID; the kernel-side gate is the PMU status/config pair, and the clock itself is owned by firmware |
| Slot address space | implicit | `JS_CONFIG_NEXT` bits 3:0 are `kctx->as_nr`; bit 10 (`START_MMU`) is only set for `BASE_HW_ISSUE_10649` |
| XAFFINITY | needed per job | never written by `kbase_job_hw_submit` in this release |
| Page-table walk attributes | default | `mmu_hw_configure` adds `PTW_MEMATTR_WRITE_BACK` + `R_ALLOCATE`, and `PTW_SH_OS` only when the system is ACE-coherent |

This port cannot prove coherency, and it builds the tables through uncached
aliases, so `B2LGPU` asks for **non-cacheable page-table walks**
(`AS_TRANSCFG_LO = 0x01000006`) and for the job chain itself uses the
non-cacheable `AS_MEMATTR` lane (index 5, byte `0x4c`). Both values are pinned
by `tests/gpu_wire_test.c`.

### Ordering that is not optional

Two sequences were copied from kbase rather than invented, because the
hardware does not accept them in any order:

* **Power-up.** The tiler is requested first because that also powers L2
  slice 0; only then are extra L2 slices, the core stacks and finally the
  shader cores requested, waiting for each group's `READY` bit in turn
  (`kbase_pm_l2_update_state`, `kbase_pm_shaders_update_state`). Shader cores
  requested before their parent groups can stay not-ready forever.
* **Job submission.** `JS_HEAD_NEXT_LO/HI`, then `JS_AFFINITY_NEXT_LO/HI`,
  then `JS_CONFIG_NEXT`, then `JS_COMMAND_NEXT = START`, the same order as
  `kbase_job_hw_submit`. `JS_CONFIG_NEXT` bits 3:0 carry the address space
  number (0 here), bits 10:8 and 14:12 clean and invalidate the L2 at the
  start and end of the job, and bits 19:16 hold thread priority 8.

### Registers used

* `GPU_CONTROL` (`0x0000`): `GPU_ID`, `*_FEATURES`, `AS_PRESENT`,
  `JS_PRESENT`, `GPU_IRQ_*`, `GPU_COMMAND`, `GPU_STATUS`, `*_PRESENT`,
  `*_READY`, `*_PWRON`.
* Job control (`0x1000`): `JOB_IRQ_RAWSTAT/CLEAR/MASK`, `JOB_SLOT0`
  (`0x1800`): head/tail, `JS_COMMAND_NEXT`, `JS_CONFIG_NEXT`,
  `JS_AFFINITY_NEXT`, `JS_STATUS`.
* MMU (`0x2000`): `MMU_IRQ_*`, `AS0` (`0x2400`): transtab, transcfg,
  memattr, command, fault status/address, status.
* PMU page `0x15862000`: `+0x100` config, `+0x104` status.

## The actual blocker for "GPU acceleration"

The only thing that could be offloaded today is the full-frame BGRA copy in
`AppleCLCD::swap` (`B2LFramebuffer.cpp`, around line 305). The GPU cannot do
that copy yet, because:

1. On Bifrost a fragment job is preceded by a tiler job that writes tile lists
   and a tile framebuffer descriptor; that geometry/tiling front end is the
   part the port has never had.
2. The job must execute a Bifrost ISA shader. There is no AIR → Bifrost
   compiler anywhere in this stack, and Metal on the host side will not accept
   a hand-written ISA.
3. Even a blit needs the above plus fencing against `AppleCLCD`'s scanout, and
   a way to recover the GPU if a job faults without taking down the display.

A boot-arg kernel probe is therefore the honest first milestone: it answers
whether the domain can be brought up and whether the job manager and MMU work
on this port at all, with the answer in the persistent log.

## Expected log lines (not yet observed on hardware)

```
B2L GPU: probe level 2 requested
B2L GPU: PMU G3D_STATUS=0x........ powered=?
B2L GPU: ID=0x........ arch=7.0 product=1 version=...
B2L GPU: model=tNOx(Bifrost) corefeat=... mmufeatures=...
B2L GPU: soft reset completed
B2L GPU: ready shader=... tiler=... l2=... stack=...
B2L GPU: page tables l0..l3=... map va=0x20000000 -> pa=...
B2L GPU: address space 0 updated, faultstatus=0x00000000
B2L GPU: job chain rawstat 0x00000000 -> 0x00000001 js_status=0x00000000 ...
B2L GPU: NULL job completed through MMU address space 0
```

A fault prints `gpu_faultstatus`, `gpu_faultaddr`, `js_status` and the AS fault
registers instead.

## Next steps, in order

1. Flash a build containing this service, boot **without** the boot argument,
   and confirm the device is unchanged (`b2l_gpu_probe` defaults to 0, so the
   only new thing is an idle IOService).
2. Boot with `b2l_gpu_probe=1` and read the log. This alone tells us the
   domain state and the exact `GPU_ID`/feature masks, with no writes.
3. If the domain is up, `b2l_gpu_probe=2` and check for `AS0 updated` and the
   NULL-job completion. If the PMU cannot ungate the domain, the next attempt
   belongs in UEFI before `ExitBootServices`, where the roadmap already
   points.
4. Only after a NULL job completes is a GPU blit worth designing: it needs
   tiler job + fragment job + a prebuilt Bifrost shader, plus a fallback to
   `B2LBlit` when the GPU is absent or faults.

None of this is a claim of acceleration. It is the measurement step that any
acceleration work has to start from.

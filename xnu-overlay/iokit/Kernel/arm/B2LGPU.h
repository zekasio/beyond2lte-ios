/* SPDX-License-Identifier: BSD-2-Clause
 * Mali-G76 (Bifrost tNOx) job/MMU wire encodings, shared by the kernel
 * service and the host tests. Nothing in this header touches hardware: every
 * function is a pure byte-layout computation so tests/ can verify the exact
 * entries that B2LGPU.cpp would program.
 *
 * Layouts come from drivers/gpu/arm/b_r19p0 (mali_midg_regmap*.h,
 * mali_kbase_mmu_mode_aarch64.c) and the published job-descriptor header
 * (exception_status, first_incomplete_task, fault_pointer, size/type,
 * barrier/flags, job_index, two dependency indices, next_job).
 */
#ifndef B2L_GPU_H
#define B2L_GPU_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* --- Job descriptor header -------------------------------------------------
 * 32 bytes on both Midgard and Bifrost. The bitfields are little-endian:
 * byte 0x10 bit 0 is job_descriptor_size, bits 7:1 are the job type; byte
 * 0x11 bit 0 is job_barrier. A zero next_job ends the chain.
 */
struct b2l_gpu_job_header {
	uint32_t exception_status;
	uint32_t first_incomplete_task;
	uint64_t fault_pointer;
	uint8_t  size_and_type;
	uint8_t  barrier_and_flags;
	uint16_t job_index;
	uint16_t dependency_1;
	uint16_t dependency_2;
	uint64_t next_job;
} __attribute__((packed));

#define B2L_GPU_JOB_HEADER_SIZE 32u

/* Job types from the architecture specification. */
enum {
	B2L_GPU_JOB_NOT_STARTED   = 0,
	B2L_GPU_JOB_NULL          = 1,
	B2L_GPU_JOB_WRITE_VALUE   = 2,
	B2L_GPU_JOB_CACHE_FLUSH   = 3,
	B2L_GPU_JOB_COMPUTE       = 4,
	B2L_GPU_JOB_VERTEX        = 5,
	B2L_GPU_JOB_GEOMETRY      = 6,
	B2L_GPU_JOB_TILER         = 7,
	B2L_GPU_JOB_FUSED         = 8,
	B2L_GPU_JOB_FRAGMENT      = 9,
};

/* 1 selects the 64-bit descriptor form used by r19p0. */
static inline void
b2l_gpu_job_header_init(struct b2l_gpu_job_header *h, unsigned type,
    unsigned size64, uint64_t next)
{
	memset(h, 0, sizeof(*h));
	h->size_and_type = (uint8_t)(((type & 0x7fu) << 1) | (size64 ? 1u : 0u));
	h->next_job = next;
}

static inline unsigned
b2l_gpu_job_header_type(const struct b2l_gpu_job_header *h)
{
	return (unsigned)(h->size_and_type >> 1);
}

/* --- Stage-1 AArch64 4 KiB page tables (AS_TRANSCFG_ADRMODE_AARCH64_4K) ----
 * Valid table entries at levels 0..2 carry type 1, leaf entries at level 3
 * carry type 3. Bits 4:2 select one of the eight attribute bytes programmed
 * in AS_MEMATTR, bit 6 is read/write, bit 10 is the access flag and bit 54 is
 * execute-never. The GPU walks 48-bit virtual addresses, so every pointer is
 * physical and must be 4 KiB aligned.
 */
#define B2L_GPU_MMU_PAGE_SIZE          4096ull
#define B2L_GPU_MMU_ENTRIES_PER_TABLE  512u
#define B2L_GPU_MMU_LEVELS             4u
#define B2L_GPU_MMU_REGION_SIZE        0x200000ull   /* one level-3 table */
#define B2L_GPU_MMU_PA_LIMIT           (1ull << 48)

#define B2L_GPU_MMU_PTE_TYPE_TABLE     1ull
#define B2L_GPU_MMU_PTE_TYPE_PAGE      3ull
#define B2L_GPU_MMU_PTE_AF             (1ull << 10)
#define B2L_GPU_MMU_PTE_RW             (1ull << 6)
#define B2L_GPU_MMU_PTE_RO             (3ull << 6)
#define B2L_GPU_MMU_PTE_NX             (1ull << 54)

/* Attribute indices, from AS_MEMATTR_INDEX_* in mali_midg_regmap.h. */
#define B2L_GPU_MMU_MEMATTR_IMPL_DEF   0u
#define B2L_GPU_MMU_MEMATTR_NON_CACHE  5u

/* AS_MEMATTR_LO/HI for a job-only address space: implementation-defined
 * inner/outer caching everywhere, plus one non-cacheable slot used for the
 * job chain and its data so no CPU cache maintenance is required. */
#define B2L_GPU_AS_MEMATTR_LO          0x888D8F88u
#define B2L_GPU_AS_MEMATTR_HI          0x00004C8Du

/* AS_TRANSCFG: AArch64 4 KiB mode. kbase's b_r19p0 mmu_hw_configure starts
 * from mmu_get_as_setup's AS_TRANSCFG_ADRMODE_AARCH64_4K and then adds
 * PTW_MEMATTR_WRITE_BACK and R_ALLOCATE (plus PTW_SH_OS when the system is
 * ACE-coherent). This port has no coherency certificate and builds its page
 * tables through uncached aliases, so it asks for non-cacheable page-table
 * walks instead: the walker then always reads the tables from DRAM and no
 * GPU-side page-table cache can hold a stale entry. */
#define B2L_GPU_AS_TRANSCFG_AARCH64_4K 6u
#define B2L_GPU_AS_TRANSCFG_PTW_NC     (1ull << 24)
#define B2L_GPU_AS_TRANSCFG_VALUE \
	((uint32_t)(B2L_GPU_AS_TRANSCFG_AARCH64_4K | B2L_GPU_AS_TRANSCFG_PTW_NC))

static inline uint64_t
b2l_gpu_mmu_table_entry(uint64_t phys)
{
	return (phys & ~(B2L_GPU_MMU_PAGE_SIZE - 1)) |
	    B2L_GPU_MMU_PTE_AF | B2L_GPU_MMU_PTE_TYPE_TABLE;
}

static inline uint64_t
b2l_gpu_mmu_page_entry(uint64_t phys, unsigned memattr)
{
	return (phys & ~(B2L_GPU_MMU_PAGE_SIZE - 1)) |
	    ((uint64_t)(memattr & 7u) << 2) |
	    B2L_GPU_MMU_PTE_AF | B2L_GPU_MMU_PTE_RW |
	    B2L_GPU_MMU_PTE_TYPE_PAGE;
}

/* Level 0 consumes VA[47:39], level 3 consumes VA[20:12]. */
static inline unsigned
b2l_gpu_mmu_index(uint64_t va, unsigned level)
{
	return (unsigned)((va >> (39u - 9u * level)) & 0x1ffu);
}

/* Fill the four tables for count consecutive 4 KiB pages. The whole range
 * must sit inside one 2 MiB region because one level-3 table is supplied;
 * anything else is rejected without writing a single entry. */
static inline int
b2l_gpu_mmu_map(uint64_t *const *tables, const uint64_t *phys,
    uint64_t va, uint64_t pa, unsigned count, unsigned memattr)
{
	if (tables == NULL || phys == NULL || count == 0 ||
	    count > B2L_GPU_MMU_ENTRIES_PER_TABLE) {
		return -1;
	}
	if ((va & (B2L_GPU_MMU_PAGE_SIZE - 1)) != 0 ||
	    (pa & (B2L_GPU_MMU_PAGE_SIZE - 1)) != 0) {
		return -1;
	}
	if (va >= B2L_GPU_MMU_PA_LIMIT || pa >= B2L_GPU_MMU_PA_LIMIT ||
	    pa + (uint64_t)count * B2L_GPU_MMU_PAGE_SIZE > B2L_GPU_MMU_PA_LIMIT) {
		return -1;
	}
	if ((va & (B2L_GPU_MMU_REGION_SIZE - 1)) +
	    (uint64_t)count * B2L_GPU_MMU_PAGE_SIZE > B2L_GPU_MMU_REGION_SIZE) {
		return -1;
	}
	for (unsigned level = 0; level < B2L_GPU_MMU_LEVELS; level++) {
		if (tables[level] == NULL) {
			return -1;
		}
	}
	tables[0][b2l_gpu_mmu_index(va, 0)] = b2l_gpu_mmu_table_entry(phys[1]);
	tables[1][b2l_gpu_mmu_index(va, 1)] = b2l_gpu_mmu_table_entry(phys[2]);
	tables[2][b2l_gpu_mmu_index(va, 2)] = b2l_gpu_mmu_table_entry(phys[3]);
	for (unsigned i = 0; i < count; i++) {
		uint64_t page_va = va + (uint64_t)i * B2L_GPU_MMU_PAGE_SIZE;
		uint64_t page_pa = pa + (uint64_t)i * B2L_GPU_MMU_PAGE_SIZE;
		tables[3][b2l_gpu_mmu_index(page_va, 3)] =
		    b2l_gpu_mmu_page_entry(page_pa, memattr);
	}
	return 0;
}
#endif /* B2L_GPU_H */

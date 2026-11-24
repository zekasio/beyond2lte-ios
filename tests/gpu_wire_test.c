/* SPDX-License-Identifier: BSD-2-Clause
 * Verify the exact bytes B2LGPU.cpp programs into the Mali-G76 job manager
 * and MMU, on the host, before any of it is executed on real hardware.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "B2LGPU.h"

static void
test_job_header(void)
{
	struct b2l_gpu_job_header h;
	uint8_t raw[B2L_GPU_JOB_HEADER_SIZE];

	assert(sizeof(h) == B2L_GPU_JOB_HEADER_SIZE);
	assert(offsetof(struct b2l_gpu_job_header, exception_status) == 0);
	assert(offsetof(struct b2l_gpu_job_header, first_incomplete_task) == 4);
	assert(offsetof(struct b2l_gpu_job_header, fault_pointer) == 8);
	assert(offsetof(struct b2l_gpu_job_header, size_and_type) == 16);
	assert(offsetof(struct b2l_gpu_job_header, barrier_and_flags) == 17);
	assert(offsetof(struct b2l_gpu_job_header, job_index) == 18);
	assert(offsetof(struct b2l_gpu_job_header, dependency_1) == 20);
	assert(offsetof(struct b2l_gpu_job_header, dependency_2) == 22);
	assert(offsetof(struct b2l_gpu_job_header, next_job) == 24);

	/* A terminating 64-bit NULL job: type 1 in bits 7:1, size bit set. */
	b2l_gpu_job_header_init(&h, B2L_GPU_JOB_NULL, 1, 0);
	assert(h.size_and_type == 0x03);
	assert(h.barrier_and_flags == 0);
	assert(h.job_index == 0 && h.dependency_1 == 0 && h.dependency_2 == 0);
	assert(h.next_job == 0);
	assert(b2l_gpu_job_header_type(&h) == B2L_GPU_JOB_NULL);

	/* A chained 32-bit fragment job keeps every other header field zero. */
	b2l_gpu_job_header_init(&h, B2L_GPU_JOB_FRAGMENT, 0, 0x1234567890abcdefull);
	assert(h.size_and_type == (uint8_t)(B2L_GPU_JOB_FRAGMENT << 1));
	assert(h.next_job == 0x1234567890abcdefull);
	assert(b2l_gpu_job_header_type(&h) == B2L_GPU_JOB_FRAGMENT);

	/* Byte layout is what the GPU reads, not just the struct in C. */
	b2l_gpu_job_header_init(&h, B2L_GPU_JOB_WRITE_VALUE, 1, 0);
	memcpy(raw, &h, sizeof(raw));
	assert(raw[0] == 0 && raw[16] == 0x05 && raw[17] == 0);
	for (unsigned i = 0; i < 8; i++) {
		assert(raw[24 + i] == 0);
	}

	/* The type field is seven bits wide, so the top bit is masked off. */
	b2l_gpu_job_header_init(&h, 0xff, 1, 0);
	assert(h.size_and_type == (uint8_t)(0xff | 1u));
}

static void
test_page_entries(void)
{
	/* Table entries ignore the low 12 bits of the pointer and set AF+type. */
	assert(b2l_gpu_mmu_table_entry(0x40100fff) == 0x40100401ull);
	assert(b2l_gpu_mmu_table_entry(0x0) == 0x401ull);
	/* Leaf entries add the attribute index in bits 4:2, RW and type 3. */
	assert(b2l_gpu_mmu_page_entry(0x50000abc, B2L_GPU_MMU_MEMATTR_NON_CACHE) ==
	    0x50000457ull);
	assert(b2l_gpu_mmu_page_entry(0x50000000, B2L_GPU_MMU_MEMATTR_IMPL_DEF) ==
	    0x50000443ull);
	/* The index field is three bits, so 9 is truncated to 1 (0x04). */
	assert((b2l_gpu_mmu_page_entry(0x50000000, 9) & 0x1cull) == 0x04ull);
}

static void
test_index_split(void)
{
	/* 0x0000_7f_ff_ff_e000 exercises the low end of every level, and
	 * 0xffff_ffff_f000 the top level-3 entry of the 48-bit space. */
	const uint64_t va = 0x00007ffffffff000ull;
	assert(b2l_gpu_mmu_index(va, 0) == 0x0ff);
	assert(b2l_gpu_mmu_index(va, 1) == 0x1ff);
	assert(b2l_gpu_mmu_index(va, 2) == 0x1ff);
	assert(b2l_gpu_mmu_index(va, 3) == 0x1ff);
	assert(b2l_gpu_mmu_index(0x20000000ull, 0) == 0);
	assert(b2l_gpu_mmu_index(0x20000000ull, 1) == 0);
	assert(b2l_gpu_mmu_index(0x20000000ull, 2) == 0x100);
	assert(b2l_gpu_mmu_index(0x20000000ull, 3) == 0);
}

static void
test_mapping(void)
{
	uint64_t l0[512], l1[512], l2[512], l3[512];
	uint64_t *tables[4] = { l0, l1, l2, l3 };
	const uint64_t phys[4] = { 0x40000000ull, 0x40100000ull,
	    0x40200000ull, 0x40300000ull };
	const uint64_t va = 0x20000000ull;
	const uint64_t pa = 0x50000000ull;

	memset(l0, 0, sizeof(l0));
	memset(l1, 0, sizeof(l1));
	memset(l2, 0, sizeof(l2));
	memset(l3, 0, sizeof(l3));

	assert(b2l_gpu_mmu_map(tables, phys, va, pa, 3,
	    B2L_GPU_MMU_MEMATTR_NON_CACHE) == 0);
	assert(l0[0] == 0x40100401ull);
	assert(l1[0] == 0x40200401ull);
	assert(l2[0x100] == 0x40300401ull);
	for (unsigned i = 0; i < 3; i++) {
		assert(l3[i] == ((pa + i * 4096ull) | 0x457ull));
	}
	assert(l3[3] == 0);

	/* A second range inside the same 2 MiB region reuses the walk tables. */
	assert(b2l_gpu_mmu_map(tables, phys, va + 0x1ff000ull, pa + 0x8000ull, 1,
	    B2L_GPU_MMU_MEMATTR_IMPL_DEF) == 0);
	assert(l3[0x1ff] == ((pa + 0x8000ull) | 0x443ull));

	/* Rejections must not modify the tables. */
	static const struct {
		uint64_t va, pa;
		unsigned count;
	} bad[] = {
		{ va, pa + 1, 1 },                      /* unaligned PA */
		{ va + 1, pa, 1 },                      /* unaligned VA */
		{ va, pa, 0 },                          /* empty range */
		{ va, pa, 513 },                        /* more than one L3 table */
		{ va + 0x1ff000ull, pa, 2 },            /* crosses the 2 MiB edge */
		{ va, 0xfffffffffffff000ull, 1 },       /* PA outside 48 bits */
		{ 0x1000000000000ull, pa, 1 },          /* VA outside 48 bits */
	};
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		uint64_t l3_copy[512];
		memcpy(l3_copy, l3, sizeof(l3_copy));
		assert(b2l_gpu_mmu_map(tables, phys, bad[i].va, bad[i].pa,
		    bad[i].count, B2L_GPU_MMU_MEMATTR_NON_CACHE) != 0);
		assert(memcmp(l3_copy, l3, sizeof(l3_copy)) == 0);
	}
	assert(b2l_gpu_mmu_map(NULL, phys, va, pa, 1, 0) != 0);
	assert(b2l_gpu_mmu_map(tables, NULL, va, pa, 1, 0) != 0);
	uint64_t *missing[4] = { l0, NULL, l2, l3 };
	assert(b2l_gpu_mmu_map(missing, phys, va, pa, 1, 0) != 0);
}

static void
test_address_space_constants(void)
{
	/* Each attribute index must select its documented byte lane. */
	static const uint32_t lanes[4] = { 0x88u, 0x8Fu, 0x8Du, 0x88u };
	for (unsigned i = 0; i < 4; i++) {
		assert(((B2L_GPU_AS_MEMATTR_LO >> (8 * i)) & 0xffu) == lanes[i]);
	}
	assert(((B2L_GPU_AS_MEMATTR_HI >> 0) & 0xffu) == 0x8Du);
	assert(((B2L_GPU_AS_MEMATTR_HI >> 8) & 0xffu) == 0x4Cu);
	/* The non-cacheable lane is the one the job chain uses. */
	assert(((B2L_GPU_AS_MEMATTR_HI >> (8 * (B2L_GPU_MMU_MEMATTR_NON_CACHE - 4))) &
	    0xffu) == 0x4Cu);

	/* AArch64 4 KiB address mode, page-table walks uncached: bits 3:0 hold
	 * the mode, bits 25:24 hold PTW_MEMATTR = 1 (non-cacheable). */
	assert((B2L_GPU_AS_TRANSCFG_VALUE & 0xfu) ==
	    B2L_GPU_AS_TRANSCFG_AARCH64_4K);
	assert(((B2L_GPU_AS_TRANSCFG_VALUE >> 24) & 0x3u) == 1u);
	assert(B2L_GPU_AS_TRANSCFG_VALUE == 0x01000006u);
}

int
main(void)
{
	test_job_header();
	test_page_entries();
	test_index_split();
	test_mapping();
	test_address_space_constants();
	puts("PASS: Mali job header, AArch64 page-table entries, 48-bit index "
	    "split and address-space constants match the b_r19p0 encodings "
	    "(ASan/UBSan)");
	return 0;
}

/* SPDX-License-Identifier: MIT
 * B2L APFS bring-up: bounded, read-only container probe.
 *
 * This is not yet a VFS filesystem driver. It validates an NXSB container
 * superblock only; it does not resolve checkpoints, OMAPs, volumes or files.
 */
#ifndef B2L_APFS_PROBE_H
#define B2L_APFS_PROBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define B2L_APFS_NXSB_MIN_BLOCK_SIZE 4096u
#define B2L_APFS_NXSB_MAX_BLOCK_SIZE 65536u

typedef int (*b2l_apfs_read_fn)(void *context, uint64_t offset,
    void *buffer, size_t length);

enum b2l_apfs_probe_result {
    B2L_APFS_PROBE_OK = 0,
    B2L_APFS_PROBE_EIO = -1,
    B2L_APFS_PROBE_EBUFFER = -2,
    B2L_APFS_PROBE_EMAGIC = -3,
    B2L_APFS_PROBE_EBLOCKSIZE = -4,
    B2L_APFS_PROBE_EBOUNDS = -5,
    B2L_APFS_PROBE_ECHECKSUM = -6,
    B2L_APFS_PROBE_ETYPE = -7
};

struct b2l_apfs_container_info {
    uint32_t block_size;
    uint64_t block_count;
    uint64_t compatible_features;
    uint64_t readonly_compatible_features;
    uint64_t incompatible_features;
    uint8_t uuid[16];
};

/*
 * Probe an APFS container at device offset zero.
 *
 * scratch must be at least B2L_APFS_NXSB_MAX_BLOCK_SIZE bytes. device_size
 * is the exact readable extent of the candidate partition. The callback
 * must perform bounded reads and return zero only after filling the buffer.
 */
int b2l_apfs_probe_container(b2l_apfs_read_fn read_fn, void *context,
    uint64_t device_size, void *scratch, size_t scratch_size,
    struct b2l_apfs_container_info *info);

#ifdef __cplusplus
}
#endif
#endif

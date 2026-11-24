/* SPDX-License-Identifier: MIT
 * B2L APFS bring-up: read-only NXSB validation.
 */
#include "b2l_apfs_probe.h"

#include <string.h>

static uint32_t
load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
load_le64(const uint8_t *p)
{
    return (uint64_t)load_le32(p) | ((uint64_t)load_le32(p + 4) << 32);
}

/* APFS Fletcher-64: 32-bit words, modulus 2^32-1, checksum field zeroed. */
static uint64_t
fletcher64_apfs(const uint8_t *block, size_t length)
{
    uint64_t sum1 = 0, sum2 = 0;
    const uint64_t modulus = 0xffffffffULL;
    size_t offset;

    for (offset = 0; offset < length; offset += 4) {
        uint32_t word = (offset < 8) ? 0 : load_le32(block + offset);
        sum1 = (sum1 + word) % modulus;
        sum2 = (sum2 + sum1) % modulus;
    }
    /* APFS stores the complemented Fletcher result, not the raw sums. */
    {
        uint64_t lower = 0xffffffffULL - ((sum1 + sum2) % modulus);
        uint64_t upper = 0xffffffffULL - ((sum1 + lower) % modulus);
        return (upper << 32) | lower;
    }
}

int
b2l_apfs_probe_container(b2l_apfs_read_fn read_fn, void *context,
    uint64_t device_size, void *scratch, size_t scratch_size,
    struct b2l_apfs_container_info *info)
{
    uint8_t *block = (uint8_t *)scratch;
    uint32_t block_size, object_type;
    uint64_t block_count, required_size;

    if (read_fn == NULL || block == NULL || info == NULL)
        return B2L_APFS_PROBE_EBUFFER;
    if (scratch_size < B2L_APFS_NXSB_MAX_BLOCK_SIZE ||
        device_size < B2L_APFS_NXSB_MIN_BLOCK_SIZE)
        return B2L_APFS_PROBE_EBUFFER;
    if (read_fn(context, 0, block, B2L_APFS_NXSB_MIN_BLOCK_SIZE) != 0)
        return B2L_APFS_PROBE_EIO;

    if (memcmp(block + 32, "NXSB", 4) != 0)
        return B2L_APFS_PROBE_EMAGIC;

    block_size = load_le32(block + 36);
    block_count = load_le64(block + 40);
    if (block_size < B2L_APFS_NXSB_MIN_BLOCK_SIZE ||
        block_size > B2L_APFS_NXSB_MAX_BLOCK_SIZE ||
        (block_size & (block_size - 1)) != 0)
        return B2L_APFS_PROBE_EBLOCKSIZE;
    if (block_size > scratch_size || block_count == 0 ||
        block_count > UINT64_MAX / block_size)
        return B2L_APFS_PROBE_EBOUNDS;

    required_size = block_count * (uint64_t)block_size;
    if (required_size > device_size)
        return B2L_APFS_PROBE_EBOUNDS;

    if (read_fn(context, 0, block, block_size) != 0)
        return B2L_APFS_PROBE_EIO;

    object_type = load_le32(block + 24);
    if ((object_type & 0xffffu) != 1u)
        return B2L_APFS_PROBE_ETYPE;
    if (load_le64(block) != fletcher64_apfs(block, block_size))
        return B2L_APFS_PROBE_ECHECKSUM;

    memset(info, 0, sizeof(*info));
    info->block_size = block_size;
    info->block_count = block_count;
    info->compatible_features = load_le64(block + 48);
    info->readonly_compatible_features = load_le64(block + 56);
    info->incompatible_features = load_le64(block + 64);
    memcpy(info->uuid, block + 72, sizeof(info->uuid));
    return B2L_APFS_PROBE_OK;
}

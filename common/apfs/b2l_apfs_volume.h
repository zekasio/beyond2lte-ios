/* SPDX-License-Identifier: MIT
 * Bounded, read-only APFS volume superblock (APSB) decoder.
 */
#ifndef B2L_APFS_VOLUME_H
#define B2L_APFS_VOLUME_H
#include <stddef.h>
#include <stdint.h>
enum b2l_apfs_volume_result {
 B2L_APFS_VOLUME_OK=0, B2L_APFS_VOLUME_EINVAL=-60,
 B2L_APFS_VOLUME_ECHECKSUM=-61, B2L_APFS_VOLUME_ETYPE=-62,
 B2L_APFS_VOLUME_EBOUNDS=-63
};
struct b2l_apfs_volume_info {
 uint64_t oid,xid,features,readonly_compatible_features,incompatible_features;
 uint64_t omap_oid,root_tree_oid,extentref_tree_oid,snap_meta_tree_oid;
 uint64_t fs_flags;
 uint32_t fs_index,root_tree_type,extentref_tree_type,snap_meta_tree_type;
 uint8_t uuid[16];
};
/* block_size is the complete APFS allocation block; checksum covers it all. */
int b2l_apfs_decode_volume(const void *block,size_t block_size,
    struct b2l_apfs_volume_info *out);
#endif

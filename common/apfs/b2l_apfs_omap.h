/* SPDX-License-Identifier: MIT
 * APFS object-map structures and bounded fixed-KV tree lookup.
 */
#ifndef B2L_APFS_OMAP_H
#define B2L_APFS_OMAP_H
#include <stddef.h>
#include <stdint.h>
#include "b2l_apfs_probe.h"
#include "b2l_apfs_volume.h"
enum b2l_apfs_omap_result {
 B2L_APFS_OMAP_OK=0, B2L_APFS_OMAP_EINVAL=-40,
 B2L_APFS_OMAP_ECHECKSUM=-41, B2L_APFS_OMAP_ETYPE=-42,
 B2L_APFS_OMAP_EBOUNDS=-43, B2L_APFS_OMAP_EDELETED=-44
};
struct b2l_apfs_omap_info {
 uint64_t oid,xid,tree_oid,snapshot_tree_oid,most_recent_snapshot;
 uint32_t flags,snapshot_count,tree_type,snapshot_tree_type;
};
struct b2l_apfs_omap_key { uint64_t oid,xid; };
struct b2l_apfs_omap_value { uint32_t flags,size; uint64_t paddr; };
int b2l_apfs_decode_omap(const void *block,size_t block_size,
    struct b2l_apfs_omap_info *out);
int b2l_apfs_decode_omap_key(const void *bytes,size_t length,
    struct b2l_apfs_omap_key *out);
int b2l_apfs_decode_omap_value(const void *bytes,size_t length,
    struct b2l_apfs_omap_value *out);
/* Read-only fixed-KV OMAP B-tree lookup. root_paddr must be the resolved
 * physical address of the B-tree root. max_xid selects the newest mapping
 * whose key XID is <= max_xid. Traversal is bounded to 32 nodes.
 */
int b2l_apfs_resolve_container_oid(b2l_apfs_read_fn read_fn,void *context,
    uint64_t device_size,uint32_t block_size,uint64_t block_count,
    uint64_t container_omap_paddr,uint64_t target_oid,uint64_t max_xid,
    void *omap_scratch,size_t omap_scratch_size,
    void *tree_scratch,size_t tree_scratch_size,
    struct b2l_apfs_omap_key *found_key,struct b2l_apfs_omap_value *found_value);
/* Resolve a volume FSTREE virtual OID through its physical volume OMAP.
 * volume_omap_paddr is volume.omap_oid for a live volume. For a snapshot
 * whose APSB has omap_oid == 0, pass the live volume OMAP block address;
 * lookup still uses the snapshot superblock XID.
 */
int b2l_apfs_resolve_volume_root(b2l_apfs_read_fn read_fn,void *context,
    uint64_t device_size,uint32_t block_size,uint64_t block_count,
    const struct b2l_apfs_volume_info *volume,uint64_t volume_omap_paddr,
    void *omap_scratch,size_t omap_scratch_size,
    void *tree_scratch,size_t tree_scratch_size,
    struct b2l_apfs_omap_key *found_key,struct b2l_apfs_omap_value *found_value);
int b2l_apfs_omap_lookup(b2l_apfs_read_fn read_fn,void *context,
    uint64_t device_size,uint32_t block_size,uint64_t block_count,
    uint64_t root_paddr,uint64_t target_oid,uint64_t max_xid,
    void *scratch,size_t scratch_size,struct b2l_apfs_omap_key *found_key,
    struct b2l_apfs_omap_value *found_value);
#endif

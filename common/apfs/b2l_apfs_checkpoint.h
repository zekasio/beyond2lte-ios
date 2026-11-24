/* SPDX-License-Identifier: MIT
 * B2L APFS checkpoint descriptor-area discovery (read-only).
 */
#ifndef B2L_APFS_CHECKPOINT_H
#define B2L_APFS_CHECKPOINT_H
#include "b2l_apfs_probe.h"
#define B2L_APFS_CHECKPOINT_MAP_TYPE 0x0000000cu
#define B2L_APFS_CHECKPOINT_MAP_LAST 0x00000001u
#define B2L_APFS_DESC_BLOCKS_FRAGMENTED 0x80000000u
#define B2L_APFS_DESC_BLOCKS_MASK 0x7fffffffu
#define B2L_APFS_MAX_CHECKPOINT_MAPS 64u
enum b2l_apfs_checkpoint_result {
 B2L_APFS_CKPT_OK=0, B2L_APFS_CKPT_EINVAL=-20, B2L_APFS_CKPT_EIO=-21,
 B2L_APFS_CKPT_EBOUNDS=-22, B2L_APFS_CKPT_ECHECKSUM=-23,
 B2L_APFS_CKPT_EFORMAT=-24, B2L_APFS_CKPT_ENOTFOUND=-25,
 B2L_APFS_CKPT_EUNSUPPORTED=-26
};
struct b2l_apfs_checkpoint_info {
 uint64_t xid,nxsb_paddr,container_omap_paddr,spaceman_oid,reaper_oid;
 uint32_t descriptor_blocks,checkpoint_map_blocks,mapping_entries,flags;
 uint64_t checkpoint_map_paddrs[B2L_APFS_MAX_CHECKPOINT_MAPS];
 uint64_t volume_oids[100]; uint32_t volume_count;
};
/* Discovers the newest NXSB that follows a complete same-XID checkpoint map.
 * Only contiguous descriptor rings and a single-block checkpoint map are
 * currently supported. Fragmented rings and map chains fail closed.
 */
int b2l_apfs_discover_checkpoint(b2l_apfs_read_fn,void *,uint64_t,
 const void *,size_t,void *,size_t,struct b2l_apfs_checkpoint_info *);
int b2l_apfs_validate_checkpoint_map(const void *,size_t,uint64_t,
 uint32_t *,uint32_t *);
/* Resolve a container metadata OID through the selected checkpoint-map chain.
 * fs_oid is normally zero for container-level objects; expected_type is the
 * low 16-bit APFS object type, or zero to accept any type.
 */
int b2l_apfs_resolve_checkpoint_oid(b2l_apfs_read_fn,void *,uint64_t,
 uint32_t,uint64_t,const struct b2l_apfs_checkpoint_info *,uint64_t,
 uint64_t,uint32_t,void *,size_t,uint64_t *,uint32_t *);
#endif

/* SPDX-License-Identifier: MIT
 * Bounded read-only APFS FSTREE inode lookup.
 */
#ifndef B2L_APFS_FSTREE_H
#define B2L_APFS_FSTREE_H
#include <stddef.h>
#include <stdint.h>
#include "b2l_apfs_omap.h"
#include "b2l_apfs_volume.h"

enum b2l_apfs_fstree_result {
 B2L_APFS_FSTREE_OK=0, B2L_APFS_FSTREE_EINVAL=-70,
 B2L_APFS_FSTREE_EIO=-71, B2L_APFS_FSTREE_ECHECKSUM=-72,
 B2L_APFS_FSTREE_ETYPE=-73, B2L_APFS_FSTREE_EBOUNDS=-74,
 B2L_APFS_FSTREE_ENOTFOUND=-75, B2L_APFS_FSTREE_EUNSUPPORTED=-76
};

/* Fixed portion of j_inode_val_t (92 bytes). Extended fields are not
 * interpreted yet; value_size reports their total containing record size.
 */
struct b2l_apfs_inode_info {
 uint64_t oid,xid,parent_id,private_id;
 uint64_t create_time,mod_time,change_time,access_time,internal_flags;
 uint32_t nchildren,owner,gid;
 uint16_t mode;
 uint32_t value_size;
};

/* Finds an exact APFS_TYPE_INODE key in a virtual per-volume FSTREE.
 * volume_omap_paddr is the physical omap_phys block address. For snapshots
 * with volume.omap_oid == 0, pass the live volume's OMAP address; the
 * snapshot's volume.xid selects the historical mapping.
 *
 * omap_scratch and node_scratch must each hold one complete APFS block.
 * The traversal is read-only, cycle-checked and limited to 32 levels.
 */
int b2l_apfs_fstree_lookup_inode(b2l_apfs_read_fn read_fn,void *context,
 uint64_t device_size,uint32_t block_size,uint64_t block_count,
 const struct b2l_apfs_volume_info *volume,uint64_t volume_omap_paddr,
 uint64_t inode_oid,void *omap_scratch,size_t omap_scratch_size,
 void *node_scratch,size_t node_scratch_size,
 struct b2l_apfs_inode_info *out);
#endif

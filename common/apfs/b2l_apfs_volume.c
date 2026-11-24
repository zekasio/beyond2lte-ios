/* SPDX-License-Identifier: MIT */
#include "b2l_apfs_volume.h"
static uint32_t r32(const uint8_t *p) {
 return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint64_t r64(const uint8_t *p) {
 return (uint64_t)r32(p)|((uint64_t)r32(p+4)<<32);
}
static uint64_t fletcher(const uint8_t *p,size_t n) {
 const uint64_t m=0xffffffffULL; uint64_t a=0,b=0,lo,hi; size_t i;
 for(i=8;i<n;i+=4) { a=(a+r32(p+i))%m; b=(b+a)%m; }
 lo=0xffffffffULL-((a+b)%m); hi=0xffffffffULL-((a+lo)%m);
 return (hi<<32)|lo;
}
int b2l_apfs_decode_volume(const void *data,size_t n,
 struct b2l_apfs_volume_info *o) {
 const uint8_t *p=(const uint8_t *)data; uint32_t type; int is_snapshot;
 if(!p||!o||n<4096u||n>65536u||(n&(n-1u))||(n&3u))
  return B2L_APFS_VOLUME_EINVAL;
 type=r32(p+24);
 /* Regular APSB objects are physical (0x0000000d); snapshot superblocks
  * are ephemeral (0x4000000d). Virtual APSB objects are not accepted. */
 if((type&0xffffu)!=0x0du||
    (type&0xbfff0000u)!=0u||r32(p+28)!=0u)
  return B2L_APFS_VOLUME_ETYPE;
 is_snapshot=(type&0x40000000u)!=0;
 if(r64(p)!=fletcher(p,n)) return B2L_APFS_VOLUME_ECHECKSUM;
 if(r32(p+32)!=0x42535041u) return B2L_APFS_VOLUME_ETYPE;
 o->oid=r64(p+8); o->xid=r64(p+16); o->fs_index=r32(p+36);
 o->features=r64(p+40); o->readonly_compatible_features=r64(p+48);
 o->incompatible_features=r64(p+56);
 o->root_tree_type=r32(p+116); o->extentref_tree_type=r32(p+120);
 o->snap_meta_tree_type=r32(p+124);
 o->omap_oid=r64(p+128); o->root_tree_oid=r64(p+136);
 o->extentref_tree_oid=r64(p+144); o->snap_meta_tree_oid=r64(p+152);
 for(size_t i=0;i<16;i++) o->uuid[i]=p[240+i];
 o->fs_flags=r64(p+264);
 if(!o->oid||!o->xid||(!is_snapshot&&!o->omap_oid)||
    !o->root_tree_oid||(o->root_tree_type&0xffffu)!=2u)
  return B2L_APFS_VOLUME_EBOUNDS;
 return B2L_APFS_VOLUME_OK;
}

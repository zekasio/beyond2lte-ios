/* SPDX-License-Identifier: MIT */
#include "../../common/apfs/b2l_apfs_volume.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BS 4096u
static uint32_t r32(const uint8_t *p) {
 return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void w32(uint8_t *p,uint32_t v) {
 p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);
}
static void w64(uint8_t *p,uint64_t v) {
 w32(p,(uint32_t)v);w32(p+4,(uint32_t)(v>>32));
}
static uint64_t sum(const uint8_t *p,size_t n) {
 uint64_t a=0,b=0,l,h;size_t i;
 for(i=8;i<n;i+=4){a=(a+r32(p+i))%0xffffffffULL;b=(b+a)%0xffffffffULL;}
 l=0xffffffffULL-((a+b)%0xffffffffULL);
 h=0xffffffffULL-((a+l)%0xffffffffULL);return(h<<32)|l;
}
static void seal(uint8_t *p){w64(p,sum(p,BS));}
static void make_volume(uint8_t *p,uint32_t type) {
 memset(p,0,BS);w64(p+8,44);w64(p+16,12);w32(p+24,type);
 w32(p+32,0x42535041);w32(p+36,1);w32(p+116,0x80000002);
 w32(p+120,0x40000002);w32(p+124,0x40000002);
 w64(p+128,55);w64(p+136,66);w64(p+144,77);w64(p+152,88);
 for(size_t i=0;i<16;i++)p[240+i]=(uint8_t)(i+1);
 w64(p+256,0x1122334455667788ULL);w64(p+264,3);seal(p);
}
int main(void) {
 uint8_t block[BS];struct b2l_apfs_volume_info v;
 make_volume(block,0x0000000d);
 assert(b2l_apfs_decode_volume(block,sizeof(block),&v)==B2L_APFS_VOLUME_OK);
 assert(v.oid==44&&v.xid==12&&v.fs_index==1);
 assert(v.omap_oid==55&&v.root_tree_oid==66);
 assert(v.extentref_tree_oid==77&&v.snap_meta_tree_oid==88);
 assert(v.uuid[0]==1&&v.uuid[15]==16&&v.fs_flags==3);
 assert(b2l_apfs_decode_volume(block,1056,&v)==B2L_APFS_VOLUME_EINVAL);
 block[32]^=1;
 assert(b2l_apfs_decode_volume(block,sizeof(block),&v)==B2L_APFS_VOLUME_ECHECKSUM);
 make_volume(block,0x0000000d);w32(block+32,0);seal(block);
 assert(b2l_apfs_decode_volume(block,sizeof(block),&v)==B2L_APFS_VOLUME_ETYPE);
 make_volume(block,0x0000000b);seal(block);
 assert(b2l_apfs_decode_volume(block,sizeof(block),&v)==B2L_APFS_VOLUME_ETYPE);
 make_volume(block,0x0000000d);w64(block+128,0);seal(block);
 assert(b2l_apfs_decode_volume(block,sizeof(block),&v)==B2L_APFS_VOLUME_EBOUNDS);
 /* Snapshot superblocks are ephemeral and may borrow the live volume OMAP. */
 make_volume(block,0x4000000d);w64(block+128,0);seal(block);
 assert(b2l_apfs_decode_volume(block,sizeof(block),&v)==B2L_APFS_VOLUME_OK);
 assert(v.omap_oid==0&&v.fs_flags==3);
 puts("APFS volume superblock tests passed");return 0;
}

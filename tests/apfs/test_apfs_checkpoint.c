/* SPDX-License-Identifier: MIT */
#include "../../common/apfs/b2l_apfs_checkpoint.h"
#include "../../common/apfs/b2l_apfs_omap.h"
#include "../../common/apfs/b2l_apfs_volume.h"
#include "../../common/apfs/b2l_apfs_fstree.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BS 4096u
#define BLOCKS 12u
static uint8_t disk[BS*BLOCKS];
static uint32_t r32(const uint8_t*p){return(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void w16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void w32(uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static void w64(uint8_t*p,uint64_t v){w32(p,(uint32_t)v);w32(p+4,(uint32_t)(v>>32));}
static uint64_t sum(const uint8_t*p,size_t n){uint64_t a=0,b=0,l,h;for(size_t i=8;i<n;i+=4){a=(a+r32(p+i))%0xffffffffULL;b=(b+a)%0xffffffffULL;}l=0xffffffffULL-((a+b)%0xffffffffULL);h=0xffffffffULL-((a+l)%0xffffffffULL);return(h<<32)|l;}
static void seal(uint8_t*p){w64(p,sum(p,BS));}
static int rd(void*c,uint64_t o,void*d,size_t n){(void)c;if(o>sizeof(disk)||n>sizeof(disk)-(size_t)o)return-1;memcpy(d,disk+(size_t)o,n);return 0;}
static void make_nx(uint8_t*p){memset(p,0,BS);w64(p+8,1);w64(p+16,7);w32(p+24,0x80000001u);memcpy(p+32,"NXSB",4);w32(p+36,BS);w64(p+40,BLOCKS);w32(p+104,4);w64(p+112,1);w32(p+136,0);w32(p+140,3);w64(p+152,10);w64(p+160,3);w64(p+168,11);w32(p+180,1);w64(p+184,99);seal(p);}
static void make_map(uint8_t*p){memset(p,0,BS);w64(p+8,2);w64(p+16,7);w32(p+24,0x4000000c);w32(p+32,1);w32(p+36,1);w32(p+40,0x4000000b);w32(p+48,BS);w64(p+64,20);w64(p+72,3);seal(p);}
int main(void){uint8_t nx[BS],scratch[BS];struct b2l_apfs_checkpoint_info ci;struct b2l_apfs_omap_info oi;struct b2l_apfs_omap_key k;struct b2l_apfs_omap_value v;
 memset(disk,0,sizeof(disk));make_nx(nx);make_map(disk+BS);memcpy(disk+2*BS,nx,BS);
 assert(b2l_apfs_validate_checkpoint_map(disk+BS,BS,7,&ci.flags,&ci.mapping_entries)==0);
 assert(b2l_apfs_discover_checkpoint(rd,NULL,sizeof(disk),nx,BS,scratch,BS,&ci)==0);
 assert(ci.xid==7&&ci.nxsb_paddr==2&&ci.container_omap_paddr==3&&ci.volume_oids[0]==99);
 {uint64_t resolved=0;uint32_t bytes=0;
  assert(b2l_apfs_resolve_checkpoint_oid(rd,NULL,sizeof(disk),BS,BLOCKS,&ci,
      0,20,0x0b,scratch,sizeof(scratch),&resolved,&bytes)==B2L_APFS_CKPT_OK);
  assert(resolved==3&&bytes==BS);
 }
 /* Fragmented descriptor ring: B-tree maps logical slots 0..2 to blocks 1..3. */
 {uint8_t *b=disk+6*BS;memset(b,0,BS);w64(b+8,6);w64(b+16,7);w32(b+24,0x40000002);
  w32(b+32,7);w32(b+36,1);w16(b+40,0);w16(b+42,4);w16(b+56,0);w16(b+58,16);w64(b+60,0);
  w64(b+4040,1);w64(b+4048,3);seal(b);
  w32(nx+104,0x80000004u);w64(nx+112,6);w32(nx+136,0);w32(nx+140,3);seal(nx);
  assert(b2l_apfs_discover_checkpoint(rd,NULL,sizeof(disk),nx,BS,scratch,BS,&ci)==0);
  assert(ci.xid==7&&ci.nxsb_paddr==2);
 }
 disk[BS+200]^=1;assert(b2l_apfs_validate_checkpoint_map(disk+BS,BS,7,&ci.flags,&ci.mapping_entries)==B2L_APFS_CKPT_ECHECKSUM);
 memset(nx,0,BS);w64(nx+8,1);w64(nx+16,1);w32(nx+24,0x4000000b);w32(nx+40,0x40000002);w64(nx+48,42);seal(nx);
 assert(b2l_apfs_decode_omap(nx,BS,&oi)==0&&oi.tree_oid==42);
 {uint8_t key[16]={0},val[16]={0};w64(key,9);w64(key+8,3);w32(val+4,BS);w64(val+8,5);assert(b2l_apfs_decode_omap_key(key,16,&k)==0&&k.oid==9&&k.xid==3);assert(b2l_apfs_decode_omap_value(val,16,&v)==0&&v.paddr==5);}
 /* Fixed-KV root leaf: one OID/XID -> physical extent mapping. */
 {uint8_t *b=disk+4*BS;memset(b,0,BS);w64(b+8,4);w64(b+16,7);w32(b+24,0x40000002);
  w32(b+32,7);w32(b+36,1);w32(b+40,0);w32(b+42,4);
  w16(b+56,0);w16(b+58,16);w64(b+60,9);w64(b+68,3);
  w32(b+4040,0);w32(b+4044,BS);w64(b+4048,5);seal(b);
  assert(b2l_apfs_omap_lookup(rd,NULL,sizeof(disk),BS,BLOCKS,4,9,10,
      scratch,sizeof(scratch),&k,&v)==B2L_APFS_OMAP_OK);
  assert(k.oid==9&&k.xid==3&&v.paddr==5&&v.size==BS);
 }
 /* Container OMAP object -> OMAP B-tree -> physical object mapping. */
 {uint8_t *o=disk+3*BS;memset(o,0,BS);w64(o+8,3);w64(o+16,7);w32(o+24,0x4000000b);
  w32(o+40,0x40000002);w64(o+48,4);seal(o);
  assert(b2l_apfs_resolve_container_oid(rd,NULL,sizeof(disk),BS,BLOCKS,3,9,10,
      scratch,sizeof(scratch),scratch+BS/2,BS/2,&k,&v)==B2L_APFS_OMAP_EINVAL);
  /* Use distinct full-block scratch buffers for the OMAP and tree read. */
  {uint8_t omap_scratch[BS],tree_scratch[BS];
   assert(b2l_apfs_resolve_container_oid(rd,NULL,sizeof(disk),BS,BLOCKS,3,9,10,
       omap_scratch,sizeof(omap_scratch),tree_scratch,sizeof(tree_scratch),&k,&v)==B2L_APFS_OMAP_OK);
   assert(k.oid==9&&v.paddr==5);
  }
 }
 /* Internal root branch descends to a non-root leaf node. */
 {uint8_t *root=disk+7*BS,*leaf=disk+5*BS;
  memset(root,0,BS);w64(root+8,7);w64(root+16,7);w32(root+24,0x40000002);
  w32(root+32,5);w32(root+36,1);w16(root+40,0);w16(root+42,4);w16(root+56,0);w16(root+58,8);
  w64(root+60,9);w64(root+68,1);w64(root+4048,5);seal(root);
  memset(leaf,0,BS);w64(leaf+8,5);w64(leaf+16,7);w32(leaf+24,0x40000003);
  w32(leaf+32,6);w32(leaf+36,1);w16(leaf+40,0);w16(leaf+42,4);w16(leaf+56,0);w16(leaf+58,16);
  w64(leaf+60,9);w64(leaf+68,3);w32(leaf+4080,0);w32(leaf+4084,BS);w64(leaf+4088,5);seal(leaf);
  assert(b2l_apfs_omap_lookup(rd,NULL,sizeof(disk),BS,BLOCKS,7,9,10,
      scratch,sizeof(scratch),&k,&v)==B2L_APFS_OMAP_OK);
  assert(k.oid==9&&k.xid==3&&v.paddr==5);
 }

 /* Volume OMAP: resolve the virtual FSTREE root at the volume XID. */
 {uint8_t *o=disk+8*BS,*tree=disk+9*BS;
  struct b2l_apfs_volume_info vol; uint8_t omap_scratch[BS],tree_scratch[BS];
  memset(o,0,BS);w64(o+8,8);w64(o+16,12);w32(o+24,0x4000000b);
  w32(o+40,0x40000002);w64(o+48,9);seal(o);
  memset(tree,0,BS);w64(tree+8,9);w64(tree+16,12);w32(tree+24,0x40000002);
  w32(tree+32,7);w32(tree+36,1);w16(tree+40,0);w16(tree+42,4);
  w16(tree+56,0);w16(tree+58,16);w64(tree+60,66);w64(tree+68,3);
  w32(tree+4040,0);w32(tree+4044,BS);w64(tree+4048,10);seal(tree);
  memset(&vol,0,sizeof(vol));vol.xid=12;vol.omap_oid=8;vol.root_tree_oid=66;
  assert(b2l_apfs_resolve_volume_root(rd,NULL,sizeof(disk),BS,BLOCKS,&vol,
      vol.omap_oid,omap_scratch,sizeof(omap_scratch),tree_scratch,
      sizeof(tree_scratch),&k,&v)==B2L_APFS_OMAP_OK);
  assert(k.oid==66&&k.xid==3&&v.paddr==10);
  /* Snapshot APSB has no own OMAP: borrow the live OMAP but use snapshot XID. */
  vol.omap_oid=0;vol.xid=2;
  assert(b2l_apfs_resolve_volume_root(rd,NULL,sizeof(disk),BS,BLOCKS,&vol,
      8,omap_scratch,sizeof(omap_scratch),tree_scratch,
      sizeof(tree_scratch),&k,&v)==B2L_APFS_OMAP_EBOUNDS);
  vol.xid=12;
  assert(b2l_apfs_resolve_volume_root(rd,NULL,sizeof(disk),BS,BLOCKS,&vol,
      8,omap_scratch,sizeof(omap_scratch),tree_scratch,
      sizeof(tree_scratch),&k,&v)==B2L_APFS_OMAP_OK);
 }

 /* FSTREE variable-KV root leaf: find and decode the root inode (OID 2). */
 {uint8_t *fs=disk+10*BS;struct b2l_apfs_volume_info vol;
  struct b2l_apfs_inode_info inode;uint8_t omap_scratch[BS],node_scratch[BS];
  memset(fs,0,BS);w64(fs+8,66);w64(fs+16,12);
  w32(fs+24,0x80000002u);w32(fs+28,0x0eu);
  w16(fs+32,3);w16(fs+34,0);w32(fs+36,1);
  w16(fs+40,0);w16(fs+42,8);
  w16(fs+56,0);w16(fs+58,8);w16(fs+60,92);w16(fs+62,92);
  w64(fs+64,(3ULL<<60)|2ULL);
  w64(fs+BS-40-92,1);w64(fs+BS-40-92+8,0);
  w64(fs+BS-40-92+16,100);w64(fs+BS-40-92+24,200);
  w64(fs+BS-40-92+32,300);w64(fs+BS-40-92+40,400);
  w64(fs+BS-40-92+48,2);w32(fs+BS-40-92+56,0);
  w32(fs+BS-40-92+72,501);w32(fs+BS-40-92+76,20);
  w16(fs+BS-40-92+80,0x41ed);seal(fs);
  memset(&vol,0,sizeof(vol));vol.xid=12;vol.omap_oid=8;
  vol.root_tree_oid=66;vol.root_tree_type=0x80000002u;
  assert(b2l_apfs_fstree_lookup_inode(rd,NULL,sizeof(disk),BS,BLOCKS,
      &vol,8,2,omap_scratch,sizeof(omap_scratch),node_scratch,
      sizeof(node_scratch),&inode)==B2L_APFS_FSTREE_OK);
  assert(inode.oid==2&&inode.parent_id==1&&inode.mode==0x41ed);
  assert(inode.owner==501&&inode.gid==20&&inode.value_size==92);
  assert(b2l_apfs_fstree_lookup_inode(rd,NULL,sizeof(disk),BS,BLOCKS,
      &vol,8,3,omap_scratch,sizeof(omap_scratch),node_scratch,
      sizeof(node_scratch),&inode)==B2L_APFS_FSTREE_ENOTFOUND);

  /* Multi-level virtual FSTREE: root branch OID 66 -> leaf OID 67. */
  {uint8_t *omap_tree=disk+9*BS,*root=disk+10*BS,*leaf=disk+11*BS;
   memset(omap_tree,0,BS);w64(omap_tree+8,9);w64(omap_tree+16,12);
   w32(omap_tree+24,0x40000002u);w32(omap_tree+32,7);w32(omap_tree+36,2);
   w16(omap_tree+40,0);w16(omap_tree+42,8);
   w16(omap_tree+56,0);w16(omap_tree+58,16);
   w16(omap_tree+60,16);w16(omap_tree+62,32);
   w64(omap_tree+64,66);w64(omap_tree+72,3);
   w64(omap_tree+80,67);w64(omap_tree+88,3);
   w32(omap_tree+4024,0);w32(omap_tree+4028,BS);w64(omap_tree+4032,11);
   w32(omap_tree+4040,0);w32(omap_tree+4044,BS);w64(omap_tree+4048,10);
   seal(omap_tree);
   memset(root,0,BS);w64(root+8,66);w64(root+16,12);
   w32(root+24,0x80000002u);w32(root+28,0x0eu);
   w16(root+32,1);w16(root+34,1);w32(root+36,1);
   w16(root+40,0);w16(root+42,8);
   w16(root+56,0);w16(root+58,8);w16(root+60,8);w16(root+62,8);
   w64(root+64,(3ULL<<60)|2ULL);w64(root+BS-40-8,67);seal(root);
   memset(leaf,0,BS);w64(leaf+8,67);w64(leaf+16,12);
   w32(leaf+24,0x80000003u);w32(leaf+28,0x0eu);
   w16(leaf+32,2);w16(leaf+34,0);w32(leaf+36,1);
   w16(leaf+40,0);w16(leaf+42,8);
   w16(leaf+56,0);w16(leaf+58,8);w16(leaf+60,92);w16(leaf+62,92);
   w64(leaf+64,(3ULL<<60)|2ULL);
   w64(leaf+BS-92,1);w64(leaf+BS-92+8,0);
   w64(leaf+BS-92+16,100);w64(leaf+BS-92+24,200);
   w64(leaf+BS-92+32,300);w64(leaf+BS-92+40,400);
   w64(leaf+BS-92+48,2);w32(leaf+BS-92+56,0);
   w32(leaf+BS-92+72,501);w32(leaf+BS-92+76,20);
   w16(leaf+BS-92+80,0x41ed);seal(leaf);
   assert(b2l_apfs_fstree_lookup_inode(rd,NULL,sizeof(disk),BS,BLOCKS,
       &vol,8,2,omap_scratch,sizeof(omap_scratch),node_scratch,
       sizeof(node_scratch),&inode)==B2L_APFS_FSTREE_OK);
   assert(inode.oid==2&&inode.owner==501&&inode.mode==0x41ed);
  }
  fs[40]=0xff;fs[41]=0xff;seal(fs);
  assert(b2l_apfs_fstree_lookup_inode(rd,NULL,sizeof(disk),BS,BLOCKS,
      &vol,8,2,omap_scratch,sizeof(omap_scratch),node_scratch,
      sizeof(node_scratch),&inode)==B2L_APFS_FSTREE_EBOUNDS);
 }
 puts("APFS checkpoint and OMAP decoder tests passed");return 0;}

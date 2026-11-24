/* SPDX-License-Identifier: MIT */
#include "b2l_apfs_fstree.h"

#define B2L_APFS_BTREE_ROOT 0x0001u
#define B2L_APFS_BTREE_LEAF 0x0002u
#define B2L_APFS_BTREE_FIXED 0x0004u
#define B2L_APFS_BTREE_HASHED 0x0008u
#define B2L_APFS_BTREE_NOHEADER 0x0010u
#define B2L_APFS_BTREE_CHECK_KOFF 0x8000u
#define B2L_APFS_FSTREE_SUBTYPE 0x0000000eu
#define B2L_APFS_INODE_TYPE 3u
#define B2L_APFS_MAX_DEPTH 32u
#define B2L_APFS_NODE_HEADER_SIZE 56u
#define B2L_APFS_BTREE_INFO_SIZE 40u
#define B2L_APFS_INODE_FIXED_SIZE 92u
#define B2L_APFS_OID_MASK 0x0fffffffffffffffULL

static uint16_t r16(const uint8_t *p) {
 return (uint16_t)p[0]|((uint16_t)p[1]<<8);
}
static uint32_t r32(const uint8_t *p) {
 return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint64_t r64(const uint8_t *p) {
 return (uint64_t)r32(p)|((uint64_t)r32(p+4)<<32);
}
static uint64_t fletcher64(const uint8_t *p,size_t n) {
 const uint64_t mod=0xffffffffULL; uint64_t a=0,b=0,lo,hi; size_t i;
 for(i=8;i<n;i+=4) { a=(a+r32(p+i))%mod; b=(b+a)%mod; }
 lo=0xffffffffULL-((a+b)%mod);
 hi=0xffffffffULL-((a+lo)%mod);
 return (hi<<32)|lo;
}

/* Compare a stored FSTREE key with the exact 8-byte inode key.
 * APFS orders the 60-bit object ID before the 4-bit record type. For an
 * inode target (no key tail), any longer key with the same prefix sorts
 * after the exact inode key.
 */
static int compare_inode_key(const uint8_t *key,size_t key_len,
 uint64_t target_oid) {
 uint64_t hdr=r64(key),oid=hdr&B2L_APFS_OID_MASK;
 uint32_t kind=(uint32_t)(hdr>>60);
 if(oid<target_oid) return -1;
 if(oid>target_oid) return 1;
 if(kind<B2L_APFS_INODE_TYPE) return -1;
 if(kind>B2L_APFS_INODE_TYPE) return 1;
 return key_len==8u?0:1;
}

static int decode_inode(uint64_t wanted_oid,uint64_t xid,
 const uint8_t *v,size_t n,struct b2l_apfs_inode_info *out) {
 struct b2l_apfs_inode_info tmp;
 if(!v||!out||n<B2L_APFS_INODE_FIXED_SIZE)
  return B2L_APFS_FSTREE_EBOUNDS;
 tmp.oid=wanted_oid; tmp.xid=xid;
 tmp.parent_id=r64(v); tmp.private_id=r64(v+8);
 tmp.create_time=r64(v+16); tmp.mod_time=r64(v+24);
 tmp.change_time=r64(v+32); tmp.access_time=r64(v+40);
 tmp.internal_flags=r64(v+48); tmp.nchildren=r32(v+56);
 tmp.owner=r32(v+72); tmp.gid=r32(v+76); tmp.mode=r16(v+80);
 tmp.value_size=(uint32_t)n;
 *out=tmp;
 return B2L_APFS_FSTREE_OK;
}

int b2l_apfs_fstree_lookup_inode(b2l_apfs_read_fn fn,void *ctx,
 uint64_t devsz,uint32_t bs,uint64_t blocks,
 const struct b2l_apfs_volume_info *volume,uint64_t volume_omap_pa,
 uint64_t inode_oid,void *omap_scratch,size_t omap_scratchsz,
 void *node_scratch,size_t node_scratchsz,struct b2l_apfs_inode_info *out) {
 uint8_t *omap=(uint8_t *)omap_scratch,*node=(uint8_t *)node_scratch;
 struct b2l_apfs_omap_info oi;
 uint64_t seen[B2L_APFS_MAX_DEPTH],current_oid;
 unsigned depth; int rc;
 if(!fn||!volume||!omap||!node||!out||!inode_oid||
    bs<4096u||bs>65536u||(bs&(bs-1u))||blocks==0||
    blocks>devsz/bs||omap_scratchsz<bs||node_scratchsz<bs||
    !volume->xid||!volume->root_tree_oid||inode_oid>B2L_APFS_OID_MASK||
    (volume->root_tree_type&0xffffu)!=2u||
    (volume->root_tree_type&0xc0000000u)!=0x80000000u||
    !volume_omap_pa||(volume->omap_oid&&volume_omap_pa!=volume->omap_oid)||
    volume_omap_pa>=blocks||volume_omap_pa>UINT64_MAX/bs)
  return B2L_APFS_FSTREE_EINVAL;

 /* The volume OMAP object is a physical block; its tree_oid is the
  * physical root of the fixed-KV OMAP B-tree.
  */
 {
  uint64_t off=volume_omap_pa*(uint64_t)bs;
  if(off>devsz||bs>devsz-off) return B2L_APFS_FSTREE_EBOUNDS;
  if(fn(ctx,off,omap,bs)!=0) return B2L_APFS_FSTREE_EIO;
 }
 rc=b2l_apfs_decode_omap(omap,bs,&oi);
 if(rc==B2L_APFS_OMAP_ECHECKSUM) return B2L_APFS_FSTREE_ECHECKSUM;
 if(rc!=B2L_APFS_OMAP_OK) return B2L_APFS_FSTREE_ETYPE;
 if(oi.tree_oid>=blocks) return B2L_APFS_FSTREE_EBOUNDS;

 current_oid=volume->root_tree_oid;
 for(depth=0;depth<B2L_APFS_MAX_DEPTH;depth++) {
  struct b2l_apfs_omap_key map_key;
  struct b2l_apfs_omap_value map_value;
  uint64_t paddr,off,child_oid=0,prev_id=0;
  uint32_t obj_type,nkeys,toc_off,toc_len,i;
  uint16_t node_flags,level;
  size_t key_start,values_end,toc_start;
  int leaf,have_prev=0,found_child=0;
  for(i=0;i<depth;i++)
   if(seen[i]==current_oid) return B2L_APFS_FSTREE_EBOUNDS;
  seen[depth]=current_oid;

  rc=b2l_apfs_omap_lookup(fn,ctx,devsz,bs,blocks,oi.tree_oid,
      current_oid,volume->xid,omap,omap_scratchsz,&map_key,&map_value);
  if(rc==B2L_APFS_OMAP_EDELETED) return B2L_APFS_FSTREE_ENOTFOUND;
  if(rc==B2L_APFS_OMAP_ECHECKSUM) return B2L_APFS_FSTREE_ECHECKSUM;
  if(rc==B2L_APFS_OMAP_ETYPE) return B2L_APFS_FSTREE_ETYPE;
  if(rc!=B2L_APFS_OMAP_OK) return B2L_APFS_FSTREE_EBOUNDS;
  if(map_value.size<bs||map_value.paddr>=blocks||
     map_value.paddr>UINT64_MAX/bs)
   return B2L_APFS_FSTREE_EBOUNDS;
  paddr=map_value.paddr; off=paddr*(uint64_t)bs;
  if(off>devsz||bs>devsz-off) return B2L_APFS_FSTREE_EBOUNDS;
  if(fn(ctx,off,node,bs)!=0) return B2L_APFS_FSTREE_EIO;
  if(r64(node)!=fletcher64(node,bs)) return B2L_APFS_FSTREE_ECHECKSUM;

  obj_type=r32(node+24);
  if(((obj_type&0xffffu)!=2u&&(obj_type&0xffffu)!=3u)||
     (obj_type&0x3fff0000u)!=0u||
     (obj_type&0xc0000000u)==0xc0000000u||
     r32(node+28)!=B2L_APFS_FSTREE_SUBTYPE||
     r64(node+8)!=current_oid||!r64(node+16)||r64(node+16)>volume->xid)
   return B2L_APFS_FSTREE_ETYPE;

  node_flags=r16(node+32); level=r16(node+34); nkeys=r32(node+36);
  if((node_flags&B2L_APFS_BTREE_CHECK_KOFF)||
     (node_flags&B2L_APFS_BTREE_FIXED)||
     (node_flags&B2L_APFS_BTREE_NOHEADER)||
     (node_flags&~(B2L_APFS_BTREE_ROOT|B2L_APFS_BTREE_LEAF|
                   B2L_APFS_BTREE_FIXED|B2L_APFS_BTREE_HASHED|
                   B2L_APFS_BTREE_NOHEADER))||
     (((node_flags&B2L_APFS_BTREE_ROOT)!=0)!=(depth==0))||
     (((obj_type&0xffffu)==2u)!=(depth==0))||
     (((node_flags&B2L_APFS_BTREE_LEAF)!=0)!=(level==0)))
   return B2L_APFS_FSTREE_ETYPE;
  leaf=(node_flags&B2L_APFS_BTREE_LEAF)!=0;
  if(nkeys==0) return B2L_APFS_FSTREE_ENOTFOUND;

  toc_off=r16(node+40); toc_len=r16(node+42);
  if(toc_off==0xffffu||toc_len==0xffffu||
     toc_off>bs-B2L_APFS_NODE_HEADER_SIZE||
     toc_len>bs-B2L_APFS_NODE_HEADER_SIZE-toc_off||
     nkeys>toc_len/8u)
   return B2L_APFS_FSTREE_EBOUNDS;
  toc_start=B2L_APFS_NODE_HEADER_SIZE+(size_t)toc_off;
  key_start=toc_start+(size_t)toc_len;
  values_end=(node_flags&B2L_APFS_BTREE_ROOT)?
      (size_t)bs-B2L_APFS_BTREE_INFO_SIZE:(size_t)bs;
  if(key_start>values_end) return B2L_APFS_FSTREE_EBOUNDS;

  for(i=0;i<nkeys;i++) {
   const uint8_t *te=node+toc_start+(size_t)i*8u;
   uint16_t ko=r16(te),kl=r16(te+2),vo=r16(te+4),vl=r16(te+6);
   size_t kp,vp;
   uint64_t hdr,id; uint32_t kind; int cmp;
   if(kl<8u||vo==0xffffu||vl==0u||
      (size_t)ko>values_end-key_start||
      (size_t)kl>values_end-key_start-(size_t)ko||
      (size_t)vo>values_end)
    return B2L_APFS_FSTREE_EBOUNDS;
   kp=key_start+(size_t)ko;
   vp=values_end-(size_t)vo;
   if(vp<key_start||(size_t)vl>values_end-vp||
      !(kp+(size_t)kl<=vp||vp+(size_t)vl<=kp))
    return B2L_APFS_FSTREE_EBOUNDS;
   hdr=r64(node+kp); id=hdr&B2L_APFS_OID_MASK; kind=(uint32_t)(hdr>>60);
   if(have_prev&&(id<prev_id)) return B2L_APFS_FSTREE_EBOUNDS;
   prev_id=id; have_prev=1;

   if(leaf) {
    cmp=compare_inode_key(node+kp,kl,inode_oid);
    if(cmp==0)
     return decode_inode(inode_oid,r64(node+16),node+vp,vl,out);
    if(cmp>0) break;
   } else {
    cmp=compare_inode_key(node+kp,kl,inode_oid);
    if(cmp<=0) {
     if(vl<8u) return B2L_APFS_FSTREE_EBOUNDS;
     child_oid=r64(node+vp);
     if(!child_oid) return B2L_APFS_FSTREE_EBOUNDS;
     found_child=1;
    } else break;
   }
   (void)kind;
  }
  if(leaf) return B2L_APFS_FSTREE_ENOTFOUND;
  if(!found_child) return B2L_APFS_FSTREE_ENOTFOUND;
  current_oid=child_oid;
 }
 return B2L_APFS_FSTREE_EBOUNDS;
}

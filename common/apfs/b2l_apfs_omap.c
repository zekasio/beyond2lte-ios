/* SPDX-License-Identifier: MIT */
#include "b2l_apfs_omap.h"
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
int b2l_apfs_decode_omap(const void *data,size_t n,struct b2l_apfs_omap_info *o) {
 const uint8_t *p=(const uint8_t *)data; uint32_t t;
 if(!p||!o||n<88||(n&3)) return B2L_APFS_OMAP_EINVAL;
 t=r32(p+24);
 if((t&0xffffu)!=0x0bu||(t&0xc0000000u)!=0x40000000u)
  return B2L_APFS_OMAP_ETYPE;
 if(r64(p)!=fletcher(p,n)) return B2L_APFS_OMAP_ECHECKSUM;
 o->oid=r64(p+8); o->xid=r64(p+16); o->flags=r32(p+32);
 o->snapshot_count=r32(p+36); o->tree_type=r32(p+40);
 o->snapshot_tree_type=r32(p+44); o->tree_oid=r64(p+48);
 o->snapshot_tree_oid=r64(p+56); o->most_recent_snapshot=r64(p+64);
 if(o->oid==0||o->tree_oid==0||o->tree_type==0)
  return B2L_APFS_OMAP_EBOUNDS;
 return B2L_APFS_OMAP_OK;
}
int b2l_apfs_decode_omap_key(const void *data,size_t n,struct b2l_apfs_omap_key *o) {
 const uint8_t *p=(const uint8_t *)data;
 if(!p||!o||n<16) return B2L_APFS_OMAP_EINVAL;
 o->oid=r64(p); o->xid=r64(p+8);
 if(!o->oid||!o->xid) return B2L_APFS_OMAP_EBOUNDS;
 return B2L_APFS_OMAP_OK;
}
int b2l_apfs_decode_omap_value(const void *data,size_t n,struct b2l_apfs_omap_value *o) {
 const uint8_t *p=(const uint8_t *)data;
 if(!p||!o||n<16) return B2L_APFS_OMAP_EINVAL;
 o->flags=r32(p); o->size=r32(p+4); o->paddr=r64(p+8);
 if(o->flags&1u) return B2L_APFS_OMAP_EDELETED;
 if(!o->size||!o->paddr) return B2L_APFS_OMAP_EBOUNDS;
 return B2L_APFS_OMAP_OK;
}

/* B-tree node flags and layout are checked before any offset is followed. */
#define OMAP_BTN_ROOT 0x0001u
#define OMAP_BTN_LEAF 0x0002u
#define OMAP_BTN_FIXED 0x0004u
#define OMAP_MAX_DEPTH 32u
static int key_leq(uint64_t ao,uint64_t ax,uint64_t bo,uint64_t bx) {
 return ao<bo || (ao==bo && ax<=bx);
}
int b2l_apfs_omap_lookup(b2l_apfs_read_fn fn,void *ctx,uint64_t devsz,
 uint32_t bs,uint64_t blocks,uint64_t root,uint64_t target_oid,uint64_t max_xid,
 void *scratch,size_t scratchsz,struct b2l_apfs_omap_key *found_key,
 struct b2l_apfs_omap_value *found_value) {
 uint8_t *p=(uint8_t *)scratch; uint64_t seen[OMAP_MAX_DEPTH];
 uint64_t current=root; unsigned depth;
 if(!fn||!p||!found_key||!found_value||!target_oid||!max_xid||
    bs<4096||bs>65536||(bs&(bs-1))||scratchsz<bs||!blocks||
    blocks>devsz/bs) return B2L_APFS_OMAP_EINVAL;
 for(depth=0;depth<OMAP_MAX_DEPTH;depth++) {
  uint64_t off; uint32_t objtype,flags,nkeys,table_off,table_len;
  uint32_t entry_size,key_start,values_end,val_size,i;
  int leaf; uint64_t child=0,best_xid=0,prev_oid=0,prev_xid=0; int have=0,have_prev=0,best_status=B2L_APFS_OMAP_EBOUNDS;
  for(i=0;i<depth;i++) if(seen[i]==current) return B2L_APFS_OMAP_EBOUNDS;
  seen[depth]=current;
  if(current>=blocks||current>UINT64_MAX/bs) return B2L_APFS_OMAP_EBOUNDS;
  off=current*(uint64_t)bs;
  if(off>devsz||bs>devsz-off) return B2L_APFS_OMAP_EBOUNDS;
  if(fn(ctx,off,p,bs)!=0) return B2L_APFS_OMAP_EINVAL;
  if(r64(p)!=fletcher(p,bs)) return B2L_APFS_OMAP_ECHECKSUM;
  objtype=r32(p+24)&0xffffu;
  if(objtype!=2u&&objtype!=3u) return B2L_APFS_OMAP_ETYPE;
  flags=(uint32_t)p[32]|((uint32_t)p[33]<<8);
  nkeys=r32(p+36);
  if(!(flags&OMAP_BTN_FIXED)||(flags&0x8000u)) return B2L_APFS_OMAP_ETYPE;
  leaf=(flags&OMAP_BTN_LEAF)!=0;
  if(((flags&OMAP_BTN_ROOT)!=0)!=(objtype==2u) || (r32(p+24)&0xc0000000u)!=0x40000000u) return B2L_APFS_OMAP_ETYPE;
  table_off=(uint32_t)p[40]|((uint32_t)p[41]<<8);
  table_len=(uint32_t)p[42]|((uint32_t)p[43]<<8);
  if(table_off==0xffffu||table_len==0xffffu||table_off>bs-56u||
     table_len>bs-56u-table_off) return B2L_APFS_OMAP_EBOUNDS;
  entry_size=4u;
  if(nkeys>table_len/entry_size) return B2L_APFS_OMAP_EBOUNDS;
  key_start=56u+table_off+table_len;
  values_end=(flags&OMAP_BTN_ROOT)?bs-40u:bs;
  val_size=leaf?16u:8u;
  if(key_start>values_end||nkeys> (values_end-key_start)/16u ||
     (uint64_t)nkeys*val_size>values_end-key_start)
     return B2L_APFS_OMAP_EBOUNDS;
  for(i=0;i<nkeys;i++) {
   uint64_t ko,vo; struct b2l_apfs_omap_key k;
   uint32_t toc=56u+table_off+i*4u;
   uint16_t keyoff=(uint16_t)(p[toc]|((uint16_t)p[toc+1]<<8));
   uint16_t valoff=(uint16_t)(p[toc+2]|((uint16_t)p[toc+3]<<8));
   ko=(uint64_t)key_start+keyoff;
   if(valoff==0||valoff>values_end-key_start) return B2L_APFS_OMAP_EBOUNDS;
   vo=(uint64_t)values_end-valoff;
   if(ko+16u>values_end||vo+val_size>values_end||vo<key_start||
      !(ko+16u<=vo || vo+val_size<=ko))
     return B2L_APFS_OMAP_EBOUNDS;
   if(b2l_apfs_decode_omap_key(p+ko,16,&k)!=B2L_APFS_OMAP_OK)
     return B2L_APFS_OMAP_EBOUNDS;
   if(have_prev&&(k.oid<prev_oid||(k.oid==prev_oid&&k.xid<prev_xid)))
     return B2L_APFS_OMAP_EBOUNDS;
   prev_oid=k.oid;prev_xid=k.xid;have_prev=1;
   if(leaf) {
    if(k.oid==target_oid&&k.xid<=max_xid&&(!have||k.xid>best_xid)) {
     int vr=b2l_apfs_decode_omap_value(p+vo,16,found_value);
     if(vr==B2L_APFS_OMAP_OK) {
      uint64_t span=((uint64_t)found_value->size+bs-1u)/bs;
      if(found_value->paddr>=blocks||span>blocks-found_value->paddr)
       return B2L_APFS_OMAP_EBOUNDS;
     }
     *found_key=k; best_xid=k.xid; have=1; best_status=vr;
    }
   } else if(key_leq(k.oid,k.xid,target_oid,max_xid)) {
    child=r64(p+vo);
   } else break;
  }
  if(leaf) return have?best_status:B2L_APFS_OMAP_EBOUNDS;
  if(!child) return B2L_APFS_OMAP_EBOUNDS;
  current=child;
 }
 return B2L_APFS_OMAP_EBOUNDS;
}

/* Read the container OMAP object from the NXSB physical address, decode its
 * B-tree root, then resolve a virtual object ID at the requested transaction.
 */
int b2l_apfs_resolve_container_oid(b2l_apfs_read_fn fn,void *ctx,uint64_t devsz,
 uint32_t bs,uint64_t blocks,uint64_t omap_pa,uint64_t oid,uint64_t max_xid,
 void *omap_scratch,size_t omap_scratchsz,void *tree_scratch,size_t tree_scratchsz,
 struct b2l_apfs_omap_key *key,struct b2l_apfs_omap_value *value) {
 uint8_t *omap=(uint8_t *)omap_scratch; uint64_t off;
 struct b2l_apfs_omap_info info;
 int rc;
 if(!fn||!omap||!tree_scratch||!key||!value||bs<4096||bs>65536||
    (bs&(bs-1))||omap_scratchsz<bs||blocks>devsz/bs||
    omap_pa>=blocks||omap_pa>UINT64_MAX/bs) return B2L_APFS_OMAP_EINVAL;
 off=omap_pa*(uint64_t)bs;
 if(off>devsz||bs>devsz-off) return B2L_APFS_OMAP_EBOUNDS;
 if(fn(ctx,off,omap,bs)!=0) return B2L_APFS_OMAP_EINVAL;
 rc=b2l_apfs_decode_omap(omap,bs,&info);
 if(rc!=B2L_APFS_OMAP_OK) return rc;
 if(info.xid>max_xid||info.tree_oid>=blocks) return B2L_APFS_OMAP_EBOUNDS;
 return b2l_apfs_omap_lookup(fn,ctx,devsz,bs,blocks,info.tree_oid,oid,max_xid,
     tree_scratch,tree_scratchsz,key,value);
}

/* Resolve the volume's virtual FSTREE root. The OMAP object and its B-tree
 * are physical objects; only root_tree_oid is virtual and needs lookup.
 */
int b2l_apfs_resolve_volume_root(b2l_apfs_read_fn fn,void *ctx,uint64_t devsz,
 uint32_t bs,uint64_t blocks,const struct b2l_apfs_volume_info *volume,
 uint64_t volume_omap_pa,void *omap_scratch,size_t omap_scratchsz,
 void *tree_scratch,size_t tree_scratchsz,struct b2l_apfs_omap_key *key,
 struct b2l_apfs_omap_value *value) {
 uint8_t *omap=(uint8_t *)omap_scratch; uint64_t off;
 struct b2l_apfs_omap_info info; int rc;
 if(!fn||!volume||!omap||!tree_scratch||!key||!value||
    bs<4096||bs>65536||(bs&(bs-1))||omap_scratchsz<bs||
    tree_scratchsz<bs||!blocks||blocks>devsz/bs||
    !volume->xid||!volume->root_tree_oid||!volume_omap_pa||
    (volume->omap_oid&&volume_omap_pa!=volume->omap_oid)||
    volume_omap_pa>=blocks||volume_omap_pa>UINT64_MAX/bs)
  return B2L_APFS_OMAP_EINVAL;
 off=volume_omap_pa*(uint64_t)bs;
 if(off>devsz||bs>devsz-off) return B2L_APFS_OMAP_EBOUNDS;
 if(fn(ctx,off,omap,bs)!=0) return B2L_APFS_OMAP_EINVAL;
 rc=b2l_apfs_decode_omap(omap,bs,&info);
 if(rc!=B2L_APFS_OMAP_OK) return rc;
 if(info.tree_oid>=blocks) return B2L_APFS_OMAP_EBOUNDS;
 return b2l_apfs_omap_lookup(fn,ctx,devsz,bs,blocks,info.tree_oid,
     volume->root_tree_oid,volume->xid,tree_scratch,tree_scratchsz,key,value);
}

/* SPDX-License-Identifier: MIT */
#include "../../common/apfs/b2l_apfs_probe.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IMAGE_SIZE 4096u
static uint8_t image[IMAGE_SIZE];

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
    p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void put64(uint8_t *p, uint64_t v) {
    put32(p,(uint32_t)v); put32(p+4,(uint32_t)(v>>32));
}
static uint64_t checksum(const uint8_t *p, size_t n) {
    uint64_t a=0,b=0;
    for (size_t i=0;i<n;i+=4) {
        uint32_t w=(i<8)?0:le32(p+i);
        a=(a+w)%0xffffffffULL; b=(b+a)%0xffffffffULL;
    }
    { uint64_t lo=0xffffffffULL-((a+b)%0xffffffffULL);
      uint64_t hi=0xffffffffULL-((a+lo)%0xffffffffULL);
      return (hi<<32)|lo; }
}
static int read_image(void *ctx, uint64_t off, void *dst, size_t n) {
    (void)ctx;
    if (off > IMAGE_SIZE || n > IMAGE_SIZE-(size_t)off) return -1;
    memcpy(dst,image+(size_t)off,n);
    return 0;
}
static void make_image(void) {
    memset(image,0,sizeof(image));
    put32(image+24,0x80000001u);
    memcpy(image+32,"NXSB",4);
    put32(image+36,4096);
    put64(image+40,1);
    image[72]=0x42;
    put64(image,checksum(image,sizeof(image)));
}
int main(void) {
    uint8_t scratch[B2L_APFS_NXSB_MAX_BLOCK_SIZE];
    struct b2l_apfs_container_info info;

    make_image();
    assert(b2l_apfs_probe_container(read_image,NULL,sizeof(image),
        scratch,sizeof(scratch),&info)==B2L_APFS_PROBE_OK);
    assert(info.block_size==4096 && info.block_count==1 && info.uuid[0]==0x42);

    image[200]^=1;
    assert(b2l_apfs_probe_container(read_image,NULL,sizeof(image),
        scratch,sizeof(scratch),&info)==B2L_APFS_PROBE_ECHECKSUM);

    make_image(); image[32]='X';
    assert(b2l_apfs_probe_container(read_image,NULL,sizeof(image),
        scratch,sizeof(scratch),&info)==B2L_APFS_PROBE_EMAGIC);

    make_image(); put32(image+36,3000);
    assert(b2l_apfs_probe_container(read_image,NULL,sizeof(image),
        scratch,sizeof(scratch),&info)==B2L_APFS_PROBE_EBLOCKSIZE);

    make_image(); put64(image+40,UINT64_MAX);
    assert(b2l_apfs_probe_container(read_image,NULL,sizeof(image),
        scratch,sizeof(scratch),&info)==B2L_APFS_PROBE_EBOUNDS);

    puts("APFS NXSB probe tests passed");
    return 0;
}

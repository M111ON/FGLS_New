/*
 * debug_check.c — pinpoint DiamondBlock integration bug
 * Build: gcc -O2 -I. -Icore/twin_core debug_check.c -lm -o debug_check
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "pogls_fold.h"
#include "geo_diamond_field.h"

static uint8_t D4_FWD[8][64], D4_INV[8][64];
static void init_d4(void) {
    uint8_t s[64]; for(int i=0;i<64;i++) s[i]=(uint8_t)i;
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[1][x*8+(7-y)]=s[y*8+x];
    for(int i=0;i<64;i++) D4_FWD[2][63-i]=s[i];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[3][(7-x)*8+y]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[4][y*8+(7-x)]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[5][(7-y)*8+x]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[6][x*8+y]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[7][(7-x)*8+(7-y)]=s[y*8+x];
    for(int i=0;i<64;i++) D4_FWD[0][i]=s[i];
    for(int st=0;st<8;st++) for(int i=0;i<64;i++) D4_INV[st][D4_FWD[st][i]]=(uint8_t)i;
}
static inline void d4_apply(uint8_t out[64],const uint8_t in[64],int st){for(int i=0;i<64;i++)out[i]=in[D4_FWD[st][i]];}
static inline void d4_inv(uint8_t out[64],const uint8_t in[64],int st){for(int i=0;i<64;i++)out[i]=in[D4_INV[st][i]];}

static uint8_t G_H[64],G_HI[64];
static void init_h(void){
    uint8_t hmap[64];
    for(int p=0;p<64;p++){
        uint32_t px=p%8,py=p/8,rx,ry,s,d=0,tx=px,ty=py;
        for(s=4;s>0;s>>=1){rx=(tx&s)?1:0;ry=(ty&s)?1:0;
            d+=s*s*((3*rx)^ry);
            if(!ry){if(rx){tx=s-1-tx;ty=s-1-ty;}uint32_t t=tx;tx=ty;ty=t;}}
        hmap[p]=(uint8_t)(d&0x3F);
    }
    for(int p=0;p<64;p++) G_H[hmap[p]]=(uint8_t)p;
    for(int p=0;p<64;p++) G_HI[G_H[p]]=p;
}
static inline void h_fwd(uint8_t o[64],const uint8_t s[64]){for(int i=0;i<64;i++)o[i]=s[G_H[i]];}
static inline void h_inv(uint8_t o[64],const uint8_t s[64]){for(int i=0;i<64;i++)o[i]=s[G_HI[i]];}
static inline uint64_t fnv64(const uint8_t *d,int n){uint64_t h=1469598103934665603ULL;for(int i=0;i<n;i++){h^=d[i];h*=1099511628211ULL;}return h;}
static uint8_t d4_canon(const uint8_t s[64],uint8_t out[64]){
    uint64_t bh=UINT64_MAX;int bi=0;
    for(int i=0;i<8;i++){uint8_t tmp[64];d4_apply(tmp,s,i);
        uint64_t h=fnv64(tmp,64);if(h<bh){bh=h;bi=i;memcpy(out,tmp,64);}}
    return (uint8_t)bi;
}

#define SEED_CAP (1<<16)
#define SEED_MASK (SEED_CAP-1)
typedef struct{uint64_t seed;uint8_t chunk[64];uint8_t rid;uint8_t used;} Slot;

static int sidx_get(Slot *idx, uint64_t seed, uint8_t *chunk, uint8_t *rid){
    uint32_t s=(uint32_t)(seed&SEED_MASK);
    for(int i=0;i<SEED_CAP;i++){
        uint32_t si=(s+i)&SEED_MASK;
        if(!idx[si].used) return 0;
        if(idx[si].seed==seed){memcpy(chunk,idx[si].chunk,64);*rid=idx[si].rid;return 1;}
    }
    return 0;
}
static void sidx_put(Slot *idx, uint64_t seed, const uint8_t *chunk, uint8_t rid){
    uint32_t s=(uint32_t)(seed&SEED_MASK);
    for(int i=0;i<SEED_CAP;i++){
        uint32_t si=(s+i)&SEED_MASK;
        if(!idx[si].used){idx[si].used=1;idx[si].seed=seed;idx[si].rid=rid;memcpy(idx[si].chunk,chunk,64);return;}
        if(idx[si].seed==seed) return;
    }
}

int main(void) {
    init_h(); init_d4();

    const char *files[]={
        "fgls/fibo_tile_dispatch.h","fgls/fibo_layer_header.h",
        "fgls/test_diamond_flow_grouping.c","fgls/hb_header_frame.h",
        "core/core/pogls_fold.h","core/core/geo_diamond_field.h",NULL
    };
    uint8_t *pool=malloc(4096*64); int total=0;
    uint8_t *fbuf=malloc(512*1024);
    for(int fi=0;files[fi];fi++){
        FILE *f=fopen(files[fi],"rb"); if(!f) continue;
        int n=(int)fread(fbuf,1,512*1024,f); fclose(f);
        int nc=n/64; if(total+nc>4096) nc=4096-total;
        memcpy(pool+total*64,fbuf,nc*64); total+=nc;
    }
    printf("total chunks: %d\n",total);

    uint8_t *data=pool;
    uint64_t N=(uint64_t)total;

    uint64_t baseline=diamond_baseline();
    printf("baseline=0x%016llx popcnt=%d\n",
           (unsigned long long)baseline,__builtin_popcountll(baseline));

    printf("\nChunk flag analysis (first 200):\n");
    int flag_count[4]={0};
    int isect_zero_count=0;

    for(uint64_t i=0;i<N&&i<200;i++){
        const uint8_t *chunk=data+i*64;
        uint64_t seed=0;
        for(int j=0;j<8;j++) seed^=((uint64_t)chunk[j])<<(j*8);
        uint8_t face=(uint8_t)(((seed>>32)*12u)>>32);
        uint8_t edge=(uint8_t)(((seed&0xFFFFFFFFu)*5u)>>32);
        uint8_t z=(uint8_t)((seed>>16)&0xFFu);

        DiamondBlock db=fold_block_init(face,edge,(uint32_t)z<<16,1,0);
        memcpy(&db.core.raw,chunk,8);
        db.invert=~db.core.raw;
        fold_build_quad_mirror(&db);

        uint64_t isect=fold_fibo_intersect(&db);
        uint8_t seen[256]={0}; uint32_t ent=0;
        for(int j=0;j<64;j++) if(!seen[chunk[j]]++) ent++;

        int xor_ok=fold_xor_audit(&db);
        int flag;
        if(xor_ok==0) flag=0;
        else if(isect==0) flag=3;
        else if(ent<=12||1) flag=0;
        else if(ent<=24) flag=1;
        else flag=2;

        if(isect==0) isect_zero_count++;
        flag_count[flag>=0?flag:0]++;

        if(i<10||(flag!=0&&i<50)){
            printf("  [%3llu] isect=0x%016llx pop=%d ent=%u xor=%d flag=%d\n",
                   (unsigned long long)i,(unsigned long long)isect,
                   __builtin_popcountll(isect),ent,xor_ok,flag);
        }
    }
    printf("\nFlag dist (first 200): seed=%d delta=%d lossless=%d dna=%d\n",
           flag_count[0],flag_count[1],flag_count[2],flag_count[3]);
    printf("isect==0 count: %d/200\n",isect_zero_count);

    printf("\n--- Pure pipeline roundtrip (seed-only, ignoring flag) ---\n");
    Slot *idx=(Slot*)calloc(SEED_CAP,sizeof(Slot));
    int ok=0;
    for(uint64_t i=0;i<N;i++){
        uint8_t canon[64];
        uint8_t rid=d4_canon(data+i*64,canon);
        uint8_t hperm[64]; h_fwd(hperm,canon);
        uint64_t s=fnv64(hperm,64);
        sidx_put(idx,s,hperm,rid);

        uint8_t hperm2[64],canon2[64],back[64],rid2;
        if(!sidx_get(idx,s,hperm2,&rid2)) continue;
        h_inv(canon2,hperm2);
        d4_inv(back,canon2,rid2);
        if(memcmp(data+i*64,back,64)==0) ok++;
    }
    printf("Pure pipeline: pass=%d/%d (%.1f%%)\n",ok,(int)N,(double)ok*100/N);

    free(idx); free(pool); free(fbuf);
    return 0;
}

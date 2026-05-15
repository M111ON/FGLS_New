/*
 * bench_realfiles.c — Test pipeline on real file types
 * Build: gcc -O2 -I. -Icore/twin_core bench_realfiles.c -lm -o bench_realfiles
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pogls_fold.h"
#include "geo_diamond_field.h"

static uint8_t G_H[64], G_HI[64];
static void init_h(void){
    uint8_t hmap[64];
    for(int p=0;p<64;p++){uint32_t px=p%8,py=p/8,rx,ry,s,d=0,tx=px,ty=py;
        for(s=4;s>0;s>>=1){rx=(tx&s)?1:0;ry=(ty&s)?1:0;
            d+=s*s*((3*rx)^ry);if(!ry){if(rx){tx=s-1-tx;ty=s-1-ty;}uint32_t t=tx;tx=ty;ty=t;}}
        hmap[p]=(uint8_t)(d&0x3F);}
    for(int p=0;p<64;p++) G_H[hmap[p]]=(uint8_t)p; for(int p=0;p<64;p++) G_HI[G_H[p]]=p;
}
static inline void h_fwd(uint8_t o[64],const uint8_t s[64]){for(int i=0;i<64;i++)o[i]=s[G_H[i]];}
static inline void h_inv(uint8_t o[64],const uint8_t s[64]){for(int i=0;i<64;i++)o[i]=s[G_HI[i]];}

static uint64_t fnv64(const uint8_t *d,int n){
    uint64_t h=1469598103934665603ULL;
    for(int i=0;i<n;i++){h^=d[i];h*=1099511628211ULL;} return h;
}

static uint8_t D4_FWD[8][64], D4_INV[8][64];
static void init_d4(void){
    uint8_t s[64]; for(int i=0;i<64;i++) s[i]=(uint8_t)i;
    for(int i=0;i<64;i++) D4_FWD[0][i]=s[i];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[1][x*8+(7-y)]=s[y*8+x];
    for(int i=0;i<64;i++) D4_FWD[2][63-i]=s[i];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[3][(7-x)*8+y]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[4][y*8+(7-x)]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[5][(7-y)*8+x]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[6][x*8+y]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[7][(7-x)*8+(7-y)]=s[y*8+x];
    for(int st=0;st<8;st++) for(int i=0;i<64;i++) D4_INV[st][D4_FWD[st][i]]=(uint8_t)i;
}
static inline void d4_apply(uint8_t out[64],const uint8_t in[64],int st){
    for(int i=0;i<64;i++) out[i]=in[D4_FWD[st][i]];}
static inline void d4_inv_apply(uint8_t out[64],const uint8_t in[64],int st){
    for(int i=0;i<64;i++) out[i]=in[D4_INV[st][i]];}
static uint8_t d4_canon_rid(const uint8_t s[64],uint8_t out[64]){
    uint64_t bh=UINT64_MAX,th; int bi=0;
    for(int i=0;i<8;i++){uint8_t tmp[64];d4_apply(tmp,s,i);
        th=fnv64(tmp,64);if(th<bh){bh=th;bi=i;memcpy(out,tmp,64);}}
    return (uint8_t)bi;
}

#define SIDX_CAP (1<<16)
#define SIDX_MASK (SIDX_CAP-1)
typedef struct{uint64_t seed;uint8_t chunk[64];uint8_t rid;uint8_t used;}SeedSlot;
typedef struct{SeedSlot*slots;uint64_t ne,nc;}SeedIndex;
static SeedIndex* sx(){SeedIndex*x=calloc(1,sizeof(SeedIndex));
    x->slots=calloc(SIDX_CAP,sizeof(SeedSlot));return x;}
static void sf(SeedIndex*x){free(x->slots);free(x);}
static void sp(SeedIndex*x,uint64_t seed,const uint8_t*ch,uint8_t rid){
    uint32_t s0=(uint32_t)(seed&SIDX_MASK);
    for(int i=0;i<SIDX_CAP;i++){uint32_t s=(s0+i)&SIDX_MASK;
        if(!x->slots[s].used){x->slots[s].used=1;x->slots[s].seed=seed;
            x->slots[s].rid=rid;memcpy(x->slots[s].chunk,ch,64);x->ne++;return;}
        if(x->slots[s].seed==seed) return;x->nc++;}}
static const SeedSlot* sg(const SeedIndex*x,uint64_t seed){
    uint32_t s0=(uint32_t)(seed&SIDX_MASK);
    for(int i=0;i<SIDX_CAP;i++){uint32_t s=(s0+i)&SIDX_MASK;
        if(!x->slots[s].used)return NULL;if(x->slots[s].seed==seed)return &x->slots[s];}
    return NULL;}

static uint64_t dseed(const uint8_t c[64]){
    const uint64_t*w=(const uint64_t*)c;
    uint64_t s=w[0]^w[1]^w[2]^w[3]^w[4]^w[5]^w[6]^w[7];
    s^=s>>33;s*=0xff51afd7ed558ccdULL;s^=s>>33;s*=0xc4ceb9fe1a85ec53ULL;s^=s>>33;
    return s;}
static void dcoord(uint64_t seed,uint8_t*f,uint8_t*e,uint8_t*z){
    uint64_t h=seed;h^=h>>33;h*=0xff51afd7ed558ccdULL;h^=h>>33;h*=0xc4ceb9fe1a85ec53ULL;h^=h>>33;
    *f=(uint8_t)(((uint64_t)(uint32_t)(h>>32)*12u)>>32);
    *e=(uint8_t)(((uint64_t)(uint32_t)(h&0xFFFFFFFFu)*5u)>>32);
    *z=(uint8_t)((h>>16)&0xFFu);}

typedef struct{uint8_t flag;uint64_t seed;uint8_t rid;uint8_t r[64];}Enc;
typedef struct{double ratio;double bsum;double pct;double enc_ms,dec_ms;
    uint64_t n,ok,ns,nd,nl,nda,raw,enc;}Metrics;

/* ── content-based flow stabilizer ─────────────────────────────────
 * ใช้ entropy range เป็น flow ID — STATELESS, invariant to shuffle
 * low entropy (0-128)  → flow 0 (seed-only candidate)
 * high entropy (129-256) → flow 1 (delta/lossless candidate)
 *
 * ข้อดี: ไม่ขึ้นกับตำแหน่ง, shuffle แล้ว behavior เหมือนเดิม
 * ข้อเสีย: coarse เกินไปสำหรับบางไฟล์
 */
typedef struct{uint8_t dummy;}FlowStab;

static void fs_init(FlowStab*f){(void)f;}

/* returns flow ID: 0=low entropy, 1=high entropy */
static int fs_update(FlowStab*f,uint32_t ent){
    (void)f;
    return (ent > 128) ? 1 : 0;}

static Metrics run_pipe(const uint8_t*data,uint64_t N,SeedIndex*idx){
    Metrics m={0};m.n=N;m.raw=N*64;
    Enc*enc=calloc(N,sizeof(Enc));uint8_t*dec=calloc(N,64);
    struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);
    for(uint64_t i=0;i<N;i++){
        const uint8_t*c=data+i*64;uint8_t f,e,z;dcoord(dseed(c),&f,&e,&z);
        DiamondBlock db=fold_block_init(f,e,(uint32_t)z<<16,1,0);
        memcpy(&db.core.raw,c,8);db.invert=~db.core.raw;fold_build_quad_mirror(&db);
        uint8_t cn[64],hp[64];uint8_t rd=d4_canon_rid(c,cn);h_fwd(hp,cn);
        uint64_t s=fnv64(hp,64);
        /* content-based fa: seed มีใน index แล้ว = เคยเห็น content นี้ */
        int fa=(sg(idx,s)!=NULL);
        if(!fold_xor_audit(&db)){
            sp(idx,s,hp,rd);enc[i]=(Enc){0,s,rd};m.ns++;m.enc+=9;continue;}
        uint8_t se[256]={0};uint32_t ent=0;for(int j=0;j<64;j++)if(!se[c[j]]++)ent++;
        if(ent<=12||fa){
            sp(idx,s,hp,rd);enc[i]=(Enc){0,s,rd};m.ns++;m.enc+=9;
        }else if(ent<=24){
            memcpy(enc[i].r,hp,64);enc[i]=(Enc){1,0,rd};memcpy(enc[i].r,hp,64);
            m.nd++;m.enc+=17;
        }else{
            memcpy(enc[i].r,hp,64);enc[i]=(Enc){2,0,rd};memcpy(enc[i].r,hp,64);
            m.nl++;m.enc+=65;
        }
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);m.enc_ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;
    for(uint64_t i=0;i<N;i++){
        uint8_t hp[64];
        if(enc[i].flag==0||enc[i].flag==3){
            const SeedSlot*sl=sg(idx,enc[i].seed);if(!sl)continue;
            memcpy(hp,sl->chunk,64);uint8_t cn[64];h_inv(cn,hp);d4_inv_apply(dec+i*64,cn,sl->rid);
        }else{memcpy(hp,enc[i].r,64);uint8_t cn[64];h_inv(cn,hp);d4_inv_apply(dec+i*64,cn,enc[i].rid);}
        if(memcmp(data+i*64,dec+i*64,64)==0) m.ok++;
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);m.dec_ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;
    m.ratio=m.enc>0?(double)m.raw/(double)m.enc:0;
    m.pct=m.n>0?(double)m.ok*100.0/(double)m.n:0;
    free(enc);free(dec);return m;
}

static void bench_one(const char *label, const uint8_t *data, uint64_t len){
    uint64_t N=len/64; if(N==0) return;
    uint8_t *buf=malloc(len); memcpy(buf,data,len);
    SeedIndex *idx=sx();
    Metrics m=run_pipe(buf,N,idx);
    double sw=(double)len/1e6/(m.enc_ms/1e3);
    printf("%-12s ", label);
    printf("N=%5llu  ratio=%5.3fx  seed=%3.0f%%  exact=%4.1f%%  enc=%5.0fMB/s  dec=%5.0fMB/s\n",
           (unsigned long long)N, m.ratio,
           (double)m.ns*100.0/(double)N,
           m.pct,
           (double)len/1e6/(m.enc_ms/1e3),
           (double)len/1e6/(m.dec_ms/1e3));

    /* Shuffle test on this data */
    uint8_t *shuf=malloc(N*64);
    for(uint64_t i=0;i<N;i++){
        uint64_t j=i+(uint64_t)rand()%(N-i);
        memcpy(shuf,buf+i*64,64);memcpy(buf+i*64,buf+j*64,64);memcpy(buf+j*64,shuf,64);
    }
    free(shuf);
    SeedIndex *idx2=sx();
    Metrics m2=run_pipe(buf,N,idx2);
    double drop=m.ratio>0?(1.0-m2.ratio/m.ratio)*100.0:0;
    printf("  └─ shuffled               ratio=%5.3fx  drop=%.1f%%\n",m2.ratio,drop);

    sf(idx); sf(idx2); free(buf);
}

static uint8_t *load_file(const char *path, uint32_t *len_out){
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *buf=malloc((size_t)sz);
    if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){fclose(f);free(buf);return NULL;}
    fclose(f); *len_out=(uint32_t)sz; return buf;
}

int main(void){
    init_h(); init_d4(); srand(42);

    printf("╔════════════════════════════════════════╗\n");
    printf("║   Pipeline Benchmark — Real Files      ║\n");
    printf("╚════════════════════════════════════════╝\n\n");

    /* Load real files */
    struct {const char *label,*path;} files[]={
        {"C source","fgls/test_diamond_flow_grouping.c"},
        {"C header","fgls/fibo_layer_header.h"},
        {"BMP image","../test_input.bmp"},
        {"Binary EXE","test_integrity_fixed.exe"},
        {"Compressed","fgls_handoff_fibolayer.zip"},
        {"Fold header","core/twin_core/pogls_fold.h"},
        {"Diamond fd","core/twin_core/geo_diamond_field.h"},
        {NULL,NULL}
    };

    for(int fi=0;files[fi].label;fi++){
        uint32_t len; uint8_t *buf=load_file(files[fi].path,&len);
        if(buf){
            bench_one(files[fi].label,buf,len);
            free(buf);
        } else printf("%-12s (cannot open)\n",files[fi].label);
    }
    return 0;
}

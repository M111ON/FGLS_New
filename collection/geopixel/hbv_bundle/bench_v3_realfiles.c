/*
 * bench_v3_realfiles.c — Test v3 pipeline on real file types
 * Build: gcc -O2 -I. -Icore/twin_core bench_v3_realfiles.c -lm -o bench_v3_realfiles
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pogls_fold.h"
#include "geo_diamond_field.h"

#define GEOM_THRESHOLD   4
#define SEED_POPCNT_MIN  8
#define ANTI_FAKE_BITS  16

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

static uint64_t chunk_isect_cached(const uint8_t *chunk, DiamondBlock *db_out){
    uint8_t face,edge,z;
    uint64_t seed=dseed(chunk);
    dcoord(seed,&face,&edge,&z);
    *db_out=fold_block_init(face,edge,(uint32_t)z<<16,1,0);
    memcpy(&db_out->core.raw,chunk,8);
    db_out->invert=~db_out->core.raw;
    fold_build_quad_mirror(db_out);
    return fold_fibo_intersect(db_out);
}

static int anti_fake_pass(const uint8_t *chunk){
    uint32_t seen=0;
    for(int i=0;i<64;i++) seen|=(1u<<chunk[i]);
    int ent=__builtin_popcount(seen);
    return (ent < 48);
}

typedef struct{uint8_t flag;uint64_t seed;uint8_t rid;uint8_t r[64];}Enc;
typedef struct{double ratio;double pct;double enc_ms,dec_ms;
    uint64_t n,ok,ns,nd,nl,nda,raw,enc,geom,flow_resets,anti_fake;}Metrics;

static Metrics run_pipe(const uint8_t*data,uint64_t N,SeedIndex*idx){
    Metrics m={0};m.n=N;m.raw=N*64;
    Enc*enc=calloc(N,sizeof(Enc));uint8_t*dec=calloc(N,64);
    uint64_t bl=diamond_baseline();DiamondFlowCtx flow;diamond_flow_init(&flow);
    struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);
    for(uint64_t i=0;i<N;i++){
        const uint8_t*c=data+i*64;
        DiamondBlock db;
        uint64_t isect=chunk_isect_cached(c,&db);
        int isect_pc=(int)__builtin_popcountll(isect);
        int has_geom=(isect_pc>=GEOM_THRESHOLD);
        if(has_geom) m.geom++;

        if(isect==0 && flow.hop_count>0){
            m.flow_resets++;
            diamond_flow_init(&flow);
        }

        if(!has_geom){
            uint8_t cn[64],hp[64];uint8_t rd=d4_canon_rid(c,cn);h_fwd(hp,cn);
            enc[i].flag=2;enc[i].rid=rd;memcpy(enc[i].r,hp,64);
            m.nl++;m.enc+=65;
        } else if(isect_pc>=SEED_POPCNT_MIN){
            if(!anti_fake_pass(c)){
                uint8_t cn[64],hp[64];uint8_t rd=d4_canon_rid(c,cn);h_fwd(hp,cn);
                enc[i].flag=2;enc[i].rid=rd;memcpy(enc[i].r,hp,64);
                m.nl++;m.enc+=65;m.anti_fake++;
            } else {
                uint8_t cn[64],hp[64];uint8_t rd=d4_canon_rid(c,cn);h_fwd(hp,cn);
                uint64_t s=fnv64(hp,64);sp(idx,s,hp,rd);
                enc[i].flag=0;enc[i].seed=s;enc[i].rid=rd;memcpy(enc[i].r,hp,64);
                flow.route_addr=diamond_route_update(flow.route_addr,isect);
                flow.hop_count++;
                m.ns++;m.enc+=9;
            }
        } else {
            flow.route_addr=diamond_route_update(flow.route_addr,isect);
            flow.hop_count++;
            FlowEndReason reason=diamond_flow_end(
                &db,bl,isect,flow.route_addr,flow.hop_count,flow.drift_acc);
            if(reason!=FLOW_CONTINUE){
                uint8_t cn[64],hp[64];uint8_t rd=d4_canon_rid(c,cn);h_fwd(hp,cn);
                uint64_t s=fnv64(hp,64);sp(idx,s,hp,rd);
                enc[i].flag=3;enc[i].seed=s;enc[i].rid=rd;memcpy(enc[i].r,hp,64);
                m.nda++;m.enc+=9;diamond_flow_init(&flow);m.flow_resets++;
            } else {
                uint8_t cn[64],hp[64];uint8_t rd=d4_canon_rid(c,cn);h_fwd(hp,cn);
                enc[i].flag=1;enc[i].rid=rd;memcpy(enc[i].r,hp,64);
                m.nd++;m.enc+=17;
            }
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
    printf("%-12s ", label);
    printf("N=%5llu  ratio=%5.3fx  geom=%3.0f%%  seed=%3.0f%%  exact=%4.1f%%  resets=%llu  fake=%llu\n",
           (unsigned long long)N, m.ratio,
           (double)m.geom*100.0/(double)N,
           (double)m.ns*100.0/(double)N,
           m.pct,
           (unsigned long long)m.flow_resets,
           (unsigned long long)m.anti_fake);

    /* Shuffle test */
    uint8_t *shuf=malloc(N*64);
    for(uint64_t i=0;i<N;i++){
        uint64_t j=i+(uint64_t)rand()%(N-i);
        memcpy(shuf,buf+i*64,64);memcpy(buf+i*64,buf+j*64,64);memcpy(buf+j*64,shuf,64);
    }
    free(shuf);
    SeedIndex *idx2=sx();
    Metrics m2=run_pipe(buf,N,idx2);
    double drop=m.ratio>0?(1.0-m2.ratio/m.ratio)*100.0:0;
    printf("  └─ shuffled               ratio=%5.3fx  drop=%+.1f%%  resets=%llu  fake=%llu\n",
           m2.ratio, drop, (unsigned long long)m2.flow_resets, (unsigned long long)m2.anti_fake);

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

    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║   V3 Pipeline Benchmark — Real Files             ║\n");
    printf("║   Gate: isect popcnt (G=%d S=%d)  Anti-fake=16 ║\n",
           GEOM_THRESHOLD, SEED_POPCNT_MIN, ANTI_FAKE_BITS);
    printf("╚══════════════════════════════════════════════════╝\n\n");

    /* entropy distribution probe */
    printf("── entropy distribution (unique bytes per 64B chunk) ──\n");
    {
        uint32_t tl;
        uint8_t *hdr=load_file("core/twin_core/geo_diamond_field.h",&tl);
        if(hdr){
            int eb[65]={0};
            for(uint64_t i=0;i+64<=tl;i+=64){
                uint32_t seen=0;for(int j=0;j<64;j++)seen|=(1u<<hdr[i+j]);
                eb[__builtin_popcount(seen)]++;
            }
            printf("  Diamond fd entropy: ");
            for(int b=0;b<=64;b++) if(eb[b]) printf("[%d]=%d ",b,eb[b]);
            printf("\n");
            free(hdr);
        }
        uint8_t *bin=load_file("test_v3.exe",&tl);
        if(bin){
            int eb[65]={0};
            for(uint64_t i=0;i+64<=tl;i+=64){
                uint32_t seen=0;for(int j=0;j<64;j++)seen|=(1u<<bin[i+j]);
                eb[__builtin_popcount(seen)]++;
            }
            printf("  Binary EXE entropy: ");
            for(int b=0;b<=64;b++) if(eb[b]) printf("[%d]=%d ",b,eb[b]);
            printf("\n");
            free(bin);
        }
    }
    printf("\n");

    struct {const char *label,*path;} files[]={
        {"C source","fgls/test_diamond_flow_grouping.c"},
        {"C header","fgls/fibo_layer_header.h"},
        {"BMP image","../test_input.bmp"},
        {"Binary EXE","test_v3.exe"},
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

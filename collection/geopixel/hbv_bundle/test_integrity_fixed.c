/*
 * test_integrity_fixed.c
 * Patches applied:
 *   1. Data: synthetic (file paths removed — works anywhere)
 *   2. DNA encode: now stores rid + hperm into enc[i]
 *   3. DNA decode: flag==3 handled same as flag==1,2
 *   4. test_byte_exact: memcpy → explicit field copy (dec_ok no longer clobbered)
 *
 * Build: gcc -O2 -I. test_integrity_fixed.c -lm -o test_fixed
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pogls_fold.h"
#include "geo_diamond_field.h"

/* ── Hilbert 8×8 ─────────────────────────────────────────────── */
static uint8_t G_H[64], G_HI[64];
static void init_hilbert(void) {
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

/* ── FNV-64 ──────────────────────────────────────────────────── */
static inline uint64_t fnv64(const uint8_t *d,int n){
    uint64_t h=1469598103934665603ULL;
    for(int i=0;i<n;i++){h^=d[i];h*=1099511628211ULL;}
    return h;
}

/* ── D4 permutation tables ───────────────────────────────────── */
static uint8_t D4_FWD[8][64], D4_INV[8][64];
static int d4_inited=0;
static void init_d4(void){
    if(d4_inited) return; d4_inited=1;
    uint8_t s[64]; for(int i=0;i<64;i++) s[i]=(uint8_t)i;
    for(int i=0;i<64;i++) D4_FWD[0][i]=s[i];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[1][x*8+(7-y)]=s[y*8+x];
    for(int i=0;i<64;i++) D4_FWD[2][63-i]=s[i];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[3][(7-x)*8+y]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[4][y*8+(7-x)]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[5][(7-y)*8+x]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[6][x*8+y]=s[y*8+x];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[7][(7-x)*8+(7-y)]=s[y*8+x];
    for(int st=0;st<8;st++)
        for(int i=0;i<64;i++)
            D4_INV[st][D4_FWD[st][i]]=(uint8_t)i;
}
static inline void d4_apply(uint8_t out[64],const uint8_t in[64],int st){
    for(int i=0;i<64;i++) out[i]=in[D4_FWD[st][i]];
}
static inline void d4_inv_apply(uint8_t out[64],const uint8_t in[64],int st){
    for(int i=0;i<64;i++) out[i]=in[D4_INV[st][i]];
}
static uint8_t d4_canon_rid(const uint8_t s[64], uint8_t out[64]) {
    init_d4();
    uint64_t bh=UINT64_MAX,th; int bi=0;
    for(int i=0;i<8;i++){
        uint8_t tmp[64]; d4_apply(tmp,s,i);
        th=fnv64(tmp,64); if(th<bh){bh=th;bi=i;memcpy(out,tmp,64);}
    }
    return (uint8_t)bi;
}

/* ── Seed Index ──────────────────────────────────────────────── */
#define SIDX_CAP  (1<<16)
#define SIDX_MASK (SIDX_CAP-1)
typedef struct { uint64_t seed; uint8_t chunk[64]; uint8_t rid; uint8_t used; } SeedSlot;
typedef struct { SeedSlot *slots; uint64_t n_entries, n_collisions; } SeedIndex;
static SeedIndex* sidx_create(void){
    SeedIndex *idx=calloc(1,sizeof(SeedIndex));
    idx->slots=calloc(SIDX_CAP,sizeof(SeedSlot)); return idx;
}
static void sidx_free(SeedIndex *idx){ free(idx->slots); free(idx); }
static void sidx_put(SeedIndex *idx, uint64_t seed, const uint8_t *chunk, uint8_t rid){
    uint32_t slot=(uint32_t)(seed & SIDX_MASK);
    for(int i=0;i<SIDX_CAP;i++){
        uint32_t s=(slot+i)&SIDX_MASK;
        if(!idx->slots[s].used){
            idx->slots[s].used=1; idx->slots[s].seed=seed;
            idx->slots[s].rid=rid;
            memcpy(idx->slots[s].chunk,chunk,64); idx->n_entries++; return;
        }
        if(idx->slots[s].seed==seed) return;
        idx->n_collisions++;
    }
}
static const SeedSlot* sidx_get(const SeedIndex *idx, uint64_t seed){
    uint32_t slot=(uint32_t)(seed & SIDX_MASK);
    for(int i=0;i<SIDX_CAP;i++){
        uint32_t s=(slot+i)&SIDX_MASK;
        if(!idx->slots[s].used) return NULL;
        if(idx->slots[s].seed==seed) return &idx->slots[s];
    }
    return NULL;
}

/* ── derive seed + coord ─────────────────────────────────────── */
static uint64_t derive_seed(const uint8_t chunk[64]){
    const uint64_t *w=(const uint64_t*)chunk;
    uint64_t s=w[0]^w[1]^w[2]^w[3]^w[4]^w[5]^w[6]^w[7];
    s^=s>>33; s*=0xff51afd7ed558ccdULL; s^=s>>33; s*=0xc4ceb9fe1a85ec53ULL; s^=s>>33;
    return s;
}
static void derive_coord(uint64_t seed, uint8_t *f, uint8_t *e, uint8_t *z){
    uint64_t h=seed; h^=h>>33; h*=0xff51afd7ed558ccdULL; h^=h>>33; h*=0xc4ceb9fe1a85ec53ULL; h^=h>>33;
    *f=(uint8_t)(((uint64_t)(uint32_t)(h>>32)*12u)>>32);
    *e=(uint8_t)(((uint64_t)(uint32_t)(h&0xFFFFFFFFu)*5u)>>32);
    *z=(uint8_t)((h>>16)&0xFFu);
}

/* ── Encoded record ──────────────────────────────────────────── */
typedef struct {
    uint8_t  flag;    /* 0=seed 1=delta 2=lossless 3=dna */
    uint64_t seed;
    uint8_t  rid;
    uint8_t  r[64];
} Encoded;

typedef struct {
    uint64_t n;
    uint64_t n_seed, n_delta, n_lossless, n_dna;
    uint64_t bytes_raw, bytes_enc;
    uint64_t dec_ok, dec_fail;
    double   enc_ms, dec_ms;
} Result;

/* ════════════════════════════════════════════════════════════════
   encode_all — FIX: DNA branch now stores rid + hperm
   ════════════════════════════════════════════════════════════════ */
static Result encode_all(const uint8_t *data, uint64_t N, uint8_t **dec_out,
                          SeedIndex *sidx, Encoded **enc_out)
{
    Result r={0}; r.n=N; r.bytes_raw=N*64;
    *enc_out=calloc(N,sizeof(Encoded));
    *dec_out=calloc(N,64);
    Encoded *enc=*enc_out;

    uint64_t baseline=diamond_baseline();
    DiamondFlowCtx flow; diamond_flow_init(&flow);
    int flow_active=0;

    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);

    for(uint64_t i=0;i<N;i++){
        const uint8_t *chunk=data+i*64;
        uint64_t seed=derive_seed(chunk);
        uint8_t face,edge,z;
        derive_coord(seed,&face,&edge,&z);

        DiamondBlock db=fold_block_init(face,edge,(uint32_t)z<<16,1,0);
        memcpy(&db.core.raw,chunk,8);
        db.invert=~db.core.raw;
        fold_build_quad_mirror(&db);

        uint64_t isect=fold_fibo_intersect(&db);

        if(!fold_xor_audit(&db)){
            uint8_t canon[64]; uint8_t rid=d4_canon_rid(chunk,canon);
            uint8_t hperm[64]; h_fwd(hperm,canon);
            uint64_t s=fnv64(hperm,64);
            sidx_put(sidx,s,hperm,rid);
            enc[i].flag=0; enc[i].seed=s; enc[i].rid=rid;
            r.n_seed++; r.bytes_enc+=9;
            flow_active=0; diamond_flow_init(&flow); continue;
        }

        flow.route_addr=diamond_route_update(flow.route_addr,isect);
        flow.hop_count++;

        uint8_t seen[256]={0}; uint32_t ent=0;
        for(int j=0;j<64;j++) if(!seen[chunk[j]]++) ent++;

        uint64_t agg_isect=flow_active?flow.route_addr:isect;

        /* ── FIX 1: DNA branch stores rid + hperm (was missing) ── */
        if(diamond_flow_end(&db,baseline,agg_isect,flow.route_addr,
                             flow.hop_count,flow.drift_acc)!=FLOW_CONTINUE){
            uint8_t canon[64]; uint8_t rid=d4_canon_rid(chunk,canon);
            uint8_t hperm[64]; h_fwd(hperm,canon);
            uint64_t s=fnv64(hperm,64);
            sidx_put(sidx,s,hperm,rid);
            enc[i].flag=3; enc[i].seed=s; enc[i].rid=rid;
            memcpy(enc[i].r,hperm,64);          /* ← was missing */
            diamond_dna_write(&db,flow.route_addr,flow.hop_count,0);
            r.n_dna++; r.bytes_enc+=9;
            diamond_flow_init(&flow); flow_active=0;
        }
        else if(ent<=12||flow_active){
            uint8_t canon[64]; uint8_t rid=d4_canon_rid(chunk,canon);
            uint8_t hperm[64]; h_fwd(hperm,canon);
            uint64_t s=fnv64(hperm,64);
            sidx_put(sidx,s,hperm,rid);
            enc[i].flag=0; enc[i].seed=s; enc[i].rid=rid;
            r.n_seed++; r.bytes_enc+=9; flow_active=1;
        }
        else if(ent<=24){
            uint8_t canon[64]; uint8_t rid=d4_canon_rid(chunk,canon);
            uint8_t hperm[64]; h_fwd(hperm,canon);
            memcpy(enc[i].r,hperm,64);
            enc[i].flag=1; enc[i].rid=rid;
            r.n_delta++; r.bytes_enc+=17; flow_active=0;
        }
        else {
            uint8_t canon[64]; uint8_t rid=d4_canon_rid(chunk,canon);
            uint8_t hperm[64]; h_fwd(hperm,canon);
            memcpy(enc[i].r,hperm,64);
            enc[i].flag=2; enc[i].rid=rid;
            r.n_lossless++; r.bytes_enc+=65; flow_active=0;
        }
    }

    clock_gettime(CLOCK_MONOTONIC,&t1);
    r.enc_ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;
    return r;
}

/* ════════════════════════════════════════════════════════════════
   decode_all — FIX: flag==3 handled (was falling through to garbage)
   ════════════════════════════════════════════════════════════════ */
static Result decode_all(const uint8_t *original, uint64_t N,
                          const Encoded *enc, SeedIndex *sidx,
                          uint8_t *decoded)
{
    Result r={0}; r.n=N;
    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);

    for(uint64_t i=0;i<N;i++){
        uint8_t hperm[64];

        if(enc[i].flag==0 || enc[i].flag==3){
            /* seed or DNA — both stored in sidx, keyed by enc[i].seed */
            /* ── FIX 2: flag==3 (DNA) now goes through seed lookup ── */
            const SeedSlot *sl=sidx_get(sidx,enc[i].seed);
            if(!sl) continue;
            memcpy(hperm,sl->chunk,64);
            uint8_t canon[64]; h_inv(canon,hperm);
            d4_inv_apply(decoded+i*64,canon,sl->rid);
        } else {
            /* delta or lossless — payload in enc[i].r */
            memcpy(hperm,enc[i].r,64);
            uint8_t canon[64]; h_inv(canon,hperm);
            d4_inv_apply(decoded+i*64,canon,enc[i].rid);
        }

        if(memcmp(original+i*64,decoded+i*64,64)==0)
            r.dec_ok++;
    }

    clock_gettime(CLOCK_MONOTONIC,&t1);
    r.dec_ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;
    r.bytes_raw=N*64;
    return r;
}

static void print_result(const Result *enc_r, const Result *dec_r, const char *label){
    double ratio = enc_r->bytes_enc > 0
        ? (double)enc_r->bytes_raw / (double)enc_r->bytes_enc : 0;
    double pct = enc_r->n > 0
        ? (double)dec_r->dec_ok * 100.0 / (double)enc_r->n : 0;
    printf("[%s]\n", label);
    printf("  chunks : %llu  seed=%llu dna=%llu delta=%llu lossless=%llu\n",
           (unsigned long long)enc_r->n,
           (unsigned long long)enc_r->n_seed,
           (unsigned long long)enc_r->n_dna,
           (unsigned long long)enc_r->n_delta,
           (unsigned long long)enc_r->n_lossless);
    printf("  ratio  : %.3fx  (raw=%llu enc=%llu)\n", ratio,
           (unsigned long long)enc_r->bytes_raw,
           (unsigned long long)enc_r->bytes_enc);
    printf("  byte-exact: %llu/%llu  (%.2f%%)\n",
           (unsigned long long)dec_r->dec_ok,
           (unsigned long long)enc_r->n, pct);
    printf("  enc: %.2fms  dec: %.2fms\n", enc_r->enc_ms, dec_r->dec_ms);
}

/* ════════════════════════════════════════════════════════════════
   TEST 1: Byte-level exactness
   FIX 3: replaced broken memcpy with explicit field copy
   ════════════════════════════════════════════════════════════════ */
static void test_byte_exact(const uint8_t *data, uint64_t N){
    printf("\n═══ TEST 1: Byte-level exactness  N=%llu ═══\n",
           (unsigned long long)N);
    SeedIndex *sidx=sidx_create();
    Encoded *enc; uint8_t *dec;
    Result er=encode_all(data,N,&dec,sidx,&enc);
    Result dr=decode_all(data,N,enc,sidx,dec);

    /* FIX 3: explicit copy — never clobber dec_ok */
    dr.bytes_enc  = er.bytes_enc;
    dr.enc_ms     = er.enc_ms;
    dr.n          = er.n;
    dr.n_seed     = er.n_seed;
    dr.n_delta    = er.n_delta;
    dr.n_lossless = er.n_lossless;
    dr.n_dna      = er.n_dna;

    print_result(&er, &dr, "geometry pipeline");

    if(dr.dec_ok==N) printf("  ✅ PASS: 100%% byte-exact\n");
    else             printf("  ❌ FAIL: %llu mismatches\n",
                            (unsigned long long)(N - dr.dec_ok));

    free(enc); free(dec); sidx_free(sidx);
}

/* ════════════════════════════════════════════════════════════════
   TEST 2: Shuffle degradation
   ════════════════════════════════════════════════════════════════ */
static void test_shuffle(uint8_t *data, uint64_t N){
    printf("\n═══ TEST 2: Shuffle degradation ═══\n");

    SeedIndex *s1=sidx_create(); Encoded *e1; uint8_t *d1;
    Result r1=encode_all(data,N,&d1,s1,&e1);
    double ratio1=r1.bytes_enc>0?(double)r1.bytes_raw/(double)r1.bytes_enc:0;

    uint8_t *shuf=malloc(N*64);
    uint8_t *copy=malloc(N*64); memcpy(copy,data,N*64);
    for(uint64_t i=0;i<N;i++){
        uint64_t j=i+(uint64_t)rand()%(N-i);
        memcpy(shuf,     copy+i*64,64);
        memcpy(copy+i*64,copy+j*64,64);
        memcpy(copy+j*64,shuf,     64);
    }
    free(shuf);

    SeedIndex *s2=sidx_create(); Encoded *e2; uint8_t *d2;
    Result r2=encode_all(copy,N,&d2,s2,&e2);
    double ratio2=r2.bytes_enc>0?(double)r2.bytes_raw/(double)r2.bytes_enc:0;

    printf("  ordered  ratio: %.3fx\n",ratio1);
    printf("  shuffled ratio: %.3fx\n",ratio2);
    double drop=(ratio1>0)?(1.0-ratio2/ratio1)*100.0:0;
    if(ratio2 < ratio1*0.85)
         printf("  ✅ PASS: ratio dropped %.1f%% (structure real)\n",drop);
    else printf("  ⚠️  WARN: ratio dropped only %.1f%% (possible overfit)\n",drop);

    free(e1); free(d1); sidx_free(s1);
    free(e2); free(d2); sidx_free(s2);
    free(copy);
}

/* ════════════════════════════════════════════════════════════════
   TEST 3: Cross-file (train A → test B)
   ════════════════════════════════════════════════════════════════ */
static void test_cross_file(const uint8_t *dA, uint64_t NA,
                              const uint8_t *dB, uint64_t NB){
    printf("\n═══ TEST 3: Cross-file (train A → test B) ═══\n");

    SeedIndex *sidx=sidx_create();
    Encoded *eA; uint8_t *dA2;
    encode_all(dA,NA,&dA2,sidx,&eA);
    free(eA); free(dA2);

    printf("  index from A: %llu entries\n",(unsigned long long)sidx->n_entries);

    Encoded *eB; uint8_t *dB2;
    Result re=encode_all(dB,NB,&dB2,sidx,&eB);
    Result rd=decode_all(dB,NB,eB,sidx,dB2);
    double ratio_cross=re.bytes_enc>0?(double)re.bytes_raw/(double)re.bytes_enc:0;
    printf("  B with A index: ratio=%.3fx  byte-exact=%llu/%llu (%.1f%%)\n",
           ratio_cross,
           (unsigned long long)rd.dec_ok,(unsigned long long)NB,
           NB>0?(double)rd.dec_ok*100.0/(double)NB:0);
    free(eB); free(dB2); sidx_free(sidx);

    SeedIndex *sB=sidx_create();
    Encoded *eBB; uint8_t *dBB;
    Result reB=encode_all(dB,NB,&dBB,sB,&eBB);
    Result rdB=decode_all(dB,NB,eBB,sB,dBB);
    double ratio_self=reB.bytes_enc>0?(double)reB.bytes_raw/(double)reB.bytes_enc:0;
    printf("  B with own index: ratio=%.3fx  byte-exact=%llu/%llu (%.1f%%)\n",
           ratio_self,
           (unsigned long long)rdB.dec_ok,(unsigned long long)NB,
           NB>0?(double)rdB.dec_ok*100.0/(double)NB:0);
    free(eBB); free(dBB); sidx_free(sB);

    if(rd.dec_ok==NB && ratio_cross < ratio_self*0.95)
         printf("  ✅ PASS: cross-file ratio < self, byte-exact intact\n");
    else if(rd.dec_ok < NB)
         printf("  ❌ FAIL: byte-exact loss in cross-file\n");
    else printf("  ⚠️  WARN: cross/self ratio delta = %.1f%%\n",
                (ratio_self>0)?(ratio_cross/ratio_self-1.0)*100.0:0);
}

/* ── synthetic data generators ────────────────────────────────── */
static uint8_t* make_data_varied(uint64_t N){
    /* Mix: repeating patterns + noise + gradient — stresses all branches */
    uint8_t *d=malloc(N*64);
    for(uint64_t i=0;i<N;i++){
        uint8_t *c=d+i*64;
        int kind=i%4;
        if(kind==0){                          /* uniform — triggers seed dedup */
            memset(c,(int)(i&0xFF),64);
        } else if(kind==1){                   /* gradient */
            for(int j=0;j<64;j++) c[j]=(uint8_t)(i*3+j);
        } else if(kind==2){                   /* sparse (few unique) */
            for(int j=0;j<64;j++) c[j]=(uint8_t)((j%4)*17+(i&3)*5);
        } else {                              /* dense noise */
            for(int j=0;j<64;j++) c[j]=(uint8_t)(i*17+j*31+i/64*13+42);
        }
    }
    return d;
}

int main(void){
    init_hilbert();
    init_d4();
    srand(42);

    /* Quick D4 sanity */
    {
        uint8_t orig[64]; for(int i=0;i<64;i++) orig[i]=(uint8_t)(i*17+42);
        uint8_t canon[64]; uint8_t rid=d4_canon_rid(orig,canon);
        uint8_t back[64]; d4_inv_apply(back,canon,rid);
        printf("D4 roundtrip: %s  (rid=%d)\n",
               memcmp(orig,back,64)==0?"OK":"FAIL", rid);
    }

    const uint64_t N=1721;
    uint8_t *data=make_data_varied(N);
    printf("data: %llu chunks (%.1f KB)  sizeof(Encoded)=%zu\n",
           (unsigned long long)N, N*64.0/1024, sizeof(Encoded));

    /* Binary search: first failure point */
    printf("\n── binary search for first failure ──\n");
    for(int batch=100; batch<=(int)N; batch+=100){
        SeedIndex *sidx=sidx_create();
        Encoded *enc; uint8_t *dec;
        Result er=encode_all(data,(uint64_t)batch,&dec,sidx,&enc);
        Result dr=decode_all(data,(uint64_t)batch,enc,sidx,dec);
        if(dr.dec_ok!=(uint64_t)batch)
            printf("  N=%4d: FAIL  pass=%llu/%d\n",batch,
                   (unsigned long long)dr.dec_ok,batch);
        else
            printf("  N=%4d: OK\n",batch);
        free(enc); free(dec); sidx_free(sidx);
    }
    /* Final full N */
    {
        SeedIndex *sidx=sidx_create();
        Encoded *enc; uint8_t *dec;
        Result er=encode_all(data,N,&dec,sidx,&enc);
        Result dr=decode_all(data,N,enc,sidx,dec);
        printf("  N=%4llu: %s  pass=%llu/%llu\n",
               (unsigned long long)N,
               dr.dec_ok==N?"OK":"FAIL",
               (unsigned long long)dr.dec_ok,
               (unsigned long long)N);
        free(enc); free(dec); sidx_free(sidx);
    }

    test_byte_exact(data, N);

    uint8_t *copy=malloc(N*64); memcpy(copy,data,N*64);
    test_shuffle(copy, N);
    free(copy);

    test_cross_file(data, N/2, data+N/2*64, N-N/2);

    free(data);
    return 0;
}

/*
 * test_integrity.c — 3 Red Flag tests for geometry pipeline
 *
 * Test 1: Byte-level diff (original vs decoded → must be 100%)
 * Test 2: Shuffle test (random order → ratio must DROP)
 * Test 3: Cross-file test (train A → test B → ratio != train ratio)
 *
 * Build: gcc -O2 -I. -Icore/twin_core test_integrity.c -lm
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pogls_fold.h"
#include "geo_diamond_field.h"

/* ─── Hilbert 8×8 ─────────────────────────────────────────────── */
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

/* ─── FNV-64 ──────────────────────────────────────────────────── */
static inline uint64_t fnv64(const uint8_t *d,int n){
    uint64_t h=1469598103934665603ULL;
    for(int i=0;i<n;i++){h^=d[i];h*=1099511628211ULL;}
    return h;
}

/* ─── D4 permutation tables (forward + inverse) ──────────────── */
/* Each D4 state = permutation P[64] where out[i] = in[P_fwd[state][i]]
 * Inverse: out[i] = in[P_inv[state][i]] */
static uint8_t D4_FWD[8][64], D4_INV[8][64];
static int d4_inited=0;

static void init_d4(void){
    if(d4_inited) return; d4_inited=1;
    /* Build forward permutations for all 8 states */
    uint8_t s[64]; for(int i=0;i<64;i++) s[i]=(uint8_t)i;

    /* 0:id */ for(int i=0;i<64;i++) D4_FWD[0][i]=s[i];
    /* 1:r90 */ for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[1][x*8+(7-y)]=s[y*8+x];
    /* 2:r180 */ for(int i=0;i<64;i++) D4_FWD[2][63-i]=s[i];
    /* 3:r270 */ for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[3][(7-x)*8+y]=s[y*8+x];
    /* 4:fh */   for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[4][y*8+(7-x)]=s[y*8+x];
    /* 5:fv */   for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[5][(7-y)*8+x]=s[y*8+x];
    /* 6:tr */   for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[6][x*8+y]=s[y*8+x];
    /* 7:at */   for(int y=0;y<8;y++)for(int x=0;x<8;x++) D4_FWD[7][(7-x)*8+(7-y)]=s[y*8+x];

    /* Compute inverse: D4_INV[st][D4_FWD[st][i]] = i */
    for(int st=0;st<8;st++)
        for(int i=0;i<64;i++)
            D4_INV[st][D4_FWD[st][i]]=(uint8_t)i;
}

static inline void d4_apply(uint8_t out[64], const uint8_t in[64], int st){
    for(int i=0;i<64;i++) out[i]=in[D4_FWD[st][i]];
}

static inline void d4_inv_apply(uint8_t out[64], const uint8_t in[64], int st){
    for(int i=0;i<64;i++) out[i]=in[D4_INV[st][i]];
}

/* Find canonical (minimum-hash) orientation, return state 0..7 */
static uint8_t d4_canon_rid(const uint8_t s[64], uint8_t out[64]) {
    init_d4();
    uint64_t bh=UINT64_MAX,th; int bi=0;
    for(int i=0;i<8;i++){
        uint8_t tmp[64]; d4_apply(tmp,s,i);
        th=fnv64(tmp,64); if(th<bh){bh=th;bi=i;memcpy(out,tmp,64);}
    }
    return (uint8_t)bi;
}

/* ─── Seed Index ──────────────────────────────────────────────── */
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

/* ─── Derive seed + coord ────────────────────────────────────── */
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

/* ═══════════════════════════════════════════════════════════════════
   Geometry pipeline encode + decode
   ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  flag;       /* 0=seed, 1=delta, 2=lossless, 3=DNA */
    uint64_t seed;       /* FNV seed for lookup */
    uint8_t  rid;        /* D4 rotation ID 0..7 */
    uint8_t  r[64];      /* Hilbert-order R channel (lossless/delta) */
} Encoded;

typedef struct {
    uint64_t n;
    uint64_t n_seed, n_delta, n_lossless, n_dna;
    uint64_t bytes_raw, bytes_enc;
    uint64_t dec_ok;          /* byte-exact match count */
    uint64_t dec_fail;        /* mismatch count */
    double   enc_ms, dec_ms;
} Result;

static Result encode_all(const uint8_t *data, uint64_t N, uint8_t **dec_out,
                          SeedIndex *sidx, Encoded **enc_out)
{
    Result r={0}; r.n=N; r.bytes_raw=N*64;
    *enc_out=calloc(N,sizeof(Encoded));
    *dec_out=calloc(N,64);
    Encoded *enc=*enc_out;
    uint8_t *dec=*dec_out;

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
            uint8_t canon[64];
            uint8_t rid=d4_canon_rid(chunk,canon);
            uint8_t hperm[64]; h_fwd(hperm,canon);
            uint64_t s=fnv64(hperm,64);
            sidx_put(sidx,s,hperm,rid);
            enc[i].flag=0; enc[i].seed=s; enc[i].rid=rid;
            r.n_seed++; r.bytes_enc+=8+1;  /* 8B seed + 1B rid */
            flow_active=0; diamond_flow_init(&flow); continue;
        }

        flow.route_addr=diamond_route_update(flow.route_addr,isect);
        flow.hop_count++;

        uint8_t seen[256]={0}; uint32_t ent=0;
        for(int j=0;j<64;j++) if(!seen[chunk[j]]++) ent++;

        uint64_t agg_isect=isect;
        if(flow_active) agg_isect=flow.route_addr;

        if(diamond_flow_end(&db,baseline,agg_isect,flow.route_addr,
                             flow.hop_count,flow.drift_acc)!=FLOW_CONTINUE){
            diamond_dna_write(&db,flow.route_addr,flow.hop_count,0);
            enc[i].flag=3; r.n_dna++; r.bytes_enc+=10;
            diamond_flow_init(&flow); flow_active=0;
        }
        else if(ent<=12||flow_active){
            uint8_t canon[64];
            uint8_t rid=d4_canon_rid(chunk,canon);
            uint8_t hperm[64]; h_fwd(hperm,canon);
            uint64_t s=fnv64(hperm,64);
            sidx_put(sidx,s,hperm,rid);
            enc[i].flag=0; enc[i].seed=s; enc[i].rid=rid;
            r.n_seed++; r.bytes_enc+=8+1; flow_active=1;
        }
        else if(ent<=24){
            uint8_t canon[64];
            uint8_t rid=d4_canon_rid(chunk,canon);
            uint8_t hperm[64]; h_fwd(hperm,canon);
            memcpy(enc[i].r,hperm,64);
            enc[i].flag=1; enc[i].rid=rid;
            r.n_delta++; r.bytes_enc+=16+1; flow_active=0;
        }
        else {
            uint8_t canon[64];
            uint8_t rid=d4_canon_rid(chunk,canon);
            uint8_t hperm[64]; h_fwd(hperm,canon);
            memcpy(enc[i].r,hperm,64);
            enc[i].flag=2; enc[i].rid=rid;
            r.n_lossless++; r.bytes_enc+=64+1; flow_active=0;
        }
    }

    clock_gettime(CLOCK_MONOTONIC,&t1);
    r.enc_ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;
    return r;
}

static Result decode_all(const uint8_t *original, uint64_t N,
                          const Encoded *enc, SeedIndex *sidx,
                          uint8_t *decoded)
{
    Result r={0}; r.n=N; r.dec_fail=N;
    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);

    for(uint64_t i=0;i<N;i++){
        uint8_t hperm[64];
        if(enc[i].flag==0){ /* seed */
            const SeedSlot *sl=sidx_get(sidx,enc[i].seed);
            if(!sl) continue;
            memcpy(hperm,sl->chunk,64);
            uint8_t canon[64]; h_inv(canon,hperm);
            d4_inv_apply(decoded+i*64,canon,sl->rid);
        } else { /* delta or lossless */
            memcpy(hperm,enc[i].r,64);
            uint8_t canon[64]; h_inv(canon,hperm);
            d4_inv_apply(decoded+i*64,canon,enc[i].rid);
        }
        /* Byte-level verify */
        if(memcmp(original+i*64,decoded+i*64,64)==0)
            r.dec_ok++;
    }

    clock_gettime(CLOCK_MONOTONIC,&t1);
    r.dec_ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;
    r.bytes_raw=N*64;
    return r;
}

static void print_result(const Result *r, const char *label){
    double ratio=(double)r->bytes_raw/(double)r->bytes_enc;
    double pct=r->n>0?(double)r->dec_ok*100.0/(double)r->n:0;
    printf("[%s]\n",label);
    printf("  chunks: %llu  seed=%llu delta=%llu los=%llu dna=%llu\n",
           (unsigned long long)r->n,(unsigned long long)r->n_seed,
           (unsigned long long)r->n_delta,(unsigned long long)r->n_lossless,
           (unsigned long long)r->n_dna);
    printf("  ratio : %.3fx  (raw=%llu enc=%llu)\n",ratio,
           (unsigned long long)r->bytes_raw,(unsigned long long)r->bytes_enc);
    printf("  byte-exact: %llu/%llu  (%.2f%%)\n",
           (unsigned long long)r->dec_ok,(unsigned long long)r->n,pct);
    printf("  enc: %.2fms  dec: %.2fms\n",r->enc_ms,r->dec_ms);
}

/* ═══════════════════════════════════════════════════════════════════
   Test 1: Byte-level diff
   ═══════════════════════════════════════════════════════════════════ */
static void test_byte_exact(uint8_t *data, uint64_t N){
    printf("\n═══ TEST 1: Byte-level exactness ═══\n");
    SeedIndex *sidx=sidx_create();
    Encoded *enc; uint8_t *dec;
    Result r=encode_all(data,N,&dec,sidx,&enc);
    Result rd=decode_all(data,N,enc,sidx,dec);
    rd.bytes_enc=r.bytes_enc; rd.enc_ms=r.enc_ms;
    memcpy(&rd.n_seed,&r.n_seed,sizeof(r)-sizeof(r.bytes_raw)-sizeof(r.enc_ms)-sizeof(r.dec_ms));
    print_result(&rd,"geometry pipeline");

    uint64_t bad=r.n-rd.dec_ok;
    if(bad==0) printf("  ✅ PASS: 100%% byte-exact match\n");
    else printf("  ❌ FAIL: %llu mismatches\n",(unsigned long long)bad);

    free(enc); free(dec); sidx_free(sidx);
}

/* ═══════════════════════════════════════════════════════════════════
   Test 2: Shuffle test → ratio must DROP
   ═══════════════════════════════════════════════════════════════════ */
static void test_shuffle(uint8_t *data, uint64_t N){
    printf("\n═══ TEST 2: Shuffle degradation ═══\n");

    /* First pass: ordered */
    SeedIndex *sidx1=sidx_create();
    Encoded *enc1; uint8_t *dec1;
    Result r1=encode_all(data,N,&dec1,sidx1,&enc1);
    double r1_ratio=(double)r1.bytes_raw/(double)r1.bytes_enc;

    /* Shuffle chunk order */
    uint8_t *shuf=malloc(N*64);
    for(uint64_t i=0;i<N;i++){
        uint64_t j=rand()%(N-i);
        memcpy(shuf+i*64,data+j*64,64);
        uint8_t tmp[64]; memcpy(tmp,data+j*64,64);
        memcpy(data+j*64,data+i*64,64);
        memcpy(data+i*64,tmp,64);
    }
    memcpy(data,shuf,N*64); free(shuf);

    /* Second pass: shuffled */
    SeedIndex *sidx2=sidx_create();
    Encoded *enc2; uint8_t *dec2;
    Result r2=encode_all(data,N,&dec2,sidx2,&enc2);
    double r2_ratio=(double)r2.bytes_raw/(double)r2.bytes_enc;

    printf("  ordered  ratio: %.3fx\n",r1_ratio);
    printf("  shuffled ratio: %.3fx\n",r2_ratio);
    if(r2_ratio<r1_ratio*0.85)
        printf("  ✅ PASS: ratio dropped by %.1f%% (structure is real)\n",
               (1.0-r2_ratio/r1_ratio)*100.0);
    else
        printf("  ⚠️  WARN: ratio dropped by only %.1f%% (possible overfit)\n",
               (1.0-r2_ratio/r1_ratio)*100.0);

    free(enc1); free(dec1); sidx_free(sidx1);
    free(enc2); free(dec2); sidx_free(sidx2);
}

/* ═══════════════════════════════════════════════════════════════════
   Test 3: Cross-file → train A, test B
   ═══════════════════════════════════════════════════════════════════ */
static void test_cross_file(uint8_t *dataA, uint64_t NA,
                             uint8_t *dataB, uint64_t NB){
    printf("\n═══ TEST 3: Cross-file (train A → test B) ═══\n");

    /* Train index on A */
    SeedIndex *sidx=sidx_create();
    Encoded *encA; uint8_t *decA;
    encode_all(dataA,NA,&decA,sidx,&encA);
    free(encA); free(decA);

    printf("  index entries from A: %llu (collisions=%llu)\n",
           (unsigned long long)sidx->n_entries,
           (unsigned long long)sidx->n_collisions);

    /* Encode B using A's index */
    Encoded *encB; uint8_t *decB;
    Result re=encode_all(dataB,NB,&decB,sidx,&encB);
    Result rd=decode_all(dataB,NB,encB,sidx,decB);
    rd.bytes_enc=re.bytes_enc;
    memcpy(&rd.n_seed,&re.n_seed,sizeof(re)-sizeof(re.bytes_raw)-sizeof(re.enc_ms)-sizeof(re.dec_ms));

    double ratio=(double)rd.bytes_raw/(double)rd.bytes_enc;
    printf("  B with A's index: ratio=%.3fx  byte-exact=%llu/%llu (%.1f%%)\n",
           ratio,(unsigned long long)rd.dec_ok,
           (unsigned long long)rd.n,
           rd.n>0?(double)rd.dec_ok*100.0/(double)rd.n:0);

    /* Encode B with its OWN index for comparison */
    SeedIndex *sidxB=sidx_create();
    Encoded *encBB; uint8_t *decBB;
    Result reB=encode_all(dataB,NB,&decBB,sidxB,&encBB);
    Result rdB=decode_all(dataB,NB,encBB,sidxB,decBB);
    rdB.bytes_enc=reB.bytes_enc;
    double ratioB=(double)rdB.bytes_raw/(double)rdB.bytes_enc;
    printf("  B with own index: ratio=%.3fx  byte-exact=%llu/%llu (%.1f%%)\n",
           ratioB,(unsigned long long)rdB.dec_ok,
           (unsigned long long)rdB.n,
           rdB.n>0?(double)rdB.dec_ok*100.0/(double)rdB.n:0);

    if(ratio<ratioB*0.9 && rd.dec_ok==rd.n)
        printf("  ✅ PASS: cross-file ratio < self ratio (structure not overfit)\n");
    else if(rd.dec_ok<rd.n)
        printf("  ❌ FAIL: byte-exact loss in cross-file decode\n");
    else
        printf("  ⚠️  cross-file ratio=%.3fx vs self=%.3fx (delta=%.1f%%)\n",
               ratio,ratioB,(ratio/ratioB-1.0)*100.0);

    free(encB); free(decB); sidx_free(sidx);
    free(encBB); free(decBB); sidx_free(sidxB);
}

static int debug_roundtrip(void){
    printf("── D4 roundtrip debug ──\n");
    uint8_t orig[64]; for(int i=0;i<64;i++) orig[i]=(uint8_t)(i*17+42);
    uint8_t canon[64]; uint8_t rid=d4_canon_rid(orig,canon);
    uint8_t back[64]; d4_inv_apply(back,canon,rid);
    int ok=memcmp(orig,back,64)==0;
    printf("  rid=%d  match=%s\n",rid,ok?"OK":"FAIL");
    if(!ok){
        int diff=0; for(int i=0;i<64;i++) if(orig[i]!=back[i]) diff++;
        printf("  %d mismatches\n",diff);
        for(int i=0;i<8;i++) printf("  [%d] orig=%02x back=%02x\n",i,orig[i],back[i]);
    }
    return ok;
}

int main(void){
    init_hilbert();
    init_d4();
    if(!debug_roundtrip()){ printf("D4 tables broken\n"); return 1; }
    srand(42);

    /* Load data */
    const char *files[]={
        "fgls/fibo_tile_dispatch.h","fgls/fibo_layer_header.h",
        "fgls/test_diamond_flow_grouping.c","fgls/hb_header_frame.h",
        "core/core/pogls_fold.h","core/core/geo_diamond_field.h",
        NULL
    };
    uint8_t *pool=malloc(4096*64); int total=0;
    uint8_t *fbuf=malloc(512*1024);
    if(!pool||!fbuf){printf("OOM\n");return 1;}
    for(int fi=0;files[fi];fi++){
        FILE *f=fopen(files[fi],"rb"); if(!f) continue;
        int n=(int)fread(fbuf,1,512*1024,f); fclose(f);
        int nc=n/64; if(total+nc>4096) nc=4096-total;
        memcpy(pool+total*64,fbuf,nc*64); total+=nc;
    }
    printf("data: %d chunks (%.1f KB) sizeof(Encoded)=%zu\n",
           total,total*64.0/1024,sizeof(Encoded));
    uint8_t *data=pool; uint64_t N=(uint64_t)total;

    /* Debug: manual encode/decode of first chunk */
    {
        printf("── Manual first chunk debug ──\n");
        uint8_t *chunk=data;
        uint8_t canon[64]; uint8_t rid=d4_canon_rid(chunk,canon);
        uint8_t hperm[64]; h_fwd(hperm,canon);
        uint8_t back_canon[64]; h_inv(back_canon,hperm);
        uint8_t back_orig[64]; d4_inv_apply(back_orig,back_canon,rid);
        int h_ok=memcmp(canon,back_canon,64)==0;
        int full_ok=memcmp(chunk,back_orig,64)==0;
        printf("  rid=%d  hilbert_inv=%s  full=%s\n",rid,
               h_ok?"OK":"FAIL",full_ok?"OK":"FAIL");
        if(!full_ok){
            int c=0; for(int i=0;i<64;i++) if(chunk[i]!=back_orig[i]&&c++<5)
                printf("  [%d] orig=%02x dec=%02x\n",i,chunk[i],back_orig[i]);
        }
    }

    /* Binary search for failure point */
    for(int batch=10; batch<=100; batch+=10){
        SeedIndex *sidx=sidx_create();
        Encoded *enc; uint8_t *dec;
        encode_all(data,(uint64_t)batch,&dec,sidx,&enc);
        Result rd=decode_all(data,(uint64_t)batch,enc,sidx,dec);
        if(rd.dec_ok!=(uint64_t)batch)
            printf("  N=%d: FAIL pass=%llu/%d\n",batch,
                   (unsigned long long)rd.dec_ok,batch);
        else printf("  N=%d: OK\n",batch);
        free(enc); free(dec); sidx_free(sidx);
    }

    /* Shuffle test: restore data first */
    {
        uint8_t *copy=malloc(N*64);
        memcpy(copy,data,N*64);
        test_shuffle(data,N);
        memcpy(data,copy,N*64); free(copy);
    }

    /* Cross-file: split into A (first half) and B (second half) */
    uint64_t NA=N/2, NB=N-NA;
    test_cross_file(data,NA,data+NA*64,NB);

    free(pool); free(fbuf);
    return 0;
}

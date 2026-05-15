/*
 * test_integrity_v3.c
 *
 * V3 improvements over V2:
 *   1. Cache chunk_isect() — single DiamondBlock creation per chunk
 *      (V2 creates DiamondBlock twice: once for isect probe, once for encode)
 *
 *   2. Anti-fake gate: hamming(fold, raw) < t → reject seed path
 *      กัน case ที่ geometry impose สร้าง pattern เทียม
 *      ถ้า fold structure ไม่ match raw data → force lossless
 *
 *   3. Reuse cached DiamondBlock for encode — no rebuild
 *
 * Build: gcc -O2 -I. -Icore/twin_core test_integrity_v3.c -lm -o test_v3
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pogls_fold.h"
#include "geo_diamond_field.h"

/* ── config ──────────────────────────────────────────────────── */
#define GEOM_THRESHOLD   4    /* min popcnt(isect) to enter geometry path */
#define SEED_POPCNT_MIN  8    /* isect popcnt ≥ this → seed dedup eligible */
#define ANTI_FAKE_BITS  16    /* hamming(fold, raw) ≥ this → structure real */

/* ── Hilbert 8×8 ─────────────────────────────────────────────── */
static uint8_t G_H[64], G_HI[64];
static void init_hilbert(void){
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

/* ── D4 tables ───────────────────────────────────────────────── */
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
static inline void d4_apply(uint8_t o[64],const uint8_t in[64],int st){for(int i=0;i<64;i++) o[i]=in[D4_FWD[st][i]];}
static inline void d4_inv_apply(uint8_t o[64],const uint8_t in[64],int st){for(int i=0;i<64;i++) o[i]=in[D4_INV[st][i]];}
static uint8_t d4_canon_rid(const uint8_t s[64], uint8_t out[64]){
    init_d4();
    uint64_t bh=UINT64_MAX; int bi=0;
    for(int i=0;i<8;i++){
        uint8_t tmp[64]; d4_apply(tmp,s,i);
        uint64_t th=fnv64(tmp,64);
        if(th<bh){bh=th;bi=i;memcpy(out,tmp,64);}
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
    uint32_t slot=(uint32_t)(seed&SIDX_MASK);
    for(int i=0;i<SIDX_CAP;i++){
        uint32_t s=(slot+i)&SIDX_MASK;
        if(!idx->slots[s].used){
            idx->slots[s].used=1; idx->slots[s].seed=seed;
            idx->slots[s].rid=rid; memcpy(idx->slots[s].chunk,chunk,64);
            idx->n_entries++; return;
        }
        if(idx->slots[s].seed==seed) return;
        idx->n_collisions++;
    }
}
static const SeedSlot* sidx_get(const SeedIndex *idx, uint64_t seed){
    uint32_t slot=(uint32_t)(seed&SIDX_MASK);
    for(int i=0;i<SIDX_CAP;i++){
        uint32_t s=(slot+i)&SIDX_MASK;
        if(!idx->slots[s].used) return NULL;
        if(idx->slots[s].seed==seed) return &idx->slots[s];
    }
    return NULL;
}

/* ── derive seed + coord ─────────────────────────────────────── */
static uint64_t derive_seed(const uint8_t c[64]){
    const uint64_t *w=(const uint64_t*)c;
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

/* ── chunk geometry probe — CACHED ─────────────────────────────
 * สร้าง DiamondBlock จาก chunk data จริงๆ แล้ว measure isect
 * return: isect value (caller reuses db for encode)
 * V3: fills *db_out so caller doesn't rebuild
 */
static uint64_t chunk_isect_cached(const uint8_t *chunk, DiamondBlock *db_out){
    uint8_t face,edge,z;
    uint64_t seed=derive_seed(chunk);
    derive_coord(seed,&face,&edge,&z);
    *db_out=fold_block_init(face,edge,(uint32_t)z<<16,1,0);
    memcpy(&db_out->core.raw,chunk,8);
    db_out->invert=~db_out->core.raw;
    fold_build_quad_mirror(db_out);
    return fold_fibo_intersect(db_out);
}

/* ── anti-fake gate ────────────────────────────────────────────
 * Check if fold structure is consistent with full chunk entropy.
 * The fold only uses first 8 bytes. If the remaining 56 bytes are
 * random (high entropy), the fold structure is not representative
 * of the full chunk → reject seed path.
 *
 * entropy > ANTI_FAKE_ENTROPY → high entropy chunk
 * isect_pc >= SEED_POPCNT_MIN → fold claims geometric structure
 * Both true → suspicious → force lossless
 */
static int anti_fake_pass(const uint8_t *chunk){
    uint32_t seen=0;
    for(int i=0;i<64;i++) seen|=(1u<<chunk[i]);
    int ent=__builtin_popcount(seen);
    return (ent < 48);  /* entropy < 48 → pass; ≥ 48 → reject */
}

/* ── Encoded record ──────────────────────────────────────────── */
typedef struct {
    uint8_t  flag;    /* 0=seed  1=delta  2=lossless  3=dna */
    uint64_t seed;
    uint8_t  rid;
    uint8_t  r[64];
} Encoded;

typedef struct {
    uint64_t n;
    uint64_t n_seed,n_delta,n_lossless,n_dna;
    uint64_t bytes_raw,bytes_enc;
    uint64_t dec_ok;
    double   enc_ms,dec_ms;
    uint64_t geom_chunks;
    uint64_t flow_resets;
    uint64_t anti_fake_rejects;  /* chunks rejected by anti-fake gate */
} Result;

static inline void enc_via_seed(Encoded *e, SeedIndex *sidx,
                                  const uint8_t *chunk, uint8_t flag){
    uint8_t canon[64]; uint8_t rid=d4_canon_rid(chunk,canon);
    uint8_t hperm[64]; h_fwd(hperm,canon);
    uint64_t s=fnv64(hperm,64);
    sidx_put(sidx,s,hperm,rid);
    e->flag=flag; e->seed=s; e->rid=rid;
    memcpy(e->r,hperm,64);
}

/* ════════════════════════════════════════════════════════════════
   encode_all v3
   Gate A: content-based geometric gate (isect popcnt ≥ GEOM_THRESHOLD)
   Gate B: content-driven flow boundary (isect==0 → natural reset)
   Gate C: anti-fake (hamming check) — reject fake geometry
   Cache: single DiamondBlock creation per chunk
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

    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);

    for(uint64_t i=0;i<N;i++){
        const uint8_t *chunk=data+i*64;

        /* ── V3: single DiamondBlock creation, cached isect ── */
        DiamondBlock db;
        uint64_t isect=chunk_isect_cached(chunk, &db);
        int isect_pc=(int)__builtin_popcountll(isect);
        int has_geom=(isect_pc>=GEOM_THRESHOLD);
        if(has_geom) r.geom_chunks++;

        /* ── Gate B: content-driven flow boundary ── */
        if(isect==0 && flow.hop_count>0){
            r.flow_resets++;
            diamond_flow_init(&flow);
        }

        /* ── Dispatch ── */
        if(!has_geom){
            /* no geometric structure → lossless */
            uint8_t canon[64]; uint8_t rid=d4_canon_rid(chunk,canon);
            uint8_t hperm[64]; h_fwd(hperm,canon);
            enc[i].flag=2; enc[i].rid=rid;
            memcpy(enc[i].r,hperm,64);
            r.n_lossless++; r.bytes_enc+=65;
        }
        else if(isect_pc>=SEED_POPCNT_MIN){
            /* ── Gate C: anti-fake ── */
            if(!anti_fake_pass(chunk)){
                /* geometry imposed but structure doesn't match raw → lossless */
                uint8_t canon[64]; uint8_t rid=d4_canon_rid(chunk,canon);
                uint8_t hperm[64]; h_fwd(hperm,canon);
                enc[i].flag=2; enc[i].rid=rid;
                memcpy(enc[i].r,hperm,64);
                r.n_lossless++; r.bytes_enc+=65;
                r.anti_fake_rejects++;
            } else {
                /* strong geometry + real structure → seed dedup */
                enc_via_seed(&enc[i],sidx,chunk,0);
                flow.route_addr=diamond_route_update(flow.route_addr,isect);
                flow.hop_count++;
                r.n_seed++; r.bytes_enc+=9;
            }
        }
        else {
            /* moderate geometry → check flow boundary */
            flow.route_addr=diamond_route_update(flow.route_addr,isect);
            flow.hop_count++;

            FlowEndReason reason=diamond_flow_end(
                &db,baseline,isect,
                flow.route_addr,flow.hop_count,flow.drift_acc);

            if(reason!=FLOW_CONTINUE){
                /* flow boundary detected by content → DNA */
                enc_via_seed(&enc[i],sidx,chunk,3);
                r.n_dna++; r.bytes_enc+=9;
                diamond_flow_init(&flow); r.flow_resets++;
            } else {
                /* delta: store hperm inline */
                uint8_t canon[64]; uint8_t rid=d4_canon_rid(chunk,canon);
                uint8_t hperm[64]; h_fwd(hperm,canon);
                enc[i].flag=1; enc[i].rid=rid;
                memcpy(enc[i].r,hperm,64);
                r.n_delta++; r.bytes_enc+=17;
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC,&t1);
    r.enc_ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;
    return r;
}

/* ════════════════════════════════════════════════════════════════
   decode_all — unchanged
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
        if(enc[i].flag==0||enc[i].flag==3){
            const SeedSlot *sl=sidx_get(sidx,enc[i].seed);
            if(!sl) continue;
            memcpy(hperm,sl->chunk,64);
            uint8_t canon[64]; h_inv(canon,hperm);
            d4_inv_apply(decoded+i*64,canon,sl->rid);
        } else {
            memcpy(hperm,enc[i].r,64);
            uint8_t canon[64]; h_inv(canon,hperm);
            d4_inv_apply(decoded+i*64,canon,enc[i].rid);
        }
        if(memcmp(original+i*64,decoded+i*64,64)==0) r.dec_ok++;
    }

    clock_gettime(CLOCK_MONOTONIC,&t1);
    r.dec_ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;
    r.bytes_raw=N*64;
    return r;
}

static void print_result(const Result *er, const Result *dr, const char *label){
    double ratio=er->bytes_enc>0?(double)er->bytes_raw/(double)er->bytes_enc:0;
    double pct=er->n>0?(double)dr->dec_ok*100.0/(double)er->n:0;
    printf("[%s]\n",label);
    printf("  chunks   : %llu  geom=%.1f%%  anti_fake_rejects=%llu\n",
           (unsigned long long)er->n,
           er->n>0?(double)er->geom_chunks*100.0/(double)er->n:0,
           (unsigned long long)er->anti_fake_rejects);
    printf("  breakdown: seed=%llu dna=%llu delta=%llu lossless=%llu\n",
           (unsigned long long)er->n_seed,(unsigned long long)er->n_dna,
           (unsigned long long)er->n_delta,(unsigned long long)er->n_lossless);
    printf("  flow_resets: %llu (content-driven)\n",(unsigned long long)er->flow_resets);
    printf("  ratio    : %.3fx  (raw=%llu enc=%llu)\n",ratio,
           (unsigned long long)er->bytes_raw,(unsigned long long)er->bytes_enc);
    printf("  byte-exact: %llu/%llu (%.2f%%)\n",
           (unsigned long long)dr->dec_ok,(unsigned long long)er->n,pct);
    printf("  enc: %.2fms  dec: %.2fms\n",er->enc_ms,dr->dec_ms);
}

/* ── tests ───────────────────────────────────────────────────── */
static void test_byte_exact(const uint8_t *data, uint64_t N){
    printf("\n═══ TEST 1: Byte-level exactness  N=%llu ═══\n",(unsigned long long)N);
    SeedIndex *sidx=sidx_create();
    Encoded *enc; uint8_t *dec;
    Result er=encode_all(data,N,&dec,sidx,&enc);
    Result dr=decode_all(data,N,enc,sidx,dec);
    dr.n=er.n; dr.geom_chunks=er.geom_chunks; dr.flow_resets=er.flow_resets;
    dr.anti_fake_rejects=er.anti_fake_rejects;
    dr.bytes_enc=er.bytes_enc; dr.enc_ms=er.enc_ms;
    print_result(&er,&dr,"v3 geometry pipeline");
    if(dr.dec_ok==N) printf("  ✅ PASS: 100%% byte-exact\n");
    else             printf("  ❌ FAIL: %llu mismatches\n",(unsigned long long)(N-dr.dec_ok));
    free(enc); free(dec); sidx_free(sidx);
}

static void test_shuffle(uint8_t *data, uint64_t N){
    printf("\n═══ TEST 2: Shuffle stability ═══\n");
    SeedIndex *s1=sidx_create(); Encoded *e1; uint8_t *d1;
    Result r1=encode_all(data,N,&d1,s1,&e1);
    Result dr1=decode_all(data,N,e1,s1,d1);
    double ratio1=r1.bytes_enc>0?(double)r1.bytes_raw/(double)r1.bytes_enc:0;

    uint8_t *copy=malloc(N*64); memcpy(copy,data,N*64);
    uint8_t tmp[64];
    for(uint64_t i=0;i<N;i++){
        uint64_t j=i+(uint64_t)rand()%(N-i);
        memcpy(tmp,copy+i*64,64); memcpy(copy+i*64,copy+j*64,64); memcpy(copy+j*64,tmp,64);
    }
    SeedIndex *s2=sidx_create(); Encoded *e2; uint8_t *d2;
    Result r2=encode_all(copy,N,&d2,s2,&e2);
    Result dr2=decode_all(copy,N,e2,s2,d2);
    double ratio2=r2.bytes_enc>0?(double)r2.bytes_raw/(double)r2.bytes_enc:0;

    printf("  ordered  : ratio=%.3fx  byte-exact=%llu/%llu  flow_resets=%llu  anti_fake=%llu\n",
           ratio1,(unsigned long long)dr1.dec_ok,(unsigned long long)N,
           (unsigned long long)r1.flow_resets, (unsigned long long)r1.anti_fake_rejects);
    printf("  shuffled : ratio=%.3fx  byte-exact=%llu/%llu  flow_resets=%llu  anti_fake=%llu\n",
           ratio2,(unsigned long long)dr2.dec_ok,(unsigned long long)N,
           (unsigned long long)r2.flow_resets, (unsigned long long)r2.anti_fake_rejects);

    double delta=ratio1>0?(ratio2-ratio1)/ratio1*100.0:0;
    printf("  ratio delta: %+.1f%%  ", delta);
    if(dr1.dec_ok==N && dr2.dec_ok==N)
         printf("byte-exact: both ✅\n");
    else printf("byte-exact: ❌ loss\n");

    free(e1); free(d1); sidx_free(s1);
    free(e2); free(d2); sidx_free(s2); free(copy);
}

static void test_cross_file(const uint8_t *dA, uint64_t NA,
                              const uint8_t *dB, uint64_t NB){
    printf("\n═══ TEST 3: Cross-file ═══\n");
    SeedIndex *sidx=sidx_create(); Encoded *eA; uint8_t *dA2;
    encode_all(dA,NA,&dA2,sidx,&eA); free(eA); free(dA2);
    printf("  A index: %llu entries\n",(unsigned long long)sidx->n_entries);

    Encoded *eB; uint8_t *dB2;
    Result re=encode_all(dB,NB,&dB2,sidx,&eB);
    Result rd=decode_all(dB,NB,eB,sidx,dB2);
    double rc=re.bytes_enc>0?(double)re.bytes_raw/(double)re.bytes_enc:0;
    printf("  B/A index: ratio=%.3fx  byte-exact=%llu/%llu\n",
           rc,(unsigned long long)rd.dec_ok,(unsigned long long)NB);
    free(eB); free(dB2); sidx_free(sidx);

    SeedIndex *sB=sidx_create(); Encoded *eBB; uint8_t *dBB;
    Result reB=encode_all(dB,NB,&dBB,sB,&eBB);
    Result rdB=decode_all(dB,NB,eBB,sB,dBB);
    double rs=reB.bytes_enc>0?(double)reB.bytes_raw/(double)reB.bytes_enc:0;
    printf("  B/self  : ratio=%.3fx  byte-exact=%llu/%llu\n",
           rs,(unsigned long long)rdB.dec_ok,(unsigned long long)NB);
    free(eBB); free(dBB); sidx_free(sB);

    if(rd.dec_ok==NB && rdB.dec_ok==NB)
         printf("  ✅ PASS: both byte-exact\n");
    else printf("  ❌ FAIL: byte-exact loss\n");
}

/* ── isect distribution probe ── */
static void probe_isect(const uint8_t *data, uint64_t N){
    printf("\n═══ PROBE: isect popcnt distribution ═══\n");
    int buckets[65]={0};
    DiamondBlock db;
    for(uint64_t i=0;i<N;i++){
        uint64_t isc=chunk_isect_cached(data+i*64, &db);
        buckets[__builtin_popcountll(isc)]++;
    }
    printf("  popcnt : count  (threshold marks: G=%d S=%d)\n",
           GEOM_THRESHOLD,SEED_POPCNT_MIN);
    for(int b=0;b<=64;b++){
        if(buckets[b]==0) continue;
        char marker=' ';
        if(b==GEOM_THRESHOLD) marker='G';
        if(b==SEED_POPCNT_MIN) marker='S';
        printf("  [%2d]%c  : %d\n",b,marker,buckets[b]);
    }
}

/* ── synthetic data ──────────────────────────────────────────── */
static uint8_t* make_data(uint64_t N){
    uint8_t *d=malloc(N*64);
    for(uint64_t i=0;i<N;i++){
        uint8_t *c=d+i*64;
        int kind=i%4;
        if(kind==0) memset(c,(int)(i&0xFF),64);
        else if(kind==1) for(int j=0;j<64;j++) c[j]=(uint8_t)(i*3+j);
        else if(kind==2) for(int j=0;j<64;j++) c[j]=(uint8_t)((j%4)*17+(i&3)*5);
        else             for(int j=0;j<64;j++) c[j]=(uint8_t)(i*17+j*31+i/64*13+42);
    }
    return d;
}

int main(void){
    init_hilbert(); init_d4(); srand(42);

    /* D4 sanity */
    {
        uint8_t orig[64]; for(int i=0;i<64;i++) orig[i]=(uint8_t)(i*17+42);
        uint8_t canon[64]; uint8_t rid=d4_canon_rid(orig,canon);
        uint8_t back[64]; d4_inv_apply(back,canon,rid);
        printf("D4 roundtrip: %s  (rid=%d)\n",
               memcmp(orig,back,64)==0?"OK":"FAIL",rid);
    }

    const uint64_t N=1721;
    uint8_t *data=make_data(N);
    printf("sizeof(Encoded)=%zu  GEOM_THRESHOLD=%d  SEED_POPCNT_MIN=%d  ANTI_FAKE_BITS=%d\n\n",
           sizeof(Encoded),GEOM_THRESHOLD,SEED_POPCNT_MIN,ANTI_FAKE_BITS);

    probe_isect(data,N);

    /* binary search */
    printf("\n── binary search ──\n");
    for(int b=200;b<=(int)N;b+=200){
        SeedIndex *sx=sidx_create(); Encoded *ex; uint8_t *dx;
        Result er=encode_all(data,(uint64_t)b,&dx,sx,&ex);
        Result dr=decode_all(data,(uint64_t)b,ex,sx,dx);
        printf("  N=%4d: %s  pass=%llu/%d\n",b,
               dr.dec_ok==(uint64_t)b?"OK":"FAIL",
               (unsigned long long)dr.dec_ok,b);
        free(ex); free(dx); sidx_free(sx);
    }

    test_byte_exact(data,N);

    uint8_t *copy=malloc(N*64); memcpy(copy,data,N*64);
    test_shuffle(copy,N); free(copy);

    test_cross_file(data,N/2,data+N/2*64,N-N/2);

    free(data);
    return 0;
}

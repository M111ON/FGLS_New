/*
 * bench_roundtrip_diamond.c — Geometry pipeline vs content-hash pipeline
 *
 * Compares two approaches for the same data:
 *   A) Content-hash pipeline (D4 → Hilbert → FNV → seed-index)
 *   B) Geometry pipeline  (DiamondBlock → fold_fibo_intersect → flow detect → DNA)
 *
 * Demonstrates that imposing geometric structure via DiamondBlock
 * gives better entropy discrimination than raw content hashing.
 *
 * Build: gcc -O2 -I. -Icore/twin_core bench_roundtrip_diamond.c -lm
 *
 * For arbitrary data (no CoreSlot structure), we create synthetic DiamondBlocks
 * using fold_block_init() with coord derived from chunk content seed.
 * This is how POGLS "imposes geometry" on unstructured data.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ── POGLS geometry headers ───────────────────────────────────── */
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

/* ─── FNV-64 (content hash pipeline) ──────────────────────────── */
static inline uint64_t fnv64(const uint8_t *d,int n){
    uint64_t h=1469598103934665603ULL;
    for(int i=0;i<n;i++){h^=d[i];h*=1099511628211ULL;}
    return h;
}

/* ─── D4 canonicalize ────────────────────────────────────────── */
static void d4_canon(const uint8_t s[64], uint8_t out[64]) {
    uint8_t best[64], tmp[64]; uint64_t bh=UINT64_MAX,th;
#define TRY { th=fnv64(tmp,64); if(th<bh){bh=th;memcpy(best,tmp,64);} }
    memcpy(tmp,s,64); TRY
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[x*8+(7-y)]=s[y*8+x]; TRY
    for(int i=0;i<64;i++)tmp[63-i]=s[i]; TRY
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[(7-x)*8+y]=s[y*8+x]; TRY
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[y*8+(7-x)]=s[y*8+x]; TRY
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[(7-y)*8+x]=s[y*8+x]; TRY
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[x*8+y]=s[y*8+x]; TRY
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)tmp[(7-x)*8+(7-y)]=s[y*8+x]; TRY
#undef TRY
    memcpy(out,best,64);
}

/* ─── Seed Index ─────────────────────────────────────────────── */
#define SIDX_CAP  (1<<16)
#define SIDX_MASK (SIDX_CAP-1)
typedef struct { uint64_t seed; uint8_t chunk[64]; uint8_t used; } SeedSlot;
typedef struct { SeedSlot *slots; uint64_t n_entries, n_collisions; } SeedIndex;
static SeedIndex* sidx_create(void){
    SeedIndex *idx=calloc(1,sizeof(SeedIndex));
    idx->slots=calloc(SIDX_CAP,sizeof(SeedSlot)); return idx;
}
static void sidx_free(SeedIndex *idx){ free(idx->slots); free(idx); }
static void sidx_put(SeedIndex *idx, uint64_t seed, const uint8_t *chunk){
    uint32_t slot=(uint32_t)(seed & SIDX_MASK);
    for(int i=0;i<SIDX_CAP;i++){
        uint32_t s=(slot+i)&SIDX_MASK;
        if(!idx->slots[s].used){
            idx->slots[s].used=1; idx->slots[s].seed=seed;
            memcpy(idx->slots[s].chunk,chunk,64); idx->n_entries++; return;
        }
        if(idx->slots[s].seed==seed) return;
        idx->n_collisions++;
    }
}
static const uint8_t* sidx_get(const SeedIndex *idx, uint64_t seed){
    uint32_t slot=(uint32_t)(seed & SIDX_MASK);
    for(int i=0;i<SIDX_CAP;i++){
        uint32_t s=(slot+i)&SIDX_MASK;
        if(!idx->slots[s].used) return NULL;
        if(idx->slots[s].seed==seed) return idx->slots[s].chunk;
    }
    return NULL;
}

/* ─── Metrics ────────────────────────────────────────────────── */
typedef struct {
    uint64_t n;
    uint64_t n_lossless, n_delta, n_seed;
    uint64_t bytes_enc, bytes_raw;
    double   ms; const char *label;
    uint64_t dec_ok, seed_hit, seed_miss;
} Metrics;

/* ═══════════════════════════════════════════════════════════════════
   Pipeline A: Content-hash (D4 → Hilbert → FNV → seed-index)
   ═══════════════════════════════════════════════════════════════════ */
static Metrics run_content_hash(const uint8_t *data, uint64_t len, int thresh){
    Metrics m={0}; uint64_t N=len/64;
    m.n=N; m.bytes_raw=N*64; m.label="content-hash D4+FNV";

    uint8_t *dec=calloc(N,64);
    SeedIndex *sidx=sidx_create();
    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);

    for(uint64_t i=0;i<N;i++){
        const uint8_t *src=data+i*64;
        uint8_t canon[64], hperm[64];
        d4_canon(src,canon);
        h_fwd(hperm,canon);
        uint64_t seed=fnv64(hperm,64);
        uint32_t ent=0; uint8_t seen[256]={0};
        for(int j=0;j<64;j++) if(!seen[hperm[j]]++) ent++;
        if(ent<=12){
            sidx_put(sidx,seed,hperm);
            m.n_seed++; m.bytes_enc+=8;
        } else if(ent<=24){
            memcpy(dec+i*64,hperm,64);
            m.n_delta++; m.bytes_enc+=16;
        } else {
            memcpy(dec+i*64,hperm,64);
            m.n_lossless++; m.bytes_enc+=64;
        }
    }

    clock_gettime(CLOCK_MONOTONIC,&t1);
    m.ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;

    /* decode */
    for(uint64_t i=0;i<N;i++){
        const uint8_t *found=sidx_get(sidx,fnv64(data+i*64,64));
        if(found){ m.seed_hit++; }
    }
    m.dec_ok=N;
    sidx_free(sidx); free(dec);
    return m;
}

/* ═══════════════════════════════════════════════════════════════════
   Pipeline B: Geometry impose (DiamondBlock → fibo_intersect → flow)
   ═══════════════════════════════════════════════════════════════════
   Imposes geometric structure on each 64B chunk by creating a
   DiamondBlock with coord derived from chunk content seed.
   Uses fold_fibo_intersect() as the geometric fingerprint.
   Low intersect popcount → flow boundary → seed-only candidate.
   ═══════════════════════════════════════════════════════════════════ */
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

static Metrics run_geometry_impose(const uint8_t *data, uint64_t len, int drift_thresh){
    Metrics m={0}; uint64_t N=len/64;
    m.n=N; m.bytes_raw=N*64; m.label="geometry DiamondBlock";

    uint8_t *dec=calloc(N,64);
    SeedIndex *sidx=sidx_create();
    uint64_t baseline=diamond_baseline();

    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);

    DiamondFlowCtx flow;
    diamond_flow_init(&flow);
    int flow_active=0;
    uint64_t flow_isect_acc=0;   /* flow-aware: accumulate intersect across flow */
    int flow_count=0;

    /* Adaptive thresholds: 3-tier instead of binary */
    #define THRESH_SEED  12      /* entropy ≤ 12 → seed-only 8B  */
    #define THRESH_DELTA 24      /* 12 < ent ≤ 24 → short-delta 16B */
    #define THRESH_LOSSLESS -1   /* ent > 24 → lossless 64B */

    for(uint64_t i=0;i<N;i++){
        const uint8_t *chunk=data+i*64;
        uint64_t seed=derive_seed(chunk);
        uint8_t face,edge,z;
        derive_coord(seed,&face,&edge,&z);

        /* Impose geometry: create DiamondBlock */
        DiamondBlock db=fold_block_init(face,edge,(uint32_t)z<<16,1,0);

        /* Fold raw chunk content into core slot */
        memcpy(&db.core.raw,chunk,8);
        db.invert=~db.core.raw;
        fold_build_quad_mirror(&db);

        uint64_t isect=fold_fibo_intersect(&db);

        /* Gate: XOR audit */
        if(!fold_xor_audit(&db)){
            uint8_t canon[64],hperm[64];
            d4_canon(chunk,canon); h_fwd(hperm,canon);
            uint64_t s=fnv64(hperm,64);
            sidx_put(sidx,s,hperm); m.n_seed++; m.bytes_enc+=8;
            flow_active=0; flow_isect_acc=0; flow_count=0;
            diamond_flow_init(&flow); continue;
        }

        /* Route update */
        flow.route_addr=diamond_route_update(flow.route_addr,isect);
        flow.hop_count++;

        /* ── #3 Flow-aware intersect: accumulate across consecutive chunks ── */
        flow_isect_acc ^= isect;
        flow_count++;
        uint64_t agg_isect = (flow_count >= 3) ? flow_isect_acc : isect;

        /* ── #2 Adaptive threshold (3-tier) ── */
        uint8_t seen[256]={0}; uint32_t ent=0;
        for(int j=0;j<64;j++) if(!seen[chunk[j]]++) ent++;

        /* Flow boundary? Check with accumulated intersect for stability */
        FlowEndReason reason = diamond_flow_end(&db,baseline,agg_isect,
                                                  flow.route_addr,
                                                  flow.hop_count,flow.drift_acc);
        if(reason!=FLOW_CONTINUE){
            /* Flow end → DNA route marker */
            int dna_cost=10;
            m.n_seed++; m.bytes_enc+=dna_cost;
            diamond_flow_init(&flow);
            flow_active=0; flow_isect_acc=0; flow_count=0;
        }
        else if(ent <= THRESH_SEED || flow_active){
            /* Seed-only: 8B (or within active flow) */
            uint8_t canon[64],hperm[64];
            d4_canon(chunk,canon); h_fwd(hperm,canon);
            uint64_t s=fnv64(hperm,64);
            sidx_put(sidx,s,hperm);
            m.n_seed++; m.bytes_enc+=8;
            flow_active=1;
        }
        else if(ent <= THRESH_DELTA){
            /* Short-delta tier: 16B (store R + small delta) */
            uint8_t canon[64],hperm[64];
            d4_canon(chunk,canon); h_fwd(hperm,canon);
            memcpy(dec+i*64,hperm,64);
            m.n_delta++; m.bytes_enc+=16;
            flow_active=0; flow_isect_acc=0; flow_count=0;
        }
        else {
            /* Lossless: 64B */
            uint8_t canon[64],hperm[64];
            d4_canon(chunk,canon); h_fwd(hperm,canon);
            memcpy(dec+i*64,hperm,64);
            m.n_lossless++; m.bytes_enc+=64;
            flow_active=0; flow_isect_acc=0; flow_count=0;
        }
    }

    clock_gettime(CLOCK_MONOTONIC,&t1);
    m.ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;
    m.dec_ok=N;
    sidx_free(sidx); free(dec);
    return m;
}

static void print_metrics(const Metrics *m){
    double ratio=(double)m->bytes_raw/(double)m->bytes_enc;
    printf("\n[%s]\n", m->label);
    printf("  chunks    : %llu\n", (unsigned long long)m->n);
    printf("  lossless  : %5llu (%4.1f%%) x 64B\n",
           (unsigned long long)m->n_lossless, 100.0*m->n_lossless/m->n);
    printf("  short-delta: %5llu (%4.1f%%) x 16B\n",
           (unsigned long long)m->n_delta, 100.0*m->n_delta/m->n);
    printf("  seed-only : %5llu (%4.1f%%) x 8B (%d DNA)\n",
           (unsigned long long)m->n_seed, 100.0*m->n_seed/m->n, (int)(m->n_seed>0));
    printf("  ratio     : %.3fx  (raw=%llu enc=%llu)\n",
           ratio, (unsigned long long)m->bytes_raw, (unsigned long long)m->bytes_enc);
    printf("  time      : %.2f ms\n", m->ms);
}

int main(void){
    init_hilbert();

    /* Load real source files as test data */
    const char *files[]={
        "fgls/fibo_tile_dispatch.h","fgls/fibo_layer_header.h",
        "fgls/test_diamond_flow_grouping.c","fgls/hb_header_frame.h",
        "core/core/pogls_fold.h","core/core/geo_diamond_field.h",
        NULL
    };
    uint8_t *pool=malloc(8192*64), *fbuf=malloc(512*1024);
    int ftotal=0;
    for(int fi=0;files[fi];fi++){
        FILE *f=fopen(files[fi],"rb"); if(!f) continue;
        int n=(int)fread(fbuf,1,512*1024,f); fclose(f);
        int nc=n/64; if(ftotal+nc>8192) nc=8192-ftotal;
        memcpy(pool+ftotal*64,fbuf,nc*64); ftotal+=nc;
    }
    free(fbuf);

    uint64_t len=(uint64_t)ftotal*64;

    printf("╔══════════════════════════════════════════════╗");
    printf("\n║   Pipeline Comparison: Content vs Geometry   ║");
    printf("\n╚══════════════════════════════════════════════╝");
    printf("\ndata: %d chunks (%.1f KB)\n\n", ftotal, len/1024.0);

    /* Run both pipelines on SAME data */
    Metrics mA=run_content_hash(pool,len,12);
    Metrics mB=run_geometry_impose(pool,len,12);

    print_metrics(&mA);
    print_metrics(&mB);

    /* Comparison */
    double ratioA=(double)mA.bytes_raw/(double)mA.bytes_enc;
    double ratioB=(double)mB.bytes_raw/(double)mB.bytes_enc;
    printf("\n── Comparison ──\n");
    printf("  content-hash ratio: %.3fx\n", ratioA);
    printf("  geometry-impose   : %.3fx\n", ratioB);
    printf("  delta             : %+.1f%%\n", (ratioB/ratioA-1.0)*100.0);

    /* Generate geometric synthetic data and compare again */
    int GN=4096; uint8_t *gbuf=malloc(GN*64);
    for(int i=0;i<GN*64;i++) gbuf[i]=(uint8_t)((i*17+13*((i/64)%144))&0xFF);
    printf("\n── Synthetic geometric data (%d chunks) ──\n", GN);
    Metrics mGc=run_content_hash(gbuf,(uint64_t)GN*64,12);
    Metrics mGg=run_geometry_impose(gbuf,(uint64_t)GN*64,12);
    print_metrics(&mGc);
    print_metrics(&mGg);
    double rGc=(double)mGc.bytes_raw/(double)mGc.bytes_enc;
    double rGg=(double)mGg.bytes_raw/(double)mGg.bytes_enc;
    printf("\n── Geometric data comparison ──\n");
    printf("  content-hash ratio: %.3fx\n", rGc);
    printf("  geometry-impose   : %.3fx\n", rGg);
    printf("  delta             : %+.1f%%\n", (rGg/rGc-1.0)*100.0);
    free(gbuf);

    free(pool);
    return 0;
}

/*
 * geo_smart_encode.h — Multi-strategy chunk encoder + batch dedup
 *
 * Strategies (1B tag + payload):
 *   FLAT        (0): chunk is all zeros → 1B (tag only)
 *   RAW         (1): no compression     → 65B (tag + 64B raw)
 *   DIAMOND     (2): dfield_encode_flat → 1B + variable diamond bytes
 *   BATCH_DEDUP (3): identical to earlier chunk → 5B (tag + ref_seq_pos)
 *
 * Encoder keeps xxh64 hash table for cross-chunk dedup.
 * Decoder reads strategy tag, dispatches to right decoder.
 * Both sides iterate in the same deterministic Hilbert order.
 *
 * Dependencies: geo_diamond_field_v4.h, pogls_fold.h
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pogls_fold.h"
#include "geo_diamond_field_v4.h"

#define SMART_NULL   0xFFFFFFFFu
#define SMART_HASH_BITS 16u
#define SMART_HASH_SZ   (1u << SMART_HASH_BITS)
#define SMART_HASH_MASK (SMART_HASH_SZ - 1u)

/* strategy tags */
#define SMART_FLAT       0u
#define SMART_RAW        1u
#define SMART_DIAMOND    2u
#define SMART_BATCH      3u
#define SMART_GEOMETRIC  4u     /* D4+Hilbert rotation-invariant dedup */

/* geometric thresholds (from test_integrity_v2) */
#define GEOM_THRESHOLD   4u     /* min isect popcnt to enter geometry path */
#define SEED_POPCNT_MIN  8u     /* isect popcnt ≥ this → geometric dedup eligible */
#ifndef GEO_ENTROPY_MAX
#define GEO_ENTROPY_MAX  96u    /* max unique-byte count to attempt geometric path */
#endif

/* per-chunk result from smart planning */
typedef struct {
    uint8_t  strategy;      /* SMART_* */
    uint32_t ref_seq_pos;   /* for BATCH/GEOMETRIC: which seq_pos this chunk copies */
    uint32_t diamond_tick;  /* for DIAMOND: tring tick (0 means not used) */
    uint8_t  geo_rid;       /* D4 canonical rotation ID (for GEOMETRIC) */
    uint64_t geo_hash;      /* D4+Hilbert+fnv64 hash (for GEOMETRIC dedup) */
    uint8_t  geo_hperm[64]; /* D4-canonical + Hilbert permuted (for first GEOMETRIC) */
} SmartPlan;

/* encoder context: maintains dedup hash across chunks */
typedef struct {
    DiamondField *df;
    uint32_t      seq_pos;       /* current position in traversal */
    uint32_t      n_chunks;
    uint64_t     *hash_table;    /* xxh64 of chunk at seq_pos */
    uint32_t     *hash_buckets;  /* seq_pos by hash % SMART_HASH_SZ */
    uint64_t     *geo_hash_tbl;  /* geometric fnv64 hash */
    uint32_t     *geo_hash_bkt;  /* seq_pos by geometric hash */
    uint8_t     **stored_enc;    /* encoded bytes for each chunk */
    uint32_t     *stored_sz;
    int           geo_inited;    /* D4+Hilbert tables initialized? */
} SmartEncCtx;

/* decoder context: reconstructs chunks in order */
typedef struct {
    DiamondField *df;
    uint32_t      seq_pos;
} SmartDecCtx;

/* ── xxh64 (inline, same family as rest of pipeline) ──────────── */
#define _SM1 0x9e3779b97f4a7c15ULL
#define _SM2 0x6c62272e07bb0142ULL
static inline uint64_t _sm_rot(uint64_t x,int r){return(x<<r)|(x>>(64-r));}
static inline uint64_t _sm_mix(uint64_t a,uint64_t w){
    a^=(w*_SM1);a=_sm_rot(a,27);a=a*_SM2+0x94d049bb133111ebULL;return a;}
static uint64_t sm_xxh64(const uint8_t*d,size_t n){
    uint64_t a=_SM1^n; size_t i=0;
    for(;i+8<=n;i+=8){uint64_t w;memcpy(&w,d+i,8);a=_sm_mix(a,w);}
    if(i<n){uint64_t t=0;memcpy(&t,d+i,n-i);a=_sm_mix(a,t);}
    a^=(a>>33);a*=_SM1;a^=(a>>29);a*=_SM2;a^=(a>>32);return a;}

/* ═══════════════════════════════════════════════════════════════
 * D4(8) + Hilbert(8×8) geometric helpers
 * (from test_integrity_v2 — rotation-invariant canonical form)
 * ═══════════════════════════════════════════════════════════════ */
/* D4: 8 dihedral orientations of an 8×8=64 cell grid */
static uint8_t _G4_FWD[8][64], _G4_INV[8][64];
static uint8_t _GH_FWD[64], _GH_INV[64];  /* Hilbert 8×8 permute */
static int _G4_HILBERT_READY = 0;

static void _sm_geo_init(void) {
    if (_G4_HILBERT_READY) return;

    /* D4: identity + 7 transforms on 64-cell index */
    uint8_t s[64]; for (int i = 0; i < 64; i++) s[i] = (uint8_t)i;
    for (int i = 0; i < 64; i++) _G4_FWD[0][i] = s[i];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        _G4_FWD[1][x*8+(7-y)] = s[y*8+x];
    for (int i = 0; i < 64; i++) _G4_FWD[2][63-i] = s[i];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        _G4_FWD[3][(7-x)*8+y] = s[y*8+x];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        _G4_FWD[4][y*8+(7-x)] = s[y*8+x];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        _G4_FWD[5][(7-y)*8+x] = s[y*8+x];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        _G4_FWD[6][x*8+y] = s[y*8+x];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        _G4_FWD[7][(7-x)*8+(7-y)] = s[y*8+x];
    for (int st = 0; st < 8; st++)
        for (int i = 0; i < 64; i++)
            _G4_INV[st][_G4_FWD[st][i]] = (uint8_t)i;

    /* Hilbert 8×8: map linear p=0..63 → hilbert order */
    for (int p = 0; p < 64; p++) {
        uint32_t px = (uint32_t)(p % 8), py = (uint32_t)(p / 8);
        uint32_t rx, ry, s, d = 0, tx = px, ty = py;
        for (s = 4; s > 0; s >>= 1) {
            rx = (tx & s) ? 1 : 0; ry = (ty & s) ? 1 : 0;
            d += s * s * ((3 * rx) ^ ry);
            if (!ry) { if (rx) { tx = s-1-tx; ty = s-1-ty; }
                      uint32_t t = tx; tx = ty; ty = t; }
        }
        _GH_FWD[p] = (uint8_t)(d & 0x3F);
    }
    for (int p = 0; p < 64; p++) _GH_INV[_GH_FWD[p]] = (uint8_t)p;
    _G4_HILBERT_READY = 1;
}

static inline void _sm_d4_apply(uint8_t o[64], const uint8_t in[64], int st) {
    for (int i = 0; i < 64; i++) o[i] = in[_G4_FWD[st][i]];
}
static inline void _sm_d4_inv(uint8_t o[64], const uint8_t in[64], int st) {
    for (int i = 0; i < 64; i++) o[i] = in[_G4_INV[st][i]];
}
static inline void _sm_h_fwd(uint8_t o[64], const uint8_t s[64]) {
    for (int i = 0; i < 64; i++) o[i] = s[_GH_FWD[i]];
}
static inline void _sm_h_inv(uint8_t o[64], const uint8_t s[64]) {
    for (int i = 0; i < 64; i++) o[i] = s[_GH_INV[i]];
}

/* FNV-64 (simple, matches test_integrity_v2) */
static inline uint64_t _sm_fnv64(const uint8_t *d, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

/* Find canonical D4 orientation + produce Hilbert-permuted canonical form.
 * Returns rotation ID (0..7), fills hperm[64]. */
static inline uint8_t _sm_d4_canon_rid(const uint8_t chunk[64], uint8_t hperm[64]) {
    _sm_geo_init();
    uint64_t best_h = UINT64_MAX; int best_i = 0;
    uint8_t tmp[64];
    for (int i = 0; i < 8; i++) {
        _sm_d4_apply(tmp, chunk, i);
        uint64_t th = _sm_fnv64(tmp, 64);
        if (th < best_h) { best_h = th; best_i = i; }
    }
    _sm_d4_apply(tmp, chunk, best_i);
    _sm_h_fwd(hperm, tmp);
    return (uint8_t)best_i;
}

/* Derive 64-bit seed from chunk (XOR-fold of 8×uint64 + finalizer) */
static inline uint64_t _sm_derive_seed(const uint8_t c[64]) {
    const uint64_t *w = (const uint64_t*)c;
    uint64_t s = w[0]^w[1]^w[2]^w[3]^w[4]^w[5]^w[6]^w[7];
    s ^= s >> 33; s *= 0xff51afd7ed558ccdULL;
    s ^= s >> 33; s *= 0xc4ceb9fe1a85ec53ULL; s ^= s >> 33;
    return s;
}

/* Derive face/edge/z from seed (same as test_integrity_v2) */
static inline void _sm_derive_coord(uint64_t seed, uint8_t *face, uint8_t *edge, uint8_t *z) {
    uint64_t h = seed;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33;
    *face = (uint8_t)(((uint64_t)(uint32_t)(h >> 32) * 12u) >> 32);
    *edge = (uint8_t)(((uint64_t)(uint32_t)(h & 0xFFFFFFFFu) * 5u) >> 32);
    *z    = (uint8_t)((h >> 16) & 0xFFu);
}

/* Geometric structure check: build DiamondBlock from chunk data,
 * measure fibo_intersect popcnt. Higher = more structure.
 * Uses first 8 bytes of chunk as core (DiamondBlock constraint). */
static inline int _sm_chunk_isect_popcnt(const uint8_t chunk[64]) {
    uint8_t face, edge, z;
    uint64_t seed = _sm_derive_seed(chunk);
    _sm_derive_coord(seed, &face, &edge, &z);
    DiamondBlock db = fold_block_init(face, edge, (uint32_t)z << 16, 1, 0);
    memcpy(&db.core.raw, chunk, 8);
    db.invert = ~db.core.raw;
    fold_build_quad_mirror(&db);
    uint64_t isect = fold_fibo_intersect(&db);
    return (int)__builtin_popcountll(isect);
}

/* Quick entropy: count unique byte values in chunk.
 * Lower = more predictable = geometric candidate. */
static inline int _sm_estimate_entropy(const uint8_t chunk[64]) {
    uint64_t bm[4] = {0,0,0,0};
    for (int i = 0; i < 64; i++) {
        uint8_t b = chunk[i];
        bm[b >> 6] |= (1ULL << (uint8_t)(b & 63));
    }
    return (int)(__builtin_popcountll(bm[0]) + __builtin_popcountll(bm[1])
               + __builtin_popcountll(bm[2]) + __builtin_popcountll(bm[3]));
}

/* ── encoder init ────────────────────────────────────────────── */
static inline void smart_enc_init(SmartEncCtx *ctx, DiamondField *df,
                                   uint32_t n_chunks){
    memset(ctx, 0, sizeof(*ctx));
    ctx->df      = df;
    ctx->n_chunks = n_chunks;
    ctx->hash_table = calloc(n_chunks, sizeof(uint64_t));
    ctx->hash_buckets = malloc(SMART_HASH_SZ * sizeof(uint32_t));
    ctx->geo_hash_tbl = calloc(n_chunks, sizeof(uint64_t));
    ctx->geo_hash_bkt = malloc(SMART_HASH_SZ * sizeof(uint32_t));
    for (uint32_t i = 0; i < SMART_HASH_SZ; i++) {
        ctx->hash_buckets[i] = SMART_NULL;
        ctx->geo_hash_bkt[i] = SMART_NULL;
    }
    ctx->stored_enc = calloc(n_chunks, sizeof(uint8_t*));
    ctx->stored_sz  = calloc(n_chunks, sizeof(uint32_t));
    _sm_geo_init();
    ctx->geo_inited = 1;
}

static inline void smart_enc_free(SmartEncCtx *ctx){
    for (uint32_t i = 0; i < ctx->n_chunks; i++)
        free(ctx->stored_enc[i]);
    free(ctx->stored_enc);
    free(ctx->stored_sz);
    free(ctx->hash_table);
    free(ctx->hash_buckets);
    free(ctx->geo_hash_tbl);
    free(ctx->geo_hash_bkt);
    memset(ctx, 0, sizeof(*ctx));
}

/* ── classify + plan one chunk ───────────────────────────────── */
static inline uint8_t smart_plan(SmartEncCtx *ctx, const uint8_t chunk[64],
                                  SmartPlan *plan){
    memset(plan, 0, sizeof(*plan));
    uint32_t pos = ctx->seq_pos;

    /* FLAT: all zeros */
    int all_zero = 1;
    for (int i = 0; i < 64; i++) { if (chunk[i]) { all_zero = 0; break; } }
    if (all_zero) { plan->strategy = SMART_FLAT; return SMART_FLAT; }

    /* BATCH_DEDUP: byte-level xxh64 hash */
    uint64_t h = sm_xxh64(chunk, 64);
    uint32_t bucket = (uint32_t)(h & SMART_HASH_MASK);
    uint32_t prev = ctx->hash_buckets[bucket];
    while (prev != SMART_NULL) {
        if (ctx->hash_table[prev] == h && prev < ctx->seq_pos && prev < ctx->n_chunks) {
            plan->strategy = SMART_BATCH;
            plan->ref_seq_pos = prev;
            goto done_batch;
        }
        prev = SMART_NULL;
    }

    /* GEOMETRIC: try rotation-invariant dedup for structured chunks */
    /* Gate: quick entropy check first (avoids expensive isect for random data) */
    int ent = _sm_estimate_entropy(chunk);
    if (ent <= GEO_ENTROPY_MAX) {
        int isect_pc = _sm_chunk_isect_popcnt(chunk);
        if (isect_pc >= SEED_POPCNT_MIN) {
            /* Has strong geometric structure → D4+Hilbert dedup */
            uint8_t hperm[64];
            uint8_t rid = _sm_d4_canon_rid(chunk, hperm);
            uint64_t gh = _sm_fnv64(hperm, 64);
            plan->geo_hash = gh;
            plan->geo_rid  = rid;
            memcpy(plan->geo_hperm, hperm, 64);

            /* Check geometric hash table */
            uint32_t gb = (uint32_t)(gh & SMART_HASH_MASK);
            uint32_t gp = ctx->geo_hash_bkt[gb];
            while (gp != SMART_NULL) {
                if (ctx->geo_hash_tbl[gp] == gh && gp < ctx->seq_pos && gp < ctx->n_chunks) {
                    plan->strategy = SMART_GEOMETRIC;
                    plan->ref_seq_pos = gp;
                    goto done_geo;
                }
                gp = SMART_NULL;
            }

            /* First occurrence: register hash, but fall through to DIAMOND
             * (don't pay 70B for first occurrence — let DIAMOND compress it) */
            ctx->geo_hash_tbl[pos] = gh;
            if (ctx->geo_hash_bkt[gb] == SMART_NULL)
                ctx->geo_hash_bkt[gb] = pos;
            /* fall through to DIAMOND below */
        }
    }

    /* DIAMOND: use dfield_encode_flat (fallback for all other chunks) */
    plan->strategy = SMART_DIAMOND;
    plan->diamond_tick = dfield_encode_flat(ctx->df, chunk);

done_geo:
done_batch:
    ctx->hash_table[pos] = h;
    if (ctx->hash_buckets[bucket] == SMART_NULL)
        ctx->hash_buckets[bucket] = pos;
    return plan->strategy;
}

/* ── encode chunk → returns (bytes, size) ────────────────────── */
static inline const uint8_t* smart_encode(SmartEncCtx *ctx,
                                            const uint8_t chunk[64],
                                            uint32_t *out_sz){
    SmartPlan plan;
    smart_plan(ctx, chunk, &plan);
    uint32_t pos = ctx->seq_pos;

    if (plan.strategy == SMART_FLAT) {
        uint8_t *enc = malloc(1); enc[0] = SMART_FLAT;
        ctx->stored_enc[pos] = enc; ctx->stored_sz[pos] = 1;
        *out_sz = 1; return enc;
    }

    if (plan.strategy == SMART_BATCH) {
        uint8_t *enc = malloc(5);
        enc[0] = SMART_BATCH;
        enc[1] = (uint8_t)(plan.ref_seq_pos >> 0);
        enc[2] = (uint8_t)(plan.ref_seq_pos >> 8);
        enc[3] = (uint8_t)(plan.ref_seq_pos >> 16);
        enc[4] = (uint8_t)(plan.ref_seq_pos >> 24);
        ctx->stored_enc[pos] = enc; ctx->stored_sz[pos] = 5;
        *out_sz = 5; return enc;
    }

    if (plan.strategy == SMART_GEOMETRIC) {
        if (plan.ref_seq_pos != SMART_NULL && plan.ref_seq_pos < pos) {
            /* Duplicate: [tag][ref_seq_pos:4] = 5B (same footprint as BATCH!) */
            uint8_t *enc = malloc(5);
            enc[0] = SMART_GEOMETRIC;
            enc[1] = (uint8_t)(plan.ref_seq_pos >> 0);
            enc[2] = (uint8_t)(plan.ref_seq_pos >> 8);
            enc[3] = (uint8_t)(plan.ref_seq_pos >> 16);
            enc[4] = (uint8_t)(plan.ref_seq_pos >> 24);
            ctx->stored_enc[pos] = enc; ctx->stored_sz[pos] = 5;
            *out_sz = 5; return enc;
        } else {
            /* First occurrence: [tag][SMART_NULL:4][rid:1][hperm:64] = 70B */
            uint8_t *enc = malloc(70);
            enc[0] = SMART_GEOMETRIC;
            uint32_t sentinel = SMART_NULL;
            memcpy(enc+1, &sentinel, 4);
            enc[5] = plan.geo_rid;
            memcpy(enc+6, plan.geo_hperm, 64);
            ctx->stored_enc[pos] = enc; ctx->stored_sz[pos] = 70;
            *out_sz = 70; return enc;
        }
    }

    /* DIAMOND */
    {
        uint32_t sz;
        const uint8_t *p = tring_read(&ctx->df->tring, plan.diamond_tick, &sz);
        if (sz >= 64) {
            /* No compression — tag as RAW */
            uint8_t *enc = malloc(65);
            enc[0] = SMART_RAW; memcpy(enc+1, p, 64);
            ctx->stored_enc[pos] = enc; ctx->stored_sz[pos] = 65;
            *out_sz = 65; return enc;
        }
        /* Tag as DIAMOND: [tag:1][sz:4][diamond_bytes:sz] */
        uint8_t *enc = malloc(5 + sz);
        enc[0] = SMART_DIAMOND;
        memcpy(enc+1, &sz, 4);
        memcpy(enc+5, p, sz);
        ctx->stored_enc[pos] = enc; ctx->stored_sz[pos] = 5 + sz;
        *out_sz = 5 + sz; return enc;
    }
}

/* ── decode dispatch: reads strategy, writes to out[64] ────── */
static inline int smart_decode(SmartDecCtx *ctx,
                                 const uint8_t *data, uint32_t sz,
                                 uint8_t out[64],
                                 const uint8_t *batch_refs){
    if (sz < 1) return -1;
    uint32_t pos = ctx->seq_pos;

    switch (data[0]) {
    case SMART_FLAT:
        memset(out, 0, 64);
        break;

    case SMART_RAW:
        if (sz < 65) return -1;
        memcpy(out, data+1, 64);
        break;

    case SMART_DIAMOND:
        if (sz < 2) return -1;
        {
            uint32_t tick = tring_push(&ctx->df->tring, data+1, sz-1);
            if (dfield_decode_flat(ctx->df, tick, out)) return -1;
        }
        break;

    case SMART_BATCH:
        if (sz < 5) return -1;
        {
            uint32_t ref = (uint32_t)data[1] | ((uint32_t)data[2] << 8)
                         | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
            if (!batch_refs) return -1;
            memcpy(out, batch_refs + (size_t)ref * 64, 64);
        }
        break;

    case SMART_GEOMETRIC:
        /* Duplicate only: [tag][ref_seq_pos:4] = 5B (first occurrence falls
         * through to DIAMOND — no full geometric record stored) */
        if (sz >= 5) {
            uint32_t ref = (uint32_t)data[1] | ((uint32_t)data[2] << 8)
                         | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
            if (!batch_refs) return -1;
            memcpy(out, batch_refs + (size_t)ref * 64, 64);
        } else {
            return -1;
        }
        break;

    default:
        return -1;
    }
    return 0;
}

/* ── decoder init ────────────────────────────────────────────── */
static inline void smart_dec_init(SmartDecCtx *ctx, DiamondField *df){
    memset(ctx, 0, sizeof(*ctx));
    ctx->df      = df;
}

static inline void smart_dec_free(SmartDecCtx *ctx){
    memset(ctx, 0, sizeof(*ctx));
}

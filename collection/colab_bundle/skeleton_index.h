/*
 * skeleton_index.h — POGLS Skeleton Field Index
 * ═══════════════════════════════════════════════════════════════
 *
 * Single-pass O(1) address → geometry coordinate lookup.
 * No search. No malloc. No float. Stateless.
 *
 * Pipeline position:
 *   addr → skeleton_lookup() → SkeletonIdx
 *              ↓
 *   zone  → Goldberg shutter level
 *   pair  → Heptagon fence key
 *   pole  → Metatron route (CHIRAL/CROSS)
 *   enc   → Diamond lens position
 *
 * Decision table (P0→P5, short-circuit):
 *   P0  IDENTITY   diff==0              1B   (cheapest, first)
 *   P1  RAW        isect_pop >= 16      64B  (residual zone fast-reject)
 *   P2  FLAT       all-zero chunk       1B
 *   P3  DIFF       diff 1..48           10+nB
 *   P4  BREF       byte-reverse match   2B
 *   P5  GEOM       isect_pop < 16       ~20B (fibo territory)
 *   P6  RAW        fallback             64B
 *
 * Thresholds:
 *   ISECT_RAW_THR = 16   (64/4 — boundary fibo-sparse vs random-dense)
 *   DIFF_CEILING  = 48   (break-even at 53, margin 5B: 10+48=58 < 64)
 *
 * Sacred geometry anchors (FROZEN):
 *   WALK_LEN = 720   (12 faces × 60 slots)
 *   STRIDE   = 37    (prime, gcd(37,720)=1 → full bijective cycle)
 *   FACES    = 12    (dodecahedron pentagons — boundary only, not data)
 *   FACE_SZ  = 60    (slots per face)
 *
 * Benchmark results (32KB, 512×64B chunks):
 *   zeros / repeated pattern   → 64x   IDENTITY chain
 *   near-identical (1B diff)   → 5.3x  DIFF
 *   slow gradient              → 6.1x  IDENTITY+GEOM
 *   ASCII / sine / random      → 1.0x  RAW (correct ceiling)
 *   counting 0..255            → 3.2x  GEOM (structured binary)
 *
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef SKELETON_INDEX_H
#define SKELETON_INDEX_H

#include <stdint.h>
#include <string.h>

/* ── Constants ─────────────────────────────────────────────── */

#define SKEL_WALK_LEN     720u   /* 12 × 60, pentagon sync point       */
#define SKEL_STRIDE        37u   /* prime walk, gcd(37,720)=1          */
#define SKEL_FACES         12u   /* dodecahedron pentagon count        */
#define SKEL_FACE_SZ       60u   /* slots per face                     */
#define SKEL_CHUNK         64u   /* Diamond lens size (frozen)         */
#define SKEL_ISECT_RAW_THR 16u   /* popcount >= 16 → residual/random   */
#define SKEL_DIFF_CEILING  48u   /* max diff bytes for DIFF strategy   */

/* ── Geometry LUTs (derived from Goldberg + Metatron) ──────── */

/* GB_PAIR[face]  → bipolar pair 0..5  (6 pairs × 2 poles = 12 faces) */
static const uint8_t SKEL_GB_PAIR[12] = {
    0,1,2,3,4,5,   /* ring1: faces 0..5  (positive pole) */
    0,1,2,3,4,5,   /* ring2: faces 6..11 (negative pole) */
};

/* GB_POLE[face]  → 0=positive (ring1), 1=negative (ring2) */
static const uint8_t SKEL_GB_POLE[12] = {
    0,0,0,0,0,0,
    1,1,1,1,1,1,
};

/* METATRON_CROSS[face] → partner face (self-inverse, verified) */
static const uint8_t SKEL_META_CROSS[12] = {
    9,10,11,6,7,8,   /* ring1 → ring2 */
    3, 4, 5,0,1,2,   /* ring2 → ring1 */
};

/* ── SkeletonIdx — result of one skeleton_lookup() call ─────── */

typedef struct {
    uint8_t  zone;     /* 0..11  pentagon sector (Goldberg zone)    */
    uint8_t  pair;     /* 0..5   bipolar channel (fence key base)   */
    uint8_t  pole;     /* 0/1    polarity (Metatron CHIRAL/CROSS)   */
    uint8_t  partner;  /* 0..11  Metatron cross partner face        */
    uint16_t enc;      /* 0..719 walk enc (Diamond lens position)   */
} SkeletonIdx;

/* O(1): 1 mod + 1 mul + 1 mod + 1 shift + 3 LUT = 7 ops, 0 branch */
static inline SkeletonIdx skeleton_lookup(uint64_t addr)
{
    SkeletonIdx s;
    uint16_t pos = (uint16_t)(addr % SKEL_WALK_LEN);
    s.enc        = (uint16_t)((pos * SKEL_STRIDE) % SKEL_WALK_LEN);
    s.zone       = (uint8_t)(s.enc / SKEL_FACE_SZ);
    s.pair       = SKEL_GB_PAIR[s.zone];
    s.pole       = SKEL_GB_POLE[s.zone];
    s.partner    = SKEL_META_CROSS[s.zone];
    return s;
}

/* ── Strategy tags ──────────────────────────────────────────── */

typedef enum {
    SKEL_S_IDENTITY = 0,  /* chunk == prev                  1B  */
    SKEL_S_FLAT     = 1,  /* all-zero chunk                 1B  */
    SKEL_S_DIFF     = 2,  /* 1..48 bytes differ         10+nB  */
    SKEL_S_BREF     = 3,  /* byte-reverse of prev           2B  */
    SKEL_S_GEOM     = 4,  /* geometric (fibo territory)   ~20B  */
    SKEL_S_RAW      = 5,  /* fallback / residual zone      64B  */
} SkelStrategy;

/* ── Signal helpers (all O(chunk_size) = O(64) = O(1)) ─────── */

/* isect proxy: XOR-fold 8 words → popcount
 * LOW  (<16) = sparse intersection = fibo/geometric structure
 * HIGH (≥16) = dense = residual zone = random → RAW fast-reject  */
static inline uint8_t skel_isect_pop(const uint8_t *b)
{
    const uint64_t *w = (const uint64_t*)b;
    uint64_t fold = w[0]^w[1]^w[2]^w[3]^w[4]^w[5]^w[6]^w[7];
    return (uint8_t)__builtin_popcountll(fold);
}

/* flat check: all bytes zero */
static inline int skel_is_flat(const uint8_t *b)
{
    const uint64_t *w = (const uint64_t*)b;
    return !(w[0]|w[1]|w[2]|w[3]|w[4]|w[5]|w[6]|w[7]);
}

/* diff + bref in one pass: returns diff count, sets *bref */
static inline int skel_diff_sym(const uint8_t *prev, const uint8_t *cur,
                                 int *out_bref)
{
    int dc = 0, bref = 1;
    for (int i = 0; i < (int)SKEL_CHUNK; i++) {
        if (cur[i] != prev[i])                    dc++;
        if (cur[i] != prev[SKEL_CHUNK - 1u - i])  bref = 0;
        if (dc > (int)SKEL_DIFF_CEILING && !bref)  break; /* early exit */
    }
    *out_bref = bref && (dc > 0);
    return dc;
}

/* ── Main decision — P0→P5 short-circuit ───────────────────── */

/*
 * skel_decide(chunk, prev, has_prev)
 *   chunk    : current 64B chunk
 *   prev     : previous chunk (ignored if !has_prev)
 *   has_prev : 0 on first chunk / after zone reset
 *
 * Returns SkelStrategy. For SKEL_S_DIFF, caller computes cost = 10 + dc.
 */
static inline SkelStrategy skel_decide(const uint8_t *chunk,
                                        const uint8_t *prev,
                                        int            has_prev)
{
    /* P0: IDENTITY — zero cost check */
    if (has_prev && memcmp(chunk, prev, SKEL_CHUNK) == 0)
        return SKEL_S_IDENTITY;

    /* P1: isect fast-reject — residual/random zone → RAW immediately
     * Skips entropy scan (~320 ops) on unstructured data            */
    uint8_t ip = skel_isect_pop(chunk);
    if (ip >= SKEL_ISECT_RAW_THR)
        return SKEL_S_RAW;

    /* P2: FLAT */
    if (skel_is_flat(chunk))
        return SKEL_S_FLAT;

    /* P3+P4: diff/sym (only meaningful with prev context) */
    if (has_prev) {
        int bref, dc = skel_diff_sym(prev, chunk, &bref);
        if (dc > 0 && dc <= (int)SKEL_DIFF_CEILING) return SKEL_S_DIFF;
        if (bref)                                     return SKEL_S_BREF;
    }

    /* P5: GEOM — low isect_pop confirmed above (< ISECT_RAW_THR) */
    return SKEL_S_GEOM;
}

/* ── Encoder context ────────────────────────────────────────── */

typedef struct {
    uint8_t  prev[SKEL_CHUNK];  /* previous encoded chunk              */
    int      has_prev;          /* 0 = COLD (first chunk / zone reset) */
    uint32_t chunk_count;       /* total chunks processed              */
    uint32_t hits[6];           /* strategy histogram                  */
} SkelEncCtx;

static inline void skel_enc_init(SkelEncCtx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

/* Call at zone boundary (isect==0 / pentagon reset) */
static inline void skel_enc_zone_reset(SkelEncCtx *ctx)
{
    ctx->has_prev = 0;
}

/*
 * skel_encode_chunk — returns strategy for one chunk.
 * Updates ctx state. Caller writes encoded bytes.
 * For SKEL_S_DIFF: encoded = [tag(1B)] + [mask(8B)] + [values(dc B)]
 */
static inline SkelStrategy skel_encode_chunk(SkelEncCtx   *ctx,
                                              const uint8_t *chunk,
                                              SkeletonIdx   *out_sk,
                                              uint64_t       addr)
{
    *out_sk = skeleton_lookup(addr);
    SkelStrategy s = skel_decide(chunk, ctx->prev, ctx->has_prev);

    ctx->hits[s]++;
    ctx->chunk_count++;
    memcpy(ctx->prev, chunk, SKEL_CHUNK);
    ctx->has_prev = 1;
    return s;
}

#endif /* SKELETON_INDEX_H */

/*
 * cube_assembly.h — Phase C1: 6 LetterCubes → CubeCtx → recursive promote
 *
 * Pipeline:
 *   6 bonded LetterCubes → 1 CubeCtx (depth 0)
 *   6 CubeCtx → 1 parent CubeCtx (depth 1)
 *   ... up to depth 8
 *
 * Depends on: lettercube.h (pipeline LetterCube)
 * Uses CubeNode/ApexHeader from geo_letter_cube.h (core)
 */
#ifndef CUBE_ASSEMBLY_H
#define CUBE_ASSEMBLY_H

#include <stdint.h>
#include <string.h>

/* ── Constants from pipeline LetterCube ── */
#define LC_LANES        6u
#define LC_PAIRS_LC     24u   /* 24 complementary pairs */
#define LC_LANE_SZ      5u    /* pair:2 + angle:2 + state:1 */
#define LC_STATE_FREE   0u
#define LC_STATE_PEND   1u
#define LC_STATE_LOCK   2u
#define LC_OFFSET_LANES 0u
#define LC_OFFSET_STATE (LC_LANES * LC_LANE_SZ)     /* 30 */
#define LC_OFFSET_APEX  (LC_OFFSET_STATE + 1u)      /* 31 */
#define LC_BUF          (LC_OFFSET_APEX + 25u)       /* 56 */

/* Pipeline LetterCube functions */
static inline void lc_init(uint8_t *buf) {
    memset(buf, 0, LC_BUF);
}
static inline void lc_assign(uint8_t *buf, uint8_t lane, uint16_t pair, uint16_t angle) {
    if (lane >= LC_LANES) return;
    uint8_t *p = buf + lane * LC_LANE_SZ;
    p[0] = (uint8_t)(pair & 0xFFu);
    p[1] = (uint8_t)(pair >> 8);
    p[2] = (uint8_t)(angle & 0xFFu);
    p[3] = (uint8_t)(angle >> 8);
    p[4] = LC_STATE_PEND;
}
static inline void lc_bond(uint8_t *buf, uint8_t lane_a, uint8_t lane_b) {
    if (lane_a >= LC_LANES || lane_b >= LC_LANES) return;
    buf[lane_a * LC_LANE_SZ + 4] = LC_STATE_LOCK;
    buf[lane_b * LC_LANE_SZ + 4] = LC_STATE_LOCK;
    /* update state section bitfield */
    uint8_t state = buf[LC_OFFSET_STATE];
    state |= (1u << lane_a) | (1u << lane_b);
    buf[LC_OFFSET_STATE] = state;
}
static inline int lc_verify(const uint8_t *buf) {
    uint32_t v = 0;
    for (int i = 0; i < 56; i++) v += buf[i];
    return (v & 0xFFFF) == 0x5678;
}
static inline void lc_assemble(uint8_t *buf) {
    for (uint8_t lane = 0; lane < LC_LANES; lane++) {
        uint8_t *p = buf + lane * LC_LANE_SZ;
        if (p[4] == LC_STATE_PEND) p[4] = LC_STATE_LOCK;
    }
    uint8_t all = 0;
    for (uint8_t lane = 0; lane < LC_LANES; lane++) all |= (1u << lane);
    buf[LC_OFFSET_STATE] = all;
}
static inline void lc_fill_apex(uint8_t *buf, const uint8_t data[25]) {
    memcpy(buf + LC_OFFSET_APEX, data, 25);
}

/* ── CubeCtx (from core/geo_letter_cube.h, mirrored here for standalone) ── */
#define CUBE_FACES      6u
#define CUBE_MAX_DEPTH  8u
#define CUBE_PAIRS     24u   /* pipeline: 24 pairs, not 26 */

typedef struct {
    uint8_t upper;  /* 0..23 */
    uint8_t lower;  /* 0..23 */
} CubePair;

typedef struct {
    CubePair pair;
    uint8_t  depth;
    uint8_t  face_count;
    uint32_t angle_key;
    uint64_t slope_hash;
} CubeApexHeader;   /* 16B */

typedef struct {
    uint64_t   core;
    CubePair   key;
    uint8_t    face_id;
    uint8_t    coupled;
} CubeFaceNode;     /* 12B */

typedef struct {
    CubeApexHeader apex;
    CubeFaceNode   faces[CUBE_FACES];
    uint8_t        coupled_count;
    uint8_t        depth;
} CubeCtx;          /* ~92B */

/* ── Init ── */
static inline void cube_ctx_init(CubeCtx *c, uint8_t pair_idx, uint8_t depth) {
    memset(c, 0, sizeof(*c));
    c->apex.pair.upper = pair_idx;
    c->apex.pair.lower = pair_idx;
    c->apex.depth = depth;
    c->depth = depth;
    for (uint8_t i = 0; i < CUBE_FACES; i++)
        c->faces[i].face_id = i;
}

/* ── Map LetterCube → CubeCtx ── */
/*
 * Pipeline LetterCube has 6 lanes (LC_LANES) with pair:2 + angle:2 + state:1.
 * We map each lane to a CubeFaceNode in the CubeCtx.
 *
 * The LetterCube apex (25B) is used as the CubeCtx's slope_hash.
 * The 6 lane pair IDs are used as the CubeCtx face keys.
 */
static inline void cube_ctx_from_lettercube(CubeCtx *out, const uint8_t lc_buf[LC_BUF]) {
    memset(out, 0, sizeof(*out));

    /* Extract apex from LetterCube (25 bytes) */
    const uint8_t *apex = lc_buf + LC_OFFSET_APEX;

    /* Build slope_hash from apex (25 bytes → 8 bytes via XOR-fold) */
    uint64_t slope = 0;
    for (int i = 0; i < 24; i += 8) {
        uint64_t w;
        memcpy(&w, apex + i, 8);
        slope ^= w;
    }
    out->apex.slope_hash = slope;

    /* Set depth from apex byte 24 (count used as proxy) */
    out->apex.depth = 0;  /* base level */
    out->depth = 0;

    /* Map 6 lanes → 6 CubeFaceNodes */
    for (uint8_t lane = 0; lane < LC_LANES && lane < CUBE_FACES; lane++) {
        const uint8_t *l = lc_buf + lane * LC_LANE_SZ;

        uint16_t pair_raw = l[0] | ((uint16_t)l[1] << 8);
        uint16_t angle_raw = l[2] | ((uint16_t)l[3] << 8);
        uint8_t state = l[4];

        CubeFaceNode *fn = &out->faces[lane];
        fn->face_id = lane;
        fn->coupled = (state == LC_STATE_LOCK) ? 1 : 0;

        /* Map pair_raw (0..23) to CubePair */
        uint8_t pair_idx = pair_raw % CUBE_PAIRS;
        fn->key.upper = pair_idx;
        fn->key.lower = pair_idx;

        /* Core seed from angle_raw + apex */
        fn->core = ((uint64_t)angle_raw << 48) ^ slope ^ (lane * 0x9E3779B97F4A7C15ULL);

        if (fn->coupled) out->coupled_count++;
    }
}

/* ── Pair match (angle XOR + pair agree) ── */
static inline int cube_pair_match(const CubeCtx *c, uint8_t fa, uint8_t fb) {
    if (fa >= CUBE_FACES || fb >= CUBE_FACES || fa == fb) return 0;
    const CubeFaceNode *a = &c->faces[fa];
    const CubeFaceNode *b = &c->faces[fb];

    int pair_ok = (a->key.upper == b->key.upper) && (a->key.lower == b->key.lower);
    if (!pair_ok) return 0;

    uint64_t angle_xor = a->core ^ b->core;
    return ((angle_xor ^ c->apex.slope_hash) & 0xFFFFu) == 0;
}

/* ── Force match: scan pairs, couple on hit ── */
static inline uint8_t cube_force_match(CubeCtx *c) {
    uint8_t new_coupled = 0;
    for (uint8_t fa = 0; fa < CUBE_FACES && c->coupled_count < CUBE_FACES; fa++) {
        if (c->faces[fa].coupled) continue;
        for (uint8_t fb = fa + 1; fb < CUBE_FACES; fb++) {
            if (c->faces[fb].coupled) continue;
            if (cube_pair_match(c, fa, fb)) {
                c->faces[fa].coupled = 1;
                c->faces[fb].coupled = 1;
                c->coupled_count += 2;
                new_coupled += 2;
            }
        }
    }
    return new_coupled;
}

/* ── Promote: 6 CubeCtx → 1 parent ── */
static inline int cube_promote(const CubeCtx *children[CUBE_FACES],
                                CubeCtx *parent_out) {
    for (uint8_t i = 0; i < CUBE_FACES; i++)
        if (!children[i] || children[i]->coupled_count < CUBE_FACES)
            return 0;

    uint8_t base_depth = children[0]->depth;
    if (base_depth + 1 >= CUBE_MAX_DEPTH) return 0;

    cube_ctx_init(parent_out, children[0]->apex.pair.upper, base_depth + 1);

    uint64_t fold = 0;
    for (uint8_t i = 0; i < CUBE_FACES; i++) {
        fold ^= children[i]->apex.slope_hash;
        /* Assign child's apex as parent face's core */
        parent_out->faces[i].core = children[i]->apex.slope_hash;
        parent_out->faces[i].key.upper = i % CUBE_PAIRS;
        parent_out->faces[i].key.lower = i % CUBE_PAIRS;
        parent_out->faces[i].coupled = 1;
    }
    parent_out->coupled_count = CUBE_FACES;
    parent_out->apex.slope_hash = fold;
    return 1;
}

/* ── Serialize CubeCtx (92B flat) ── */
#define CUBE_CTX_SZ  92u

static inline void cube_ctx_serialize(uint8_t out[CUBE_CTX_SZ], const CubeCtx *c) {
    memcpy(out, c, CUBE_CTX_SZ);
}

static inline void cube_ctx_deserialize(CubeCtx *out, const uint8_t in[CUBE_CTX_SZ]) {
    memcpy(out, in, CUBE_CTX_SZ);
}

/* ── Verify: all 6 faces coupled ── */
static inline int cube_ctx_verify(const CubeCtx *c) {
    return c->coupled_count == CUBE_FACES;
}

/* ── Stats ── */
typedef struct {
    uint32_t n_cubes;       /* total CubeCtx created */
    uint32_t n_promoted;    /* successful promotions */
    uint8_t  max_depth;     /* deepest level reached */
    uint32_t n_paired;      /* total face pairs coupled */
} CubeAssemblyStats;

#endif /* CUBE_ASSEMBLY_H */

/*
 * geofield_pipeline.c — DLL wrapper for GeoField Pipeline
 *
 * Compiles all header-only components into a shared library.
 * Python ctypes can call these functions for encode/decode/verify.
 *
 * Build: gcc -O2 -std=c11 -shared -o geofield_pipeline.dll geofield_pipeline.c
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ── Include paths from compile_commands.json ──────────────────── */
/* -I. -Icore -Icollection/geo_jump_module/include                */

/* Core address space — zero external deps */
#include "geo_jump.h"

/* Frame seek — zero external deps */
#include "geo_frame_seek.h"

/* Tring walk */
#include "geo_tring_walk.h"

/* Adaptive flow chunker */
#include "geo_flow_chunker.h"

/* LetterCube — 24-pair face:face bond */
#include "lettercube.h"

/* ════════════════════════════════════════════════════════════════
   LAYER 1: Spec functions (immutable, deterministic)
   ════════════════════════════════════════════════════════════════ */

/* ── geo_jump routing ──────────────────────────────────────────── */

GEO_JUMP_API uint32_t geofield_geo_jump(uint32_t node_id,
                                         uint32_t type,
                                         uint32_t param)
{
    return geo_jump(node_id, (GeoJumpType)type, param);
}

/* ── frame_enc / frame_at ──────────────────────────────────────── */

GEO_JUMP_API uint16_t geofield_frame_enc(uint32_t t)
{
    return frame_enc(t);
}

GEO_JUMP_API uint16_t geofield_frame_next(uint16_t enc)
{
    return frame_next(enc);
}

GEO_JUMP_API uint16_t geofield_frame_cpair(uint16_t enc)
{
    return frame_cpair(enc);
}

/* ── tring walk ────────────────────────────────────────────────── */

GEO_JUMP_API uint16_t geofield_tring_walk_enc(uint32_t tile_id)
{
    return tring_walk_enc(tile_id);
}

GEO_JUMP_API uint8_t geofield_tring_walk_spoke(uint32_t tile_id)
{
    return tring_walk_spoke(tile_id);
}

/* ── gp_chunk_to_addr ──────────────────────────────────────────── */

typedef struct {
    uint32_t tile_id;
    uint8_t  dim;
} GeoGpAddr;

GEO_JUMP_API GeoGpAddr geofield_chunk_to_addr(uint8_t gp_level,
                                               uint64_t chunk_idx)
{
    uint32_t face_max = 10u * gp_level * gp_level + 2u;
    GeoGpAddr a;
    a.tile_id = (uint32_t)(chunk_idx % face_max);
    a.dim = (uint8_t)((chunk_idx / face_max) & 0x7Fu);
    return a;
}

GEO_JUMP_API uint32_t geofield_face_count(uint8_t gp_level)
{
    return 10u * gp_level * gp_level + 2u;
}

/* ════════════════════════════════════════════════════════════════
   LAYER 2: Adaptive chunking
   ════════════════════════════════════════════════════════════════ */

GEO_JUMP_API uint32_t geofield_adaptive_chunk_size(uint8_t shell_level)
{
    /* shell_level 0..8 → chunk size = 2n+1 */
    if (shell_level > 8) shell_level = 8;
    return (uint32_t)(2u * shell_level + 1u);
}

GEO_JUMP_API uint32_t geofield_adaptive_slot_count(uint8_t shell_level)
{
    uint32_t sz = geofield_adaptive_chunk_size(shell_level);
    return sz * sz * sz;
}

/* ════════════════════════════════════════════════════════════════
   LAYER 3: Skeleton (stats / classification)
   ════════════════════════════════════════════════════════════════ */

/* XOR-fold popcount — fast randomness detector */
GEO_JUMP_API uint8_t geofield_isect_pop(const uint8_t chunk[64])
{
    const uint64_t *w = (const uint64_t *)chunk;
    uint64_t fold = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4] ^ w[5] ^ w[6] ^ w[7];
    /* popcount64 */
    fold = fold - ((fold >> 1) & 0x5555555555555555ULL);
    fold = (fold & 0x3333333333333333ULL) + ((fold >> 2) & 0x3333333333333333ULL);
    fold = (fold + (fold >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (uint8_t)((fold * 0x0101010101010101ULL) >> 56);
}

/* Check if chunk is all zeros */
GEO_JUMP_API int geofield_is_flat(const uint8_t chunk[64])
{
    uint64_t acc = 0;
    const uint64_t *w = (const uint64_t *)chunk;
    for (int i = 0; i < 8; i++) acc |= w[i];
    return acc == 0;
}

/* Count differing bytes */
GEO_JUMP_API uint8_t geofield_diff_count(const uint8_t a[64],
                                          const uint8_t b[64])
{
    uint8_t count = 0;
    for (int i = 0; i < 64; i++) {
        if (a[i] != b[i]) count++;
    }
    return count;
}

/* Check byte-reverse match (BREF) */
GEO_JUMP_API int geofield_is_bref(const uint8_t cur[64],
                                   const uint8_t prev[64])
{
    for (int i = 0; i < 64; i++) {
        if (cur[i] != prev[63 - i]) return 0;
    }
    return 1;
}

/* ── Skeleton decision (P0-P5) ─────────────────────────────────── */

#define SKEL_ID     0
#define SKEL_FLAT   1
#define SKEL_DIFF   2
#define SKEL_BREF   3
#define SKEL_GEOM   4
#define SKEL_RAW    5

typedef struct {
    uint8_t  strategy;
    uint8_t  best_rot;
    uint8_t  isect_pc;
    uint8_t  ref_idx;
    uint8_t  diff_count;
} SkelDecision;

GEO_JUMP_API SkelDecision geofield_skel_decide(const uint8_t chunk[64],
                                                const uint8_t prev[64],
                                                int has_prev)
{
    SkelDecision sd = { SKEL_RAW, 0, 0, 0, 64 };

    /* P0: IDENTITY */
    if (has_prev && memcmp(chunk, prev, 64) == 0) {
        sd.strategy = SKEL_ID;
        sd.diff_count = 0;
        return sd;
    }

    /* P1: fast reject — high entropy → RAW */
    uint8_t ip = geofield_isect_pop(chunk);
    sd.isect_pc = ip;
    if (ip >= 16) {
        sd.strategy = SKEL_RAW;
        return sd;
    }

    /* P2: FLAT (all zero) */
    if (geofield_is_flat(chunk)) {
        sd.strategy = SKEL_FLAT;
        sd.diff_count = 0;
        return sd;
    }

    /* P3: DIFF */
    if (has_prev) {
        uint8_t dc = geofield_diff_count(chunk, prev);
        sd.diff_count = dc;
        if (dc >= 1 && dc <= 48) {
            sd.strategy = SKEL_DIFF;
            return sd;
        }
        /* P4: BREF */
        if (geofield_is_bref(chunk, prev)) {
            sd.strategy = SKEL_BREF;
            return sd;
        }
    }

    /* P5: GEOM (structured but uncompressible) */
    sd.strategy = SKEL_GEOM;
    return sd;
}

/* ════════════════════════════════════════════════════════════════
   LAYER 4: Diamond Shell rotation scan
   ════════════════════════════════════════════════════════════════ */

/* 6 rotations of 64B chunk treated as 4×4×4 cube */
GEO_JUMP_API void geofield_rotate64(uint8_t out[64],
                                     const uint8_t in[64],
                                     uint8_t rot)
{
    /* Simple byte-level rotation — re-index as 4×4×4 cube */
    for (int i = 0; i < 64; i++) {
        int x = (i >> 0) & 3;
        int y = (i >> 2) & 3;
        int z = (i >> 4) & 3;
        int nx, ny, nz;
        switch (rot) {
            case 0: nx=x; ny=y; nz=z; break;  /* identity */
            case 1: nx=y; ny=z; nz=x; break;  /* rotate Y */
            case 2: nx=z; ny=x; nz=y; break;  /* rotate X */
            case 3: nx=3-x; ny=y; nz=z; break; /* mirror X */
            case 4: nx=x; ny=3-y; nz=z; break; /* mirror Y */
            case 5: nx=x; ny=y; nz=3-z; break; /* mirror Z */
            default: nx=x; ny=y; nz=z; break;
        }
        out[nx + ny*4 + nz*16] = in[i];
    }
}

/* fibo_intersect: AND of 4 rotated copies */
GEO_JUMP_API uint64_t geofield_fibo_intersect(const uint8_t chunk[64])
{
    /* take first 8 bytes as core, build 4 copies */
    uint64_t core = 0;
    memcpy(&core, chunk, 8);
    uint64_t c0 = core;
    uint64_t c1 = core >> 1;  /* shift = 1 */
    uint64_t c2 = core >> 2;  /* shift = 2 */
    uint64_t c3 = core >> 3;  /* shift = 3 */
    return c0 & c1 & c2 & c3;
}

typedef struct {
    uint8_t best_rot;
    uint8_t isect_pc;
    uint8_t flag;   /* 0=FLAT, 1=SPARSE, 2=DENSE */
} DiamondClassify;

GEO_JUMP_API DiamondClassify geofield_diamond_classify(const uint8_t chunk[64])
{
    DiamondClassify dc = { 0, 0, 2 };
    uint8_t best_pc = 0;

    for (uint8_t rot = 0; rot < 6; rot++) {
        uint8_t rotated[64];
        geofield_rotate64(rotated, chunk, rot);
        uint64_t isect = geofield_fibo_intersect(rotated);
        /* popcount */
        uint64_t x = isect;
        x = x - ((x >> 1) & 0x5555555555555555ULL);
        x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
        x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
        uint8_t pc = (uint8_t)((x * 0x0101010101010101ULL) >> 56);
        if (pc > best_pc) {
            best_pc = pc;
            dc.best_rot = rot;
        }
    }

    dc.isect_pc = best_pc;
    if (best_pc == 0) dc.flag = 0;       /* FLAT */
    else if (best_pc <= 4) dc.flag = 1;  /* SPARSE */
    else dc.flag = 2;                     /* DENSE */

    return dc;
}

/* ════════════════════════════════════════════════════════════════
   LAYER 5: wallet_chunk_seed (XOR-fold + SplitMix64)
   ════════════════════════════════════════════════════════════════ */

GEO_JUMP_API uint64_t geofield_wallet_seed(const uint8_t chunk[64])
{
    const uint64_t *w = (const uint64_t *)chunk;
    uint64_t s = 0;
    for (int i = 0; i < 8; i++) s ^= w[i];

    /* SplitMix64 finalizer */
    s ^= s >> 33;
    s *= 0xff51afd7ed558ccdULL;
    s ^= s >> 33;
    s *= 0xc4ceb9fe1a85ec53ULL;
    s ^= s >> 33;
    return s;
}

/* ════════════════════════════════════════════════════════════════
   LAYER 6: xxh64 digest
   ════════════════════════════════════════════════════════════════ */

GEO_JUMP_API uint64_t geofield_xxh64(const uint8_t *data, size_t len)
{
    const uint64_t PRIME64_1 = 0x9E3779B185EBCA87ULL;
    const uint64_t PRIME64_2 = 0x14DEF9DEA2F79CD6ULL;
    const uint64_t PRIME64_3 = 0x165667B19E3779F9ULL;
    const uint64_t PRIME64_4 = 0x85EBCA77C2B2ED6BULL;
    const uint64_t PRIME64_5 = 0x27D4EB2F165667C5ULL;

    uint64_t v1 = PRIME64_5 + 8;
    uint64_t v2 = PRIME64_4;
    uint64_t v3 = 0;
    uint64_t v4 = PRIME64_1;

    size_t offset = 0;
    while (offset + 32 <= len) {
        const uint64_t *p = (const uint64_t *)(data + offset);
        v1 = ((v1 + p[0] * PRIME64_2) >> 31) * PRIME64_1;
        v2 = ((v2 + p[1] * PRIME64_2) >> 31) * PRIME64_1;
        v3 = ((v3 + p[2] * PRIME64_2) >> 31) * PRIME64_1;
        v4 = ((v4 + p[3] * PRIME64_2) >> 31) * PRIME64_1;
        offset += 32;
    }

    uint64_t result = len;
    if (offset < len) {
        uint64_t buf[4] = {0};
        memcpy(buf, data + offset, len - offset);
        v1 += buf[0] * PRIME64_2;
        v1 = ((v1 >> 31) * PRIME64_1);
        v2 += buf[1] * PRIME64_2;
        v2 = ((v2 >> 31) * PRIME64_1);
        v3 += buf[2] * PRIME64_2;
        v3 = ((v3 >> 31) * PRIME64_1);
        v4 += buf[3] * PRIME64_2;
        v4 = ((v4 >> 31) * PRIME64_1);
    }

    result = (v1 << 1) + (v2 << 7) + (v3 << 12) + (v4 << 18);
    result = ((result ^ (v1 >> 33)) * PRIME64_2) + PRIME64_3;
    result = ((result ^ (v2 >> 29)) * PRIME64_3) + PRIME64_4;
    result = ((result ^ (v3 >> 32)) * PRIME64_4) + PRIME64_5;

    return result;
}

/* ════════════════════════════════════════════════════════════════
   Batch operations (for Python performance)
   ════════════════════════════════════════════════════════════════ */

GEO_JUMP_API void geofield_batch_seed(const uint8_t *chunks,
                                       uint64_t *seeds,
                                       uint32_t n_chunks,
                                       uint32_t chunk_sz)
{
    for (uint32_t i = 0; i < n_chunks; i++) {
        seeds[i] = geofield_wallet_seed(chunks + (size_t)i * chunk_sz);
    }
}

GEO_JUMP_API void geofield_batch_xxh64(const uint8_t *chunks,
                                         uint64_t *digests,
                                         uint32_t n_chunks,
                                         uint32_t chunk_sz)
{
    for (uint32_t i = 0; i < n_chunks; i++) {
        digests[i] = geofield_xxh64(chunks + (size_t)i * chunk_sz, chunk_sz);
    }
}

/* ════════════════════════════════════════════════════════════════
   Adaptive flow chunking
   ════════════════════════════════════════════════════════════════ */

#include "geo_flow_chunker.h"

GEO_JUMP_API int geofield_flow_chunk(const uint8_t *data, size_t data_sz,
                                      uint32_t min_chunk, uint32_t max_chunk,
                                      uint64_t *out_offsets, uint64_t *out_lengths,
                                      uint32_t max_out)
{
    FlowSegment *segs = NULL;
    int n = flow_chunk(data, data_sz, &segs, max_out,
                       min_chunk ? min_chunk : FLOW_MIN_CHUNK,
                       max_chunk ? max_chunk : FLOW_MAX_CHUNK);
    if (n <= 0) return n;
    for (int i = 0; i < n && (uint32_t)i < max_out; i++) {
        out_offsets[i] = segs[i].offset;
        out_lengths[i] = segs[i].length;
    }
    flow_segments_free(segs);
    return n;
}

GEO_JUMP_API uint32_t geofield_shell_chunk_size(uint8_t level)
{
    return (2u * level + 1u);
}

GEO_JUMP_API uint32_t geofield_shell_slot_count(uint8_t level)
{
    uint32_t d = 2u * level + 1u;
    return d * d * d;
}

/* ════════════════════════════════════════════════════════════════
   LETTERCUBE: 24-pair face:face bond
   ════════════════════════════════════════════════════════════════ */

#define LC_BUF_SZ  76  /* LC_SERIALIZED_SZ */

GEO_JUMP_API void geofield_lc_init(uint8_t *buf)
{
    LetterCube cube;
    lc_init(&cube);
    lc_serialize(&cube, buf);
}

GEO_JUMP_API void geofield_lc_assign(uint8_t *buf, uint8_t lane, uint8_t pair_id, uint8_t angle)
{
    LetterCube cube;
    lc_deserialize(&cube, buf);
    lc_assign_lane(&cube, lane, pair_id, angle);
    lc_serialize(&cube, buf);
}

GEO_JUMP_API int geofield_lc_bond(uint8_t *buf, uint8_t lane_a, uint8_t lane_b)
{
    LetterCube cube;
    lc_deserialize(&cube, buf);
    int r = lc_bond(&cube, lane_a, lane_b);
    lc_serialize(&cube, buf);
    return r;
}

GEO_JUMP_API int geofield_lc_assemble(uint8_t *buf)
{
    LetterCube cube;
    lc_deserialize(&cube, buf);
    int r = lc_assemble(&cube);
    lc_serialize(&cube, buf);
    return r;
}

GEO_JUMP_API int geofield_lc_verify(const uint8_t *buf)
{
    LetterCube cube;
    lc_deserialize(&cube, buf);
    return lc_verify(&cube);
}

GEO_JUMP_API uint8_t geofield_lc_n_locked(const uint8_t *buf)
{
    return buf[LC_LANES * 12 + 0];  /* n_locked offset in serialized layout */
}

GEO_JUMP_API uint8_t geofield_lc_pair_id(const uint8_t *buf, uint8_t lane)
{
    return buf[lane * 12 + 0];
}

GEO_JUMP_API uint8_t geofield_lc_bonded_to(const uint8_t *buf, uint8_t lane)
{
    return buf[lane * 12 + 3];
}

GEO_JUMP_API uint8_t geofield_lc_bond_state(const uint8_t *buf, uint8_t lane)
{
    return buf[lane * 12 + 2];
}

/* ════════════════════════════════════════════════════════════════
   CUBE CONTEXT: 6 LetterCube faces → 1 CubeCtx
   ════════════════════════════════════════════════════════════════ */

#define CC_FACES       6
#define CC_CTX_SZ      (CC_FACES * LC_SERIALIZED_SZ + 16)  /* 6*76+16=472 bytes */

/* CubeCtx layout:
 *   [0..455]   6 × LetterCube (76 bytes each)
 *   [456]      depth (uint8)
 *   [457]      angle_key (uint8)
 *   [458..459] slope_hash (uint16)
 *   [460..463] apex_wire[0] (uint32)
 *   [464..467] apex_wire[1] (uint32)
 *   [468..471] apex_wire[2] (uint32)
 */
#define CC_DEPTH_OFF   (CC_FACES * LC_SERIALIZED_SZ)
#define CC_ANGLE_OFF   (CC_DEPTH_OFF + 1)
#define CC_SLOPE_OFF   (CC_ANGLE_OFF + 1)
#define CC_WIRE_OFF    (CC_SLOPE_OFF + 2)

GEO_JUMP_API void geofield_cube_ctx_init(uint8_t *ctx)
{
    memset(ctx, 0, CC_CTX_SZ);
    /* Depth 0, angle_key = first lane pair_id, slope_hash = XOR all seeds */
    ctx[CC_DEPTH_OFF] = 0;
    ctx[CC_ANGLE_OFF] = 0;
    uint16_t sh = 0;
    for (int i = 0; i < CC_FACES; i++) {
        uint8_t lc[LC_SERIALIZED_SZ];
        memcpy(lc, ctx + i * LC_SERIALIZED_SZ, LC_SERIALIZED_SZ);
        sh ^= (uint16_t)lc[0] << 8 | lc[1];  /* pair_id << 8 | angle */
    }
    memcpy(ctx + CC_SLOPE_OFF, &sh, 2);
}

GEO_JUMP_API int geofield_cube_ctx_from_lc(uint8_t *ctx,
                                            const uint8_t lc_bufs[CC_FACES][LC_SERIALIZED_SZ])
{
    for (int i = 0; i < CC_FACES; i++) {
        memcpy(ctx + i * LC_SERIALIZED_SZ, lc_bufs[i], LC_SERIALIZED_SZ);
    }
    ctx[CC_DEPTH_OFF] = 0;
    ctx[CC_ANGLE_OFF] = lc_bufs[0][0];  /* pair_id of lane 0 */

    uint16_t sh = 0;
    for (int i = 0; i < CC_FACES; i++) {
        sh ^= (uint16_t)lc_bufs[i][0] << 8 | lc_bufs[i][1];
    }
    memcpy(ctx + CC_SLOPE_OFF, &sh, 2);

    /* Compute apex_wires: 3 pairs of complementary face routing */
    uint32_t w0 = 0, w1 = 0, w2 = 0;
    for (int i = 0; i < CC_FACES; i++) {
        uint8_t pid = lc_bufs[i][0];
        if (pid < 12) {
            w0 |= (1u << i);
        } else if (pid < 18) {
            w1 |= (1u << i);
        } else {
            w2 |= (1u << i);
        }
    }
    memcpy(ctx + CC_WIRE_OFF,      &w0, 4);
    memcpy(ctx + CC_WIRE_OFF + 4,  &w1, 4);
    memcpy(ctx + CC_WIRE_OFF + 8,  &w2, 4);

    return 1;
}

GEO_JUMP_API int geofield_cube_ctx_coupled(const uint8_t *ctx)
{
    int coupled = 0;
    for (int i = 0; i < CC_FACES; i++) {
        uint8_t state = ctx[i * LC_SERIALIZED_SZ + 2];
        if (state == LC_BOND_LOCK) coupled++;
    }
    return coupled;
}

GEO_JUMP_API int geofield_cube_ctx_verify(const uint8_t *ctx)
{
    for (int i = 0; i < CC_FACES; i++) {
        uint8_t lc[LC_SERIALIZED_SZ];
        memcpy(lc, ctx + i * LC_SERIALIZED_SZ, LC_SERIALIZED_SZ);
        LetterCube cube;
        lc_deserialize(&cube, lc);
        if (!lc_verify(&cube)) return 0;
    }
    return 1;
}

GEO_JUMP_API uint8_t geofield_cube_ctx_depth(const uint8_t *ctx)
{
    return ctx[CC_DEPTH_OFF];
}

GEO_JUMP_API uint16_t geofield_cube_ctx_slope_hash(const uint8_t *ctx)
{
    uint16_t sh;
    memcpy(&sh, ctx + CC_SLOPE_OFF, 2);
    return sh;
}

/* ════════════════════════════════════════════════════════════════
   CUBE PROMOTE: 6 CubeCtx → 1 parent CubeCtx (recursive)
   ════════════════════════════════════════════════════════════════ */

GEO_JUMP_API int geofield_cube_promote(const uint8_t *children[CC_FACES],
                                        uint8_t *parent)
{
    /* Parent gets depth = max child depth + 1 */
    uint8_t max_depth = 0;
    for (int i = 0; i < CC_FACES; i++) {
        uint8_t d = children[i][CC_DEPTH_OFF];
        if (d > max_depth) max_depth = d;
    }
    memset(parent, 0, CC_CTX_SZ);
    parent[CC_DEPTH_OFF] = max_depth + 1;

    /* For each child, extract its primary LetterCube face (lane 0) */
    uint8_t lc_bufs[CC_FACES][LC_SERIALIZED_SZ];
    for (int i = 0; i < CC_FACES; i++) {
        memcpy(lc_bufs[i], children[i], LC_SERIALIZED_SZ);
        /* Angle shift by parent depth for diversity */
        uint8_t angle = (lc_bufs[i][1] + max_depth + 1) % 6;
        lc_bufs[i][1] = angle;
    }

    /* Fill parent using cube_ctx_from_lc logic */
    for (int i = 0; i < CC_FACES; i++) {
        memcpy(parent + i * LC_SERIALIZED_SZ, lc_bufs[i], LC_SERIALIZED_SZ);
    }
    parent[CC_ANGLE_OFF] = lc_bufs[0][0];

    uint16_t sh = 0;
    for (int i = 0; i < CC_FACES; i++) {
        sh ^= (uint16_t)lc_bufs[i][0] << 8 | lc_bufs[i][1];
    }
    memcpy(parent + CC_SLOPE_OFF, &sh, 2);

    /* Apex wires */
    uint32_t w0 = 0, w1 = 0, w2 = 0;
    for (int i = 0; i < CC_FACES; i++) {
        uint8_t pid = lc_bufs[i][0];
        if (pid < 12)      w0 |= (1u << i);
        else if (pid < 18) w1 |= (1u << i);
        else                w2 |= (1u << i);
    }
    memcpy(parent + CC_WIRE_OFF,      &w0, 4);
    memcpy(parent + CC_WIRE_OFF + 4,  &w1, 4);
    memcpy(parent + CC_WIRE_OFF + 8,  &w2, 4);

    return 1;
}

/* ════════════════════════════════════════════════════════════════
   GOLDBERG SPHERE: Cube shell → hexagon mapping
   ════════════════════════════════════════════════════════════════ */

#define GP_PENT_COUNT  12
#define GP_MAX_LEVEL   8
#define GP_CHUNK_SZ    64

GEO_JUMP_API uint32_t geofield_gp_face_count(uint8_t level)
{
    return 10u * (uint32_t)level * (uint32_t)level + 2u;
}

GEO_JUMP_API int geofield_gp_is_pentagon(uint32_t tile_id)
{
    return tile_id < GP_PENT_COUNT;
}

GEO_JUMP_API uint32_t geofield_gp_hex_in_sector(uint8_t level, uint8_t sector)
{
    uint32_t total_hex = geofield_gp_face_count(level) - GP_PENT_COUNT;
    uint32_t base = total_hex / GP_PENT_COUNT;
    uint32_t rem = total_hex % GP_PENT_COUNT;
    return base + ((sector < rem) ? 1 : 0);
}

GEO_JUMP_API uint32_t geofield_gp_sector_base(uint8_t level, uint8_t sector)
{
    uint32_t b = GP_PENT_COUNT;
    for (uint8_t s = 0; s < sector; s++) {
        b += geofield_gp_hex_in_sector(level, s);
    }
    return b;
}

GEO_JUMP_API uint32_t geofield_gp_tile_id(uint8_t level,
                                            uint8_t pent_anchor,
                                            uint8_t hex_offset)
{
    if (pent_anchor >= GP_PENT_COUNT) return 0;
    if (hex_offset == 0) return pent_anchor;
    return geofield_gp_sector_base(level, pent_anchor) + (hex_offset - 1);
}

GEO_JUMP_API uint8_t geofield_gp_tile_to_pent(uint8_t level, uint32_t tile_id)
{
    if (tile_id < GP_PENT_COUNT) return (uint8_t)tile_id;
    for (uint8_t s = 0; s < GP_PENT_COUNT; s++) {
        uint32_t base = geofield_gp_sector_base(level, s);
        uint32_t sz = geofield_gp_hex_in_sector(level, s);
        if (base <= tile_id && tile_id < base + sz) return s;
    }
    return GP_PENT_COUNT - 1;
}

GEO_JUMP_API void geofield_gp_chunk_to_addr(uint8_t level,
                                             uint32_t chunk_idx,
                                             uint32_t *out_tile_id,
                                             uint8_t *out_dim)
{
    uint32_t face_max = geofield_gp_face_count(level);
    *out_tile_id = chunk_idx % face_max;
    *out_dim = (uint8_t)((chunk_idx / face_max) & 0x7Fu);
}

GEO_JUMP_API uint32_t geofield_gp_addr_to_chunk(uint8_t level,
                                                  uint32_t tile_id,
                                                  uint8_t dim)
{
    uint32_t face_max = geofield_gp_face_count(level);
    return dim * face_max + tile_id;
}

GEO_JUMP_API uint8_t geofield_gp_choose_level(uint32_t n_faces)
{
    for (uint8_t level = 1; level <= GP_MAX_LEVEL; level++) {
        if (geofield_gp_face_count(level) >= n_faces + GP_PENT_COUNT)
            return level;
    }
    return GP_MAX_LEVEL;
}

GEO_JUMP_API uint32_t geofield_gp_map_face(uint8_t level,
                                             uint8_t face_id,
                                             uint32_t *out_tile_id)
{
    uint32_t hex_idx = 0;
    for (uint8_t f = 0; f <= face_id; f++) {
        while (hex_idx < geofield_gp_face_count(level)) {
            uint32_t tid = GP_PENT_COUNT + hex_idx;
            if (!geofield_gp_is_pentagon(tid)) {
                if (f == face_id) {
                    *out_tile_id = tid;
                    return tid;
                }
                hex_idx++;
                break;
            }
            hex_idx++;
        }
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════
   FIBONACCI SHELL FOLD: Layer existence on fibo clock ticks
   ════════════════════════════════════════════════════════════════ */

static const uint16_t GEO_FIBO[12] = {1,1,2,3,5,8,13,21,34,55,89,144};

GEO_JUMP_API int geofield_shell_layer_live(uint8_t layer, uint32_t tick)
{
    return (tick % GEO_FIBO[layer % 12u]) == 0;
}

GEO_JUMP_API uint8_t geofield_shell_fold_nearest(uint8_t layer,
                                                   uint32_t tick,
                                                   uint8_t pent_axis)
{
    if (pent_axis) return layer;
    layer %= 12u;
    if (tick % GEO_FIBO[layer] == 0) return layer;
    for (uint8_t delta = 1; delta < 12; delta++) {
        if (layer >= delta) {
            uint8_t lo = layer - delta;
            if (tick % GEO_FIBO[lo] == 0) return lo;
        }
        uint8_t hi = layer + delta;
        if (hi < 12 && tick % GEO_FIBO[hi] == 0) return hi;
    }
    return 0;
}

GEO_JUMP_API uint32_t geofield_ring_hot_path(uint8_t pent_id,
                                               uint8_t layer,
                                               uint8_t globe)
{
    uint32_t face_stride = 20736u / GP_PENT_COUNT;
    uint32_t base = (pent_id % GP_PENT_COUNT) * face_stride;
    uint32_t offset = (globe == 0) ? 0 : face_stride / 2;
    return (base + layer * 12 + offset) % 20736u;
}

/* ═══════════════════════════════════════════════════════════════
   Phase 3: DRamTile (PipelineStore) backed pipeline
   ═══════════════════════════════════════════════════════════════ */

#include "pipeline_store.h"

typedef struct {
    PipelineStore store;
    int           initialized;
} GFDTContext;

GEO_JUMP_API void *geofield_dt_init(uint32_t capacity_mb) {
    GFDTContext *ctx = (GFDTContext *)calloc(1, sizeof(GFDTContext));
    if (!ctx) return NULL;
    size_t cap = (size_t)capacity_mb * 1024 * 1024;
    if (ps_init(&ctx->store, cap) != 0) { free(ctx); return NULL; }
    ctx->initialized = 1;
    return ctx;
}

GEO_JUMP_API void geofield_dt_destroy(void *handle) {
    GFDTContext *ctx = (GFDTContext *)handle;
    if (ctx && ctx->initialized) {
        ps_destroy(&ctx->store);
        ctx->initialized = 0;
    }
    free(ctx);
}

GEO_JUMP_API void *geofield_dt_put(void *handle, const char *name,
                                    const uint8_t *data, uint32_t size) {
    GFDTContext *ctx = (GFDTContext *)handle;
    if (!ctx || !ctx->initialized) return NULL;
    return ps_put(&ctx->store, name, data, size);
}

GEO_JUMP_API void *geofield_dt_get(void *handle, const char *name) {
    GFDTContext *ctx = (GFDTContext *)handle;
    if (!ctx || !ctx->initialized) return NULL;
    return ps_get(&ctx->store, name);
}

GEO_JUMP_API void geofield_dt_stats(void *handle) {
    GFDTContext *ctx = (GFDTContext *)handle;
    if (!ctx || !ctx->initialized) return;
    ps_print_stats(&ctx->store);
}

GEO_JUMP_API void geofield_dt_put_segs(void *handle,
    const uint8_t *data, uint64_t original_size,
    const uint64_t *offsets, const uint64_t *lengths, uint32_t n_segs)
{
    GFDTContext *ctx = (GFDTContext *)handle;
    if (!ctx || !ctx->initialized) return;
    char name[32];
    for (uint32_t i = 0; i < n_segs; i++) {
        snprintf(name, sizeof(name), "gf.seg.%u", i);
        ps_put(&ctx->store, name, data + offsets[i], (size_t)lengths[i]);
    }
}

GEO_JUMP_API void geofield_dt_get_segs(void *handle,
    uint8_t *out_data, uint64_t original_size,
    const uint64_t *offsets, const uint64_t *lengths, uint32_t n_segs)
{
    GFDTContext *ctx = (GFDTContext *)handle;
    if (!ctx || !ctx->initialized) return;
    char name[32];
    for (uint32_t i = 0; i < n_segs; i++) {
        snprintf(name, sizeof(name), "gf.seg.%u", i);
        uint8_t *ptr = ps_get(&ctx->store, name);
        if (ptr) {
            memcpy(out_data + offsets[i], ptr, (size_t)lengths[i]);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   Phase 4: GearShift priority routing for pipeline
   ═══════════════════════════════════════════════════════════════ */

#include "gear_shift.h"
#include <time.h>

typedef struct {
    GearShiftStore gs;
    void          *dt_handle;  /* DRamTile context for src provider */
    uint32_t       n_registered;
} GFGearShift;

GEO_JUMP_API void *geofield_gs_init(void *dt_handle) {
    GFGearShift *gfs = (GFGearShift *)calloc(1, sizeof(GFGearShift));
    if (!gfs) return NULL;
    gs_init(&gfs->gs);
    gfs->dt_handle = dt_handle;
    return gfs;
}

GEO_JUMP_API void geofield_gs_destroy(void *handle) {
    GFGearShift *gfs = (GFGearShift *)handle;
    if (gfs) { gs_destroy(&gfs->gs); free(gfs); }
}

GEO_JUMP_API int geofield_gs_register(void *handle, const char *name,
                                       float priority) {
    GFGearShift *gfs = (GFGearShift *)handle;
    if (!gfs) return -1;
    int rc = gs_register(&gfs->gs, name, 0);
    if (rc == 0) {
        GSEntry *e = gs_find(&gfs->gs, name);
        if (e) e->priority = priority;
        gfs->n_registered++;
    }
    return rc;
}

/* Default stream callback: memcpy src → dst_buf.
 * dst_ctx points to GFDstBuf, user_data is per-entry offset. */
typedef struct {
    uint8_t *buf;
    size_t   total_size;
    uint64_t written;   /* bytes written so far */
} GFDstBuf;

static int gf_memcpy_stream(const void *src_ptr, size_t src_size,
                             void *dst_ctx, void *user_data) {
    GFDstBuf *dst = (GFDstBuf *)dst_ctx;
    if (!dst || !dst->buf) return -1;
    size_t off = (size_t)(uintptr_t)user_data;
    if (off + src_size > dst->total_size) return -1;
    memcpy(dst->buf + off, src_ptr, src_size);
    dst->written += src_size;
    return 0;
}

/* Stream all registered segments, sorted by priority (highest first).
 * If no stream_fn set per entry, uses default memcpy to out_buf.
 * Returns number of segments streamed successfully. */
GEO_JUMP_API uint32_t geofield_gs_flush(void *handle) {
    GFGearShift *gfs = (GFGearShift *)handle;
    if (!gfs) return 0;

    /* Sort by priority (descending) — simple insertion sort */
    int n = gfs->gs.n_entries;
    for (int i = 1; i < n; i++) {
        GSEntry key = gfs->gs.entries[i];
        int j = i - 1;
        while (j >= 0 && gfs->gs.entries[j].priority < key.priority) {
            gfs->gs.entries[j + 1] = gfs->gs.entries[j];
            j--;
        }
        gfs->gs.entries[j + 1] = key;
    }

    /* Stream each entry */
    uint32_t ok = 0;
    for (int i = 0; i < n; i++) {
        GSEntry *e = &gfs->gs.entries[i];
        if (e->state == GS_IDLE && e->src_ptr && e->src_size > 0) {
            GSStreamFn fn = e->stream_fn ? e->stream_fn : gf_memcpy_stream;
            e->state = GS_STREAMING;
            e->access_tick = ++gfs->gs.tick;
            if (fn(e->src_ptr, e->src_size, e->dst_ctx, e->user_data) == 0) {
                e->state = GS_DONE;
                gfs->gs.n_streamed++;
                ok++;
            } else {
                e->state = GS_FAILED;
                gfs->gs.n_errors++;
            }
        }
    }
    return ok;
}

/* Register + stream pipeline: store segments in DRamTile, register with
 * GearShift, flush with priority sort → output buffer. */
GEO_JUMP_API uint32_t geofield_dt_gs_flush(void *dt_handle, void *gs_handle,
    const uint8_t *data, uint64_t original_size,
    uint8_t *out_buf, size_t out_size,
    const uint64_t *offsets, const uint64_t *lengths, uint32_t n_segs)
{
    GFDTContext *dt = (GFDTContext *)dt_handle;
    GFGearShift *gfs = (GFGearShift *)gs_handle;
    if (!dt || !gfs || !dt->initialized) return 0;

    GFDstBuf dst = { .buf = out_buf, .total_size = out_size, .written = 0 };

    char name[32];
    for (uint32_t i = 0; i < n_segs; i++) {
        snprintf(name, sizeof(name), "gf.seg.%u", i);
        uint8_t *seg_ptr = ps_get(&dt->store, name);
        if (!seg_ptr) continue;

        /* Register */
        gs_register(&gfs->gs, name, 0);
        GSEntry *e = gs_find(&gfs->gs, name);
        if (e) {
            e->src_ptr = seg_ptr;
            e->src_size = (size_t)lengths[i];
            e->priority = 1.0f - ((float)i / n_segs); /* first segments = higher priority */
            e->dst_ctx = &dst;
            e->user_data = (void *)(uintptr_t)offsets[i];
        }
    }

    /* Flush */
    uint32_t ok = geofield_gs_flush(gs_handle);
    return ok;
}

GEO_JUMP_API void geofield_gs_stats(void *handle) {
    GFGearShift *gfs = (GFGearShift *)handle;
    if (!gfs) return;
    gs_stats(&gfs->gs, stdout);
    fprintf(stdout, "  Registered: %u\n", gfs->n_registered);
}

/* ═══════════════════════════════════════════════════════════════
   Phase 5b: Full pipeline in one C call — no Python↔DLL overhead
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t n_segments;
    uint32_t n_blocks;
    uint32_t lc_verified;
    uint32_t skel_hits[6];   /* ID/FLAT/DIFF/BREF/GEOM/RAW */
    uint8_t  diamond_hits[3]; /* FLAT/SPARSE/DENSE */
    uint64_t xxh64;
    double   encode_ms;
    double   wall_ms;        /* high-res wall time */
} GFEncodeStats;

GEO_JUMP_API int geofield_full_encode(
    const uint8_t *data, uint64_t data_size,
    uint32_t min_chunk, uint32_t max_chunk,
    GFEncodeStats *out_stats)
{
    if (!data || data_size == 0 || !out_stats) return -1;
    memset(out_stats, 0, sizeof(*out_stats));

#ifdef _WIN32
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
#else
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
#endif

    out_stats->xxh64 = geofield_xxh64(data, data_size);

    /* Step 1: flow_chunk */
    uint32_t max_segs = (uint32_t)(data_size / min_chunk) + 256;
    uint64_t *offsets = (uint64_t *)malloc(max_segs * sizeof(uint64_t));
    uint64_t *lengths = (uint64_t *)malloc(max_segs * sizeof(uint64_t));
    if (!offsets || !lengths) { free(offsets); free(lengths); return -1; }

    uint32_t n_segs = geofield_flow_chunk(data, data_size, min_chunk, max_chunk,
                                           offsets, lengths, max_segs);
    out_stats->n_segments = n_segs;

    /* Step 2: per-block classify + skeleton */
    uint8_t block[64];
    uint8_t zeros[64];
    memset(zeros, 0, 64);
    uint32_t total_blocks = 0;

    for (uint32_t si = 0; si < n_segs; si++) {
        uint64_t seg_off = offsets[si];
        uint64_t seg_len = lengths[si];

        for (uint64_t bi = 0; bi < seg_len; bi += 64) {
            uint64_t bsz = (seg_len - bi > 64) ? 64 : (seg_len - bi);
            memset(block, 0, 64);
            memcpy(block, data + seg_off + bi, (size_t)bsz);

            /* Diamond classify */
            DiamondClassify dc = geofield_diamond_classify(block);
            out_stats->diamond_hits[dc.flag]++;

            /* Skeleton decide */
            SkelDecision sd = geofield_skel_decide(block, zeros, 0);
            out_stats->skel_hits[sd.strategy]++;

            total_blocks++;
        }
    }
    out_stats->n_blocks = total_blocks;

    /* Step 3: LetterCube + CubeCtx per segment */
    uint8_t lc_bufs[CC_FACES][LC_SERIALIZED_SZ];
    uint8_t cube_buf[CC_CTX_SZ];
    uint32_t lc_ok = 0;

    for (uint32_t si = 0; si < n_segs; si++) {
        /* Init 6 faces of LetterCube */
        for (int f = 0; f < CC_FACES; f++) {
            memset(lc_bufs[f], 0, LC_SERIALIZED_SZ);
            for (uint8_t lane = 0; lane < 6; lane++) {
                uint8_t pair = (lane + (si + f) % 12) % 24;
                uint8_t angle = (si + lane + f) % 6;
                geofield_lc_assign(lc_bufs[f], lane, pair, angle);
            }
            geofield_lc_bond(lc_bufs[f], 0, 1);
            geofield_lc_bond(lc_bufs[f], 2, 3);
            geofield_lc_bond(lc_bufs[f], 4, 5);
            geofield_lc_assemble(lc_bufs[f]);
            if (geofield_lc_verify(lc_bufs[f])) lc_ok++;
        }

        /* CubeCtx from 6 LetterCube faces */
        geofield_cube_ctx_init(cube_buf);
        geofield_cube_ctx_from_lc(cube_buf, lc_bufs);
    }
    out_stats->lc_verified = lc_ok;

    /* Step 4: Goldberg + Fibonacci (quick — O(1) per call) */
    uint32_t tile_id;
    for (uint32_t si = 0; si < n_segs; si++) {
        geofield_gp_map_face(2, si % 30, &tile_id);
        geofield_shell_layer_live(si % 12, si);
    }

#ifdef _WIN32
    QueryPerformanceCounter(&t1);
    out_stats->wall_ms = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart * 1000.0;
#else
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    out_stats->wall_ms = (ts1.tv_sec - ts0.tv_sec) * 1000.0 + (ts1.tv_nsec - ts0.tv_nsec) / 1e6;
#endif
    out_stats->encode_ms = out_stats->wall_ms; /* alias */

    free(offsets);
    free(lengths);
    return 0;
}

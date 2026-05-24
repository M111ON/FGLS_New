/*
 * geo_transform_seq.h — Transform sequence encoder
 *
 * Vector sequence encoding: consecutive 64B chunks in Hilbert traversal order
 * are often geometrically related (byte-rotation, grid transformation, minor diff).
 * Instead of storing each chunk independently, detect the transform from the
 * previous chunk and store only a tag + parameter (1-2 bytes total).
 *
 * Tags:
 *   TRANS_IDENTITY (5):  chunk[N] == chunk[N-1]          → 1B
 *   TRANS_BROT     (6):  chunk[N] = byte_rotate_k(chunk[N-1]) → 2B (tag + k)
 *   TRANS_BREF     (7):  chunk[N] = byte_reverse(chunk[N-1])   → 2B
 *   TRANS_D4       (8):  chunk[N] = d4_i(chunk[N-1])     → 2B (tag + i)
 *   TRANS_DIFF     (9):  chunk[N] differs by ≤32B → 10B + diff bytes
 *   Tags 0-4: fallback to smart_encode (FLAT/RAW/DIAMOND/BATCH/GEOMETRIC)
 *
 * Usage:
 *   ts_encode() wraps smart_encode: tries transforms first, falls through
 *   ts_decode() wraps smart_decode: applies inverse transform
 *
 * No changes to geo_smart_encode.h required — this is a wrapper layer.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "geo_smart_encode.h"

/* strategy tags for transforms (keep distinct from SMART_* 0-4) */
#define TRANS_IDENTITY  5u
#define TRANS_BROT      6u   /* byte rotation left by k */
#define TRANS_BREF      7u   /* byte reverse (mirror) */
#define TRANS_D4        8u   /* D4(8) 8×8 grid transform (0..7) */
#define TRANS_DIFF      9u   /* diff: ≤32 bytes differ, bitmask + values */

/* encoder context — wraps a SmartEncCtx */
typedef struct {
    SmartEncCtx  base;        /* underlying smart encoder */
    uint8_t      prev_chunk[64];  /* previously encoded chunk */
    int          has_prev;    /* 0 for first chunk */
    uint32_t     n_chunks;
    uint64_t    *xxh_table;   /* xxh64 of each chunk (for identity detect) */
    uint8_t    **stored_enc;  /* must be same size as SmartEncCtx.stored_enc */
    uint32_t    *stored_sz;
} TsEncCtx;

typedef struct {
    SmartDecCtx base;         /* underlying smart decoder */
    uint8_t     prev_decoded[64];
    int         has_prev;
} TsDecCtx;

/* ── transform detection helpers ───────────────────────────────── */

static inline int ts_is_identity(const uint8_t a[64], const uint8_t b[64]) {
    return memcmp(a, b, 64) == 0;
}

/* Find byte rotation k such that b = rotate_left(a, k). Returns -1 if none. */
static inline int ts_find_brot(const uint8_t a[64], const uint8_t b[64]) {
    for (int k = 1; k < 64; k++) {
        int match = 1;
        for (int i = 0; i < 64 && match; i++)
            if (b[i] != a[(i + k) % 64]) match = 0;
        if (match) return k;
    }
    return -1;
}

static inline int ts_is_bref(const uint8_t a[64], const uint8_t b[64]) {
    for (int i = 0; i < 64; i++)
        if (b[i] != a[63 - i]) return 0;
    return 1;
}

/* D4(8) grid transforms: use _sm_d4_apply from geo_smart_encode.h
 * Returns D4 index (0..7) if found, -1 if none. */
static inline int ts_find_d4(const uint8_t prev[64], const uint8_t cur[64]) {
    _sm_geo_init();
    uint8_t tmp[64];
    for (int i = 0; i < 8; i++) {
        _sm_d4_apply(tmp, prev, i);
        if (memcmp(tmp, cur, 64) == 0) return i;
    }
    return -1;
}

/* Find partial diff: count differing bytes, fill mask + values.
 * Returns count if ≤ 32, 0 if too many diffs. */
static inline int ts_find_diff(const uint8_t a[64], const uint8_t b[64],
                                uint64_t *mask, uint8_t *values) {
    uint64_t m = 0;
    int count = 0;
    for (int i = 0; i < 64; i++) {
        if (a[i] != b[i]) {
            m |= (1ULL << i);
            if (count < 64) values[count] = b[i];
            count++;
        }
    }
    *mask = m;
    return (count > 0 && count <= 32) ? count : 0;
}

/* ── encoder init/free ─────────────────────────────────────────── */

static inline void ts_enc_init(TsEncCtx *ctx, DiamondField *df, uint32_t n_chunks) {
    smart_enc_init(&ctx->base, df, n_chunks);
    memset(ctx->prev_chunk, 0, 64);
    ctx->has_prev = 0;
    ctx->n_chunks = n_chunks;
    ctx->xxh_table = calloc(n_chunks, sizeof(uint64_t));
    ctx->stored_enc = calloc(n_chunks, sizeof(uint8_t*));
    ctx->stored_sz  = calloc(n_chunks, sizeof(uint32_t));
    /* share the same storage pointer arrays */
    /* Note: stored_enc/stored_sz are SEPARATE from base.stored_enc/base.stored_sz.
     * The pipeline should use ts_ versions for reading back strategy breakdown.
     * Actually, let's use the base ones directly — just set them here too. */
    free(ctx->base.stored_enc);
    free(ctx->base.stored_sz);
    ctx->base.stored_enc = ctx->stored_enc;
    ctx->base.stored_sz  = ctx->stored_sz;
}

static inline void ts_enc_free(TsEncCtx *ctx) {
    /* stored_enc/sz freed by smart_enc_free */
    smart_enc_free(&ctx->base);
    free(ctx->xxh_table);
    /* stored_enc/sz were aliased to base's — base already freed them */
}

/* ── single-chunk transform encode ──────────────────────────────── */

static inline const uint8_t* ts_encode(TsEncCtx *ctx,
                                        const uint8_t chunk[64],
                                        uint32_t *out_sz,
                                        uint32_t seq_pos) {
    uint32_t pos = seq_pos;
    uint64_t h = sm_xxh64(chunk, 64);
    ctx->xxh_table[pos] = h;

    /* If we have a previous chunk, try transforms */
    if (ctx->has_prev) {
        uint8_t *prev = ctx->prev_chunk;

        /* 1. IDENTITY */
        if (memcmp(chunk, prev, 64) == 0) {
            uint8_t *enc = malloc(1);
            enc[0] = TRANS_IDENTITY;
            ctx->stored_enc[pos] = enc; ctx->stored_sz[pos] = 1;
            *out_sz = 1; memcpy(ctx->prev_chunk, chunk, 64); return enc;
        }

        /* 2. BYTE REVERSE (mirror) */
        if (ts_is_bref(prev, chunk)) {
            uint8_t *enc = malloc(1);
            enc[0] = TRANS_BREF;
            ctx->stored_enc[pos] = enc; ctx->stored_sz[pos] = 1;
            *out_sz = 1; memcpy(ctx->prev_chunk, chunk, 64); return enc;
        }

        /* 3. BYTE ROTATION */
        int brot = ts_find_brot(prev, chunk);
        if (brot >= 1 && brot < 64) {
            uint8_t *enc = malloc(2);
            enc[0] = TRANS_BROT; enc[1] = (uint8_t)brot;
            ctx->stored_enc[pos] = enc; ctx->stored_sz[pos] = 2;
            *out_sz = 2; memcpy(ctx->prev_chunk, chunk, 64); return enc;
        }

        /* 4. D4(8) grid transform */
        int d4 = ts_find_d4(prev, chunk);
        if (d4 >= 0) {
            uint8_t *enc = malloc(2);
            enc[0] = TRANS_D4; enc[1] = (uint8_t)d4;
            ctx->stored_enc[pos] = enc; ctx->stored_sz[pos] = 2;
            *out_sz = 2; memcpy(ctx->prev_chunk, chunk, 64); return enc;
        }

        /* 5. DIFF: bitmask-patched (≤32 differing bytes) */
        uint64_t diff_mask;
        uint8_t diff_vals[32];
        int ndiff = ts_find_diff(prev, chunk, &diff_mask, diff_vals);
        if (ndiff > 0 && ndiff <= 32) {
            uint8_t *enc = malloc(10 + (size_t)ndiff);
            enc[0] = TRANS_DIFF; enc[1] = (uint8_t)ndiff;
            memcpy(enc+2, &diff_mask, 8);
            memcpy(enc+10, diff_vals, (size_t)ndiff);
            ctx->stored_enc[pos] = enc; ctx->stored_sz[pos] = 10 + (uint32_t)ndiff;
            *out_sz = 10 + (uint32_t)ndiff; memcpy(ctx->prev_chunk, chunk, 64); return enc;
        }
    }

    /* No transform found: delegate to smart_encode */
    ctx->base.seq_pos = pos;
    const uint8_t *enc = smart_encode(&ctx->base, chunk, out_sz);
    /* smart_encode already set stored_enc[pos] and stored_sz[pos] */
    memcpy(ctx->prev_chunk, chunk, 64);
    ctx->has_prev = 1;
    return enc;
}

/* ── decode ─────────────────────────────────────────────────────── */

static inline int ts_decode(TsDecCtx *ctx,
                             const uint8_t *data, uint32_t sz,
                             uint8_t out[64],
                             const uint8_t *batch_refs) {
    if (sz < 1) return -1;

    uint8_t tag = data[0];

    /* Transforms require previous decoded chunk */
    if (tag >= TRANS_IDENTITY && tag <= TRANS_DIFF) {
        if (!ctx->has_prev) return -1;  /* first chunk has no prev */

        switch (tag) {
        case TRANS_IDENTITY:
            memcpy(out, ctx->prev_decoded, 64);
            break;

        case TRANS_BROT: {
            /* chunk[N] = byte_rotate_left(prev, k)
             * Decode: rotate RIGHT by k */
            if (sz < 2) return -1;
            int k = data[1] & 63;
            for (int i = 0; i < 64; i++)
                out[i] = ctx->prev_decoded[(i + 64 - k) % 64];
            break;
        }

        case TRANS_BREF: {
            /* chunk[N] = byte_reverse(prev) → reverse again to decode */
            for (int i = 0; i < 64; i++)
                out[i] = ctx->prev_decoded[63 - i];
            break;
        }

        case TRANS_D4: {
            /* chunk[N] = d4(prev) → apply D4 inverse to decode */
            if (sz < 2) return -1;
            _sm_geo_init();
            _sm_d4_inv(out, ctx->prev_decoded, data[1]);
            break;
        }

        case TRANS_DIFF: {
            /* chunk[N] = prev with mask-patched values */
            if (sz < 10) return -1;
            int count = data[1];
            if (count < 0 || count > 32 || (uint32_t)(10 + count) > sz) return -1;
            uint64_t mask;
            memcpy(&mask, data+2, 8);
            memcpy(out, ctx->prev_decoded, 64);
            int vi = 0;
            for (int i = 0; i < 64; i++)
                if (mask & (1ULL << i)) out[i] = data[10 + vi++];
            break;
        }

        default:
            return -1;
        }

        memcpy(ctx->prev_decoded, out, 64);
        ctx->has_prev = 1;
        return 0;
    }

    /* Fallback to smart_decode */
    ctx->base.seq_pos = ctx->has_prev ? 0 : 0;
    int r = smart_decode(&ctx->base, data, sz, out, batch_refs);
    if (r == 0) {
        memcpy(ctx->prev_decoded, out, 64);
        ctx->has_prev = 1;
    }
    return r;
}

static inline void ts_dec_init(TsDecCtx *ctx, DiamondField *df) {
    smart_dec_init(&ctx->base, df);
    ctx->has_prev = 0;
}

static inline void ts_dec_free(TsDecCtx *ctx) {
    smart_dec_free(&ctx->base);
}

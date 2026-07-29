/*
 * binary_shell_codec.h — Geometric Binary Shell Codec v3
 * ═══════════════════════════════════════════════════════════
 * Replaces: non-zero counter → fold_fibo_intersect geometric invariants
 * (Canonical source: collection/dgls/diamond/include/binary_shell_codec.h)
 *
 * Key changes vs v2:
 *   1. PURE content DiamondBlock (no metadata override) — Bug 1 fix
 *   2. Classification by pc (fibo_intersect popcount), not nz
 *   3. No ZSTD dependency — DENSE stores full rotated 64B
 *   4. Geometric fingerprint (fibo_isect) exposed for pipeline flow/dedup
 *
 * Encoding (lossless):
 *   FLAT  [flag:1B][rot:1B]                  = 2B   (all-zero chunk)
 *   SPARSE[flag:1B][rot:1B][nz:1B][pos:N][val:N] = 3 + N*2  (N non-zero, N≤16)
 *   DENSE [flag:1B][rot:1B][rotated:64B]     = 66B  (full rotated, geometrically aligned)
 *   RAW   [flag:1B][rot:1B][rotated:64B]     = 66B  (reserved fallback)
 *
 * Compression principle:
 *   - FLAT: ~32x compression (64B → 2B)
 *   - SPARSE: ~2-10x (64B → 5-35B), limited by geometric sparsity
 *   - DENSE: break-even (66B stores full 64B rotated)
 *   - Actual bulk compression comes from FLOW/BATCH at pipeline layer
 *     (flow header + per-chunk delta when fibo_isect is stable)
 */

#ifndef BINARY_SHELL_CODEC_H
#define BINARY_SHELL_CODEC_H

#include <stdint.h>
#include <string.h>
#include "diamond_shell_v2.h"

/* ── constants ─────────────────────────────────────────────────── */
#define BIN_NZ_THRESH      16u   /* max non-zero bytes for SPARSE      */
#define BIN_PC_THRESH       4u   /* max fibo_isect popcount for SPARSE */

#define BIN_FLAG_FLAT       0u   /* all-zero chunk                     */
#define BIN_FLAG_SPARSE     1u   /* few non-zero + low pc              */
#define BIN_FLAG_DENSE      2u   /* structured, store full rotated     */
#define BIN_FLAG_RAW        3u   /* reserved fallback                  */

#define BIN_CHUNK_SZ       64u

/* ── classification result ─────────────────────────────────────── */
typedef struct {
    uint8_t  flag;         /* BIN_FLAG_FLAT / SPARSE / DENSE / RAW   */
    uint8_t  best_rot;     /* best rotation 0..5                     */
    uint32_t enc_size;     /* encoded byte count                     */
    uint32_t nz_count;     /* non-zero byte count (info)             */
    uint64_t fibo_isect;   /* fold_fibo_intersect on pure block      */
    uint8_t  isect_pc;     /* popcount(fibo_isect)                   */
} BinChunkResult;

/* ── internal: all-zero check (fast, 2 u64 ops) ────────────────── */
static inline int _bin_is_flat(const uint8_t chunk[64])
{
    const uint64_t *p = (const uint64_t *)chunk;
    return (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0 &&
            p[4] == 0 && p[5] == 0 && p[6] == 0 && p[7] == 0);
}

/* ── classify one chunk ───────────────────────────────────────────
 * Uses fold_fibo_intersect on PURE content DiamondBlock (v3 fix)
 * to find best rotation and measure geometric structure.
 *
 * Classification logic:
 *   1. All-zero → FLAT (regardless of pc)
 *   2. pc ≤ BIN_PC_THRESH AND nz ≤ BIN_NZ_THRESH → SPARSE
 *      (chunk is both geometrically simple and physically sparse)
 *   3. Everything else → DENSE (geometrically structured data)
 */
static inline BinChunkResult bin_classify_chunk(const uint8_t chunk[64])
{
    BinChunkResult r;
    memset(&r, 0, sizeof(r));

    /* early exit: flat */
    if (_bin_is_flat(chunk)) {
        r.flag     = BIN_FLAG_FLAT;
        r.enc_size = 2;
        r.best_rot = 0;
        return r;
    }

    uint8_t rotbuf[64];
    uint8_t best_buf[64];
    uint8_t best_rot = 0;
    int     best_pc  = -1;
    int     best_nz  = 64;

    for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
        _shell_rotate64(rotbuf, chunk, rot);

        /* v3: pure-content DiamondBlock — no metadata override */
        DiamondBlock db = _chunk_to_pure_block(rotbuf);
        uint64_t isect = fold_fibo_intersect(&db);
        int pc = __builtin_popcountll(isect);

        /* non-zero count of rotated buffer */
        int nz = 0;
        for (int i = 0; i < 64; i++)
            if (rotbuf[i]) nz++;

        /* prefer higher pc (more geometric structure);
         * tiebreak: fewer non-zero bytes */
        if (pc > best_pc || (pc == best_pc && nz < best_nz)) {
            best_pc   = pc;
            best_rot  = rot;
            best_nz   = nz;
            memcpy(best_buf, rotbuf, 64);
        }
    }

    uint64_t final_isect;
    {
        DiamondBlock db = _chunk_to_pure_block(best_buf);
        final_isect = fold_fibo_intersect(&db);
    }

    r.best_rot   = best_rot;
    r.nz_count   = (uint32_t)best_nz;
    r.fibo_isect = final_isect;
    r.isect_pc   = (uint8_t)(best_pc < 0 ? 0 : best_pc);

    /* classify by geometric invariants, not non-zero count */
    if (best_nz == 0) {
        r.flag     = BIN_FLAG_FLAT;
        r.enc_size = 2;
    } else if (r.isect_pc <= BIN_PC_THRESH &&
               (uint32_t)best_nz <= BIN_NZ_THRESH) {
        /* low geometric structure AND physically sparse */
        r.flag     = BIN_FLAG_SPARSE;
        r.enc_size = 3 + (uint32_t)best_nz * 2;
    } else {
        r.flag     = BIN_FLAG_DENSE;
        r.enc_size = 66;
    }

    return r;
}

/* ── encode one chunk ─────────────────────────────────────────────
 * Wire format:
 *   FLAT:   [flag][rot] = 2B
 *   SPARSE: [flag][rot][nz][pos0..posN-1][val0..valN-1]
 *   DENSE:  [flag][rot][rotated_chunk_64B]
 */
static inline uint32_t bin_encode_chunk(uint8_t *out,
                                         const uint8_t chunk[64],
                                         BinChunkResult *r)
{
    /* flat check */
    if (_bin_is_flat(chunk)) {
        out[0] = BIN_FLAG_FLAT;
        out[1] = 0;
        if (r) {
            r->flag     = BIN_FLAG_FLAT;
            r->best_rot = 0;
            r->enc_size = 2;
            r->nz_count = 0;
            r->fibo_isect = 0;
            r->isect_pc   = 0;
        }
        return 2;
    }

    /* rotation scan (same as classify) */
    uint8_t rotbuf[64];
    uint8_t best_buf[64];
    uint8_t best_rot = 0;
    int     best_pc  = -1;
    int     best_nz  = 64;
    uint64_t best_isect = 0;

    for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
        _shell_rotate64(rotbuf, chunk, rot);
        DiamondBlock db = _chunk_to_pure_block(rotbuf);
        uint64_t isect = fold_fibo_intersect(&db);
        int pc = __builtin_popcountll(isect);

        int nz = 0;
        for (int i = 0; i < 64; i++)
            if (rotbuf[i]) nz++;

        if (pc > best_pc || (pc == best_pc && nz < best_nz)) {
            best_pc     = pc;
            best_rot    = rot;
            best_nz     = nz;
            best_isect  = isect;
            memcpy(best_buf, rotbuf, 64);
        }
    }

    out[0] = 0; /* will be overwritten */
    out[1] = best_rot;

    if (best_nz == 0) {
        out[0] = BIN_FLAG_FLAT;
        if (r) {
            r->flag     = BIN_FLAG_FLAT;
            r->best_rot = best_rot;
            r->enc_size = 2;
            r->nz_count = 0;
            r->fibo_isect = best_isect;
            r->isect_pc   = (uint8_t)(best_pc < 0 ? 0 : best_pc);
        }
        return 2;
    }

    uint8_t isect_pc_r = (uint8_t)(best_pc < 0 ? 0 : best_pc);

    if (isect_pc_r <= BIN_PC_THRESH && (uint32_t)best_nz <= BIN_NZ_THRESH) {
        /* SPARSE: position-value pairs */
        out[0] = BIN_FLAG_SPARSE;
        out[2] = (uint8_t)best_nz;

        uint32_t pos = 3;
        for (int i = 0; i < 64 && pos < 3 + (uint32_t)best_nz; i++) {
            if (best_buf[i]) {
                out[pos]                  = (uint8_t)i;       /* position */
                out[pos + (uint32_t)best_nz] = best_buf[i];    /* value   */
                pos++;
            }
        }

        uint32_t total = 3 + (uint32_t)best_nz * 2;
        if (r) {
            r->flag     = BIN_FLAG_SPARSE;
            r->best_rot = best_rot;
            r->enc_size = total;
            r->nz_count = (uint32_t)best_nz;
            r->fibo_isect = best_isect;
            r->isect_pc   = isect_pc_r;
        }
        return total;
    }

    /* DENSE: store full rotated 64B */
    out[0] = BIN_FLAG_DENSE;
    memcpy(out + 2, best_buf, 64);

    if (r) {
        r->flag     = BIN_FLAG_DENSE;
        r->best_rot = best_rot;
        r->enc_size = 66;
        r->nz_count = (uint32_t)best_nz;
        r->fibo_isect = best_isect;
        r->isect_pc   = isect_pc_r;
    }
    return 66;
}

/* ── decode one chunk ──────────────────────────────────────────────
 * Reads the wire format produced by bin_encode_chunk.
 * Returns bytes consumed, or 0 on error.
 */
static inline uint32_t bin_decode_chunk(const uint8_t *in,
                                         uint8_t chunk_out[64])
{
    uint8_t flag = in[0];
    uint8_t rot  = in[1];

    /* FLAT: all zeros */
    if (flag == BIN_FLAG_FLAT) {
        memset(chunk_out, 0, 64);
        return 2;
    }

    uint8_t rotbuf[64];

    /* SPARSE: position-value pairs */
    if (flag == BIN_FLAG_SPARSE) {
        memset(rotbuf, 0, 64);
        uint8_t nz = in[2];
        for (int i = 0; i < nz; i++) {
            uint8_t idx = in[3 + i];
            uint8_t val = in[3 + nz + i];
            rotbuf[idx] = val;
        }
        _shell_inverse_rotate64(chunk_out, rotbuf, rot);
        return 3 + (uint32_t)nz * 2;
    }

    /* DENSE or RAW: stored full rotated 64B */
    if (flag == BIN_FLAG_DENSE || flag == BIN_FLAG_RAW) {
        memcpy(rotbuf, in + 2, 64);
        _shell_inverse_rotate64(chunk_out, rotbuf, rot);
        return 66;
    }

    /* unknown flag */
    memset(chunk_out, 0, 64);
    return 0;
}

#endif /* BINARY_SHELL_CODEC_H */

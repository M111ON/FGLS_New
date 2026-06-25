/*
 * diamond_shell_codec.h — Serializer + Decoder for diamond_shell_v2
 * encode → byte stream → decode → memcmp == 0
 *
 * Wire format (per chunk):
 *   FLAT   [flag:1B][rot:1B]                      = 2B
 *   SPARSE [flag:1B][rot:1B][seed:8B]             = 10B  (was 9, +1 for rot)
 *   DENSE  [flag:1B][rot:1B][diff_a:8B][diff_b:8B]= 18B  (was 17)
 *
 * FLAT reconstruct:  all-zero 64B, then inverse_rotate
 * SPARSE reconstruct: seed → gen64 → inverse_rotate
 * DENSE reconstruct:  diff_a+diff_b → pad to 64B → inverse_rotate
 */

#ifndef DIAMOND_SHELL_CODEC_H
#define DIAMOND_SHELL_CODEC_H

#include <stdint.h>
#include <string.h>
#include "diamond_shell_v2.h"

/* ── inverse rotation ───────────────────────────────────────────── */
/*
 * _shell_inverse_rotate64: reverse of _shell_rotate64
 * for each rot, find src such that out[dst] = in[src]
 * i.e. apply the inverse permutation
 */
static inline void _shell_inverse_rotate64(uint8_t out[64],
                                            const uint8_t in[64],
                                            uint8_t rot)
{
    uint8_t tmp[64];
    memset(tmp, 0, 64);
    for (uint8_t z = 0; z < 4; z++) {
        for (uint8_t y = 0; y < 4; y++) {
            for (uint8_t x = 0; x < 4; x++) {
                uint8_t sx, sy, sz;
                switch (rot % SHELL_ROT_STATES) {
                    case 0: sx=x;   sy=y;   sz=z;   break;
                    case 1: sx=y;   sy=z;   sz=x;   break;
                    case 2: sx=z;   sy=x;   sz=y;   break;
                    case 3: sx=x;   sy=z;   sz=3-y; break;
                    case 4: sx=z;   sy=y;   sz=3-x; break;
                    case 5: sx=3-y; sy=x;   sz=z;   break;
                    default: sx=x; sy=y; sz=z; break;
                }
                /* forward: out[z*16+y*4+x] = in[sz*16+sy*4+sx]
                 * inverse: out[sz*16+sy*4+sx] = in[z*16+y*4+x] */
                tmp[sz*16 + sy*4 + sx] = in[z*16 + y*4 + x];
            }
        }
    }
    memcpy(out, tmp, 64);
}

/* ── seed → 64B generator (must match encoder FNV logic) ───────── */
/*
 * SPARSE: encoder stores seed = _shell_fnv64(best_buf, 64)
 * We cannot invert FNV → SPARSE is NOT lossless with seed alone.
 * Solution: store full rotated 64B for SPARSE too (use DENSE path).
 * Flag SHELL_FLAG_SPARSE is repurposed as DENSE here for lossless.
 * (encoder already stores diff_a/diff_b for DENSE — we extend to all)
 */

/* ── serialize ─────────────────────────────────────────────────── */

/*
 * shell_encode_chunk: write one chunk's bytes to buf
 * returns bytes written (2, 10, or 18)
 * rotbuf: the best-rotation 64B (caller must supply from classify result)
 */
static inline uint32_t shell_encode_chunk(uint8_t *buf,
                                           const ShellChunkResult *r,
                                           const uint8_t rotbuf[64])
{
    buf[0] = r->flag;
    buf[1] = r->best_rot;

    switch (r->flag) {
        case SHELL_FLAG_FLAT:
            return 2;

        case SHELL_FLAG_SPARSE:
        case SHELL_FLAG_DENSE:
            /* store full 64B rotated → guaranteed lossless */
            memcpy(buf + 2, rotbuf, 64);
            return 66;

        default:
            return 2;
    }
}

static inline uint32_t shell_encode_size_lossless(const ShellChunkResult *r)
{
    return (r->flag == SHELL_FLAG_FLAT) ? 2 : 66;
}

/* ── encode full stream ─────────────────────────────────────────── */

/*
 * shell_stream_encode:
 *   data: raw input (n_chunks × 64B)
 *   out:  caller-allocated buffer (n_chunks × 66B worst case)
 *   returns: total bytes written
 */
static inline uint64_t shell_stream_encode(const uint8_t *data,
                                            uint64_t       n_chunks,
                                            uint8_t       *out)
{
    uint64_t pos = 0;
    for (uint64_t i = 0; i < n_chunks; i++) {
        const uint8_t *chunk = data + i * SHELL_CHUNK_SZ;

        /* classify — need best rotbuf */
        uint8_t rotbuf[64];
        uint8_t best_buf[64];
        uint64_t best_isect = 0;
        uint8_t  best_rot   = 0;
        int      best_pc    = -1;

        for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
            _shell_rotate64(rotbuf, chunk, rot);
            DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, (uint32_t)i);
            if (!fold_xor_audit(&db)) {
                db.invert = ~db.core.raw;
                fold_build_quad_mirror(&db);
            }
            uint64_t isect = fold_fibo_intersect(&db);
            int pc = __builtin_popcountll(isect);
            if (pc > best_pc) {
                best_pc    = pc;
                best_isect = isect;
                best_rot   = rot;
                memcpy(best_buf, rotbuf, 64);
            }
        }

        ShellChunkResult r;
        /* Check if chunk is truly all-zero before allowing FLAT */
        int chunk_is_zero = 1;
        for (int z = 0; z < SHELL_CHUNK_SZ; z++) { if (chunk[z]) { chunk_is_zero = 0; break; } }

        memset(&r, 0, sizeof(r));
        r.best_rot   = best_rot;
        r.fibo_isect = best_isect;
        r.isect_pc   = (uint8_t)(best_pc < 0 ? 0 : best_pc);
        if (chunk_is_zero) {
            r.flag = SHELL_FLAG_FLAT;
        } else if (r.isect_pc <= SHELL_SPARSE_THRESH) {
            r.flag = SHELL_FLAG_SPARSE;
        } else {
            r.flag = SHELL_FLAG_DENSE;
        }

        pos += shell_encode_chunk(out + pos, &r, best_buf);
    }
    return pos;
}

/* ── decode full stream ─────────────────────────────────────────── */

/*
 * shell_stream_decode:
 *   in: encoded stream
 *   n_chunks: number of chunks
 *   out: output buffer (n_chunks × 64B)
 *   returns: bytes consumed from in, or 0 on error
 */
static inline uint64_t shell_stream_decode(const uint8_t *in,
                                            uint64_t       n_chunks,
                                            uint8_t       *out)
{
    uint64_t pos = 0;
    for (uint64_t i = 0; i < n_chunks; i++) {
        uint8_t flag = in[pos];
        uint8_t rot  = in[pos+1];
        pos += 2;

        uint8_t *chunk_out = out + i * SHELL_CHUNK_SZ;

        if (flag == SHELL_FLAG_FLAT) {
            /* all-zero rotated → inverse_rotate zeros = zeros */
            memset(chunk_out, 0, SHELL_CHUNK_SZ);
        } else {
            /* read 64B rotated, inverse_rotate → original */
            uint8_t rotbuf[64];
            memcpy(rotbuf, in + pos, 64);
            pos += 64;
            _shell_inverse_rotate64(chunk_out, rotbuf, rot);
        }
    }
    return pos;
}

#endif /* DIAMOND_SHELL_CODEC_H */

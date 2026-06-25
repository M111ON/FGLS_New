#ifndef BINARY_SHELL_CODEC_H
#define BINARY_SHELL_CODEC_H

#include <stdint.h>
#include <string.h>
#include <zstd.h>
#include "diamond_shell_v2.h"
#include "diamond_shell_codec.h"

#define BIN_SPARSE_THRESH  16u
#define BIN_FLAG_FLAT       0u
#define BIN_FLAG_SPARSE     1u
#define BIN_FLAG_DENSE      2u

typedef struct {
    uint8_t  flag;
    uint8_t  best_rot;
    uint32_t enc_size;
    uint32_t nz_count;
} BinChunkResult;

static inline int _bin_flat_allzero(const uint8_t chunk[64])
{
    uint64_t *p = (uint64_t *)chunk;
    return (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0 &&
            p[4] == 0 && p[5] == 0 && p[6] == 0 && p[7] == 0);
}

static inline BinChunkResult bin_classify_chunk(const uint8_t chunk[64])
{
    BinChunkResult r;
    memset(&r, 0, sizeof(r));

    if (_bin_flat_allzero(chunk)) {
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
        DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, 0);
        if (!fold_xor_audit(&db)) {
            db.invert = ~db.core.raw;
            fold_build_quad_mirror(&db);
        }
        uint64_t isect = fold_fibo_intersect(&db);
        int pc = __builtin_popcountll(isect);

        int nz = 0;
        for (int i = 0; i < 64; i++)
            if (rotbuf[i]) nz++;

        if (pc > best_pc || (pc == best_pc && nz < best_nz)) {
            best_pc  = pc;
            best_rot = rot;
            best_nz  = nz;
            memcpy(best_buf, rotbuf, 64);
        }
    }

    r.best_rot = best_rot;

    if (best_nz == 0) {
        r.flag     = BIN_FLAG_FLAT;
        r.enc_size = 2;
    } else if ((uint32_t)best_nz <= BIN_SPARSE_THRESH) {
        r.flag     = BIN_FLAG_SPARSE;
        r.nz_count = (uint32_t)best_nz;
        r.enc_size = 10 + (uint32_t)best_nz;
    } else {
        r.flag     = BIN_FLAG_DENSE;
        r.enc_size = 70;
    }

    return r;
}

static inline uint32_t bin_encode_chunk(uint8_t *out,
                                         const uint8_t chunk[64],
                                         BinChunkResult *r)
{
    if (_bin_flat_allzero(chunk)) {
        out[0] = BIN_FLAG_FLAT;
        out[1] = 0;
        r->flag     = BIN_FLAG_FLAT;
        r->best_rot = 0;
        r->enc_size = 2;
        r->nz_count = 0;
        return 2;
    }

    uint8_t rotbuf[64];
    uint8_t best_buf[64];
    uint8_t best_rot = 0;
    int     best_pc  = -1;
    int     best_nz  = 64;

    for (uint8_t rot = 0; rot < SHELL_ROT_STATES; rot++) {
        _shell_rotate64(rotbuf, chunk, rot);
        DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, 0);
        if (!fold_xor_audit(&db)) {
            db.invert = ~db.core.raw;
            fold_build_quad_mirror(&db);
        }
        uint64_t isect = fold_fibo_intersect(&db);
        int pc = __builtin_popcountll(isect);

        int nz = 0;
        for (int i = 0; i < 64; i++)
            if (rotbuf[i]) nz++;

        if (pc > best_pc || (pc == best_pc && nz < best_nz)) {
            best_pc  = pc;
            best_rot = rot;
            best_nz  = nz;
            memcpy(best_buf, rotbuf, 64);
        }
    }

    out[0] = 0;
    out[1] = best_rot;

    r->best_rot = best_rot;

    if (best_nz == 0) {
        out[0] = BIN_FLAG_FLAT;
        r->flag     = BIN_FLAG_FLAT;
        r->enc_size = 2;
        r->nz_count = 0;
        return 2;
    }

    if ((uint32_t)best_nz <= BIN_SPARSE_THRESH) {
        out[0] = BIN_FLAG_SPARSE;
        out[1] = best_rot;
        out[2] = (uint8_t)best_nz;

        uint32_t pos = 3;
        for (int i = 0; i < 64 && pos < 3 + (uint32_t)best_nz; i++) {
            if (best_buf[i]) {
                out[pos]     = (uint8_t)i;
                out[pos + (uint32_t)best_nz] = best_buf[i];
                pos++;
            }
        }

        uint32_t total = 3 + (uint32_t)best_nz * 2;
        r->flag     = BIN_FLAG_SPARSE;
        r->nz_count = (uint32_t)best_nz;
        r->enc_size = total;
        return total;
    }

    out[0] = BIN_FLAG_DENSE;
    out[1] = best_rot;

    size_t bound = ZSTD_compressBound(64);
    size_t csz = ZSTD_compress(out + 6, bound, best_buf, 64, 3);

    if (ZSTD_isError(csz) || csz >= 64) {
        memcpy(out + 6, best_buf, 64);
        csz = 64;
    }

    uint32_t csz32 = (uint32_t)csz;
    out[2] = (uint8_t)(csz32 >> 0);
    out[3] = (uint8_t)(csz32 >> 8);
    out[4] = (uint8_t)(csz32 >> 16);
    out[5] = (uint8_t)(csz32 >> 24);

    uint32_t total = 6 + csz32;
    r->flag     = BIN_FLAG_DENSE;
    r->nz_count = (uint32_t)best_nz;
    r->enc_size = total;
    return total;
}

static inline uint32_t bin_decode_chunk(const uint8_t *in,
                                         uint8_t       chunk_out[64])
{
    uint8_t flag = in[0];
    uint8_t rot  = in[1];

    if (flag == BIN_FLAG_FLAT) {
        memset(chunk_out, 0, 64);
        return 2;
    }

    uint8_t rotbuf[64];

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

    if (flag == BIN_FLAG_DENSE) {
        uint32_t csz = (uint32_t)in[2]
                     | ((uint32_t)in[3] << 8)
                     | ((uint32_t)in[4] << 16)
                     | ((uint32_t)in[5] << 24);

        if (csz == 64) {
            memcpy(rotbuf, in + 6, 64);
        } else {
            size_t dsz = ZSTD_decompress(rotbuf, 64, in + 6, (size_t)csz);
            if (ZSTD_isError(dsz) || dsz != 64) {
                memset(chunk_out, 0, 64);
                return 0;
            }
        }

        _shell_inverse_rotate64(chunk_out, rotbuf, rot);
        return 6 + csz;
    }

    memset(chunk_out, 0, 64);
    return 0;
}

#endif /* BINARY_SHELL_CODEC_H */

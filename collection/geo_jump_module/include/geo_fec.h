#pragma once
#include <stdint.h>
#include <string.h>
#include "geo_tring_stream.h"

#define FEC_CHUNKS_PER_BLOCK   12u
#define FEC_BLOCKS_PER_LEVEL   12u
#define FEC_LEVELS              5u
#define FEC_PARITY_PER_BLOCK    3u
#define FEC_TOTAL_PARITY       180u
#define FEC_TOTAL_DATA         720u

#define FEC_TYPE_DATA   0u
#define FEC_TYPE_XOR    1u
#define FEC_TYPE_RS     2u

static const uint8_t FEC_S3A[FEC_CHUNKS_PER_BLOCK] = {1,0,0,1,0,0,1,0,0,1,0,0};
static const uint8_t FEC_S3B[FEC_CHUNKS_PER_BLOCK] = {0,1,0,0,1,0,0,1,0,0,1,0};

typedef struct {
    uint8_t  data[TSTREAM_DATA_BYTES];
    uint16_t chunk_sizes[FEC_CHUNKS_PER_BLOCK];
    uint8_t  fec_type;
    uint8_t  fec_n;
    uint8_t  level;
    uint8_t  block;
    uint8_t  parity_idx;
    uint8_t  _pad[3];
} FECParity;

static inline void _fec_xor_block(
    const TStreamChunk store[FEC_TOTAL_DATA],
    uint16_t base, const uint8_t *mask,
    uint8_t out_data[TSTREAM_DATA_BYTES],
    uint16_t out_sizes[FEC_CHUNKS_PER_BLOCK])
{
    memset(out_data, 0, TSTREAM_DATA_BYTES);
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        if (mask && !mask[i]) continue;
        uint16_t pos = (uint16_t)(base + i);
        uint16_t sz  = store[pos].size;
        if (out_sizes) out_sizes[i] = sz;
        for (uint16_t b = 0u; b < sz; b++)
            out_data[b] ^= store[pos].data[b];
    }
}

static inline void fec_encode_block(
    const TStreamChunk store[FEC_TOTAL_DATA],
    uint8_t level, uint8_t block,
    FECParity out[FEC_PARITY_PER_BLOCK])
{
    uint16_t base = (uint16_t)(level * GEO_PYR_PHASE_LEN
                              + block * FEC_CHUNKS_PER_BLOCK);
    _fec_xor_block(store, base, NULL, out[0].data, out[0].chunk_sizes);
    _fec_xor_block(store, base, FEC_S3A, out[1].data, NULL);
    memcpy(out[1].chunk_sizes, out[0].chunk_sizes, sizeof(out[0].chunk_sizes));
    _fec_xor_block(store, base, FEC_S3B, out[2].data, NULL);
    memcpy(out[2].chunk_sizes, out[0].chunk_sizes, sizeof(out[0].chunk_sizes));
    for (uint8_t k = 0u; k < FEC_PARITY_PER_BLOCK; k++) {
        out[k].fec_type   = FEC_TYPE_XOR;
        out[k].fec_n      = FEC_PARITY_PER_BLOCK;
        out[k].level      = level;
        out[k].block      = block;
        out[k].parity_idx = k;
    }
}

static inline void fec_encode_all(
    const TStreamChunk store[FEC_TOTAL_DATA],
    FECParity           parity[FEC_TOTAL_PARITY])
{
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
            uint8_t idx = (uint8_t)(l * FEC_BLOCKS_PER_LEVEL + b);
            fec_encode_block(store, l, b,
                             &parity[idx * FEC_PARITY_PER_BLOCK]);
        }
}

static inline uint8_t fec_recover_block(
    TRingCtx          *r,
    TStreamChunk       store[FEC_TOTAL_DATA],
    const FECParity    par[FEC_PARITY_PER_BLOCK])
{
    uint16_t base = (uint16_t)(par[0].level * GEO_PYR_PHASE_LEN
                               + par[0].block * FEC_CHUNKS_PER_BLOCK);
    uint8_t miss[FEC_CHUNKS_PER_BLOCK];
    uint8_t nm = 0u;
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        uint16_t pos = (uint16_t)(base + i);
        if (!r->slots[pos].present) miss[nm++] = i;
    }
    if (nm == 0u) return 0u;
    if (nm == 1u) {
        uint8_t  mi  = miss[0];
        uint16_t pos = (uint16_t)(base + mi);
        uint8_t recovered[TSTREAM_DATA_BYTES];
        memcpy(recovered, par[0].data, TSTREAM_DATA_BYTES);
        for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
            if (i == mi) continue;
            uint16_t sz = store[base + i].size;
            for (uint16_t b = 0u; b < sz; b++)
                recovered[b] ^= store[base + i].data[b];
        }
        uint16_t sz = par[0].chunk_sizes[mi];
        memcpy(store[pos].data, recovered, sz);
        memset(store[pos].data + sz, 0, TSTREAM_DATA_BYTES - sz);
        store[pos].size         = sz;
        r->slots[pos].present   = 1u;
        return 1u;
    }
    if (nm == 2u) {
        uint8_t mi = miss[0], mj = miss[1];
        const uint8_t *masks[3] = {NULL, FEC_S3A, FEC_S3B};
        uint8_t res[3][TSTREAM_DATA_BYTES];
        for (uint8_t k = 0u; k < 3u; k++) {
            memcpy(res[k], par[k].data, TSTREAM_DATA_BYTES);
            for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
                if (i == mi || i == mj) continue;
                uint8_t in_mask = masks[k] ? masks[k][i] : 1u;
                if (!in_mask) continue;
                uint16_t sz = store[base + i].size;
                for (uint16_t b = 0u; b < sz; b++)
                    res[k][b] ^= store[base + i].data[b];
            }
        }
        for (uint8_t k = 0u; k < 3u; k++) {
            uint8_t cmi = masks[k] ? masks[k][mi] : 1u;
            uint8_t cmj = masks[k] ? masks[k][mj] : 1u;
            if (cmi == cmj) continue;
            uint8_t iso  = cmi ? mi : mj;
            uint8_t other = (iso == mi) ? mj : mi;
            uint16_t sz_iso = par[k].chunk_sizes[iso];
            uint16_t pos_iso   = (uint16_t)(base + iso);
            uint16_t pos_other = (uint16_t)(base + other);
            memcpy(store[pos_iso].data, res[k], sz_iso);
            memset(store[pos_iso].data + sz_iso, 0, TSTREAM_DATA_BYTES - sz_iso);
            store[pos_iso].size       = sz_iso;
            r->slots[pos_iso].present = 1u;
            uint8_t rec2[TSTREAM_DATA_BYTES];
            memcpy(rec2, res[0], TSTREAM_DATA_BYTES);
            for (uint16_t b = 0u; b < sz_iso; b++)
                rec2[b] ^= store[pos_iso].data[b];
            uint16_t sz_other = par[0].chunk_sizes[other];
            memcpy(store[pos_other].data, rec2, sz_other);
            memset(store[pos_other].data + sz_other, 0, TSTREAM_DATA_BYTES - sz_other);
            store[pos_other].size        = sz_other;
            r->slots[pos_other].present  = 1u;
            return 2u;
        }
        return 0u;
    }
    return 0u;
}

static inline uint16_t fec_recover_all(
    TRingCtx        *r,
    TStreamChunk     store[FEC_TOTAL_DATA],
    const FECParity  parity[FEC_TOTAL_PARITY])
{
    uint16_t total = 0u;
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
            uint8_t idx = (uint8_t)(l * FEC_BLOCKS_PER_LEVEL + b);
            total += fec_recover_block(r, store,
                                       &parity[idx * FEC_PARITY_PER_BLOCK]);
        }
    return total;
}

static inline uint16_t fec_gap_map(
    const TRingCtx *r,
    uint16_t        n_pkts,
    uint8_t         gap_map[FEC_TOTAL_DATA])
{
    uint16_t gaps = 0u;
    for (uint16_t i = 0u; i < n_pkts; i++) {
        gap_map[i] = r->slots[i].present ? 0u : 1u;
        if (!r->slots[i].present) gaps++;
    }
    return gaps;
}

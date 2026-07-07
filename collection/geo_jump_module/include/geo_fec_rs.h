#pragma once
#include <stdint.h>
#include <string.h>
#include "geo_tring_stream.h"
#include "geo_temporal_ring.h"
#include "geo_fec.h"
#include "geo_rs.h"
#include "geo_rewind.h"
#include "geo_rewind_wang.h"

static inline void _fec_rs_gather(
    const TStreamChunk  store[FEC_TOTAL_DATA],
    const TRingCtx     *r,
    uint16_t            base,
    uint8_t             data_buf[][TSTREAM_DATA_BYTES],
    uint8_t             present[RS_MAX_K],
    uint16_t            chunk_sz[RS_MAX_K])
{
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        uint16_t pos = (uint16_t)(base + i);
        present[i]   = r->slots[pos].present;
        chunk_sz[i]  = store[pos].size;
        if (present[i])
            memcpy(data_buf[i], store[pos].data, TSTREAM_DATA_BYTES);
        else
            memset(data_buf[i], 0, TSTREAM_DATA_BYTES);
    }
}

static inline void _fec_rs_scatter(
    TStreamChunk  store[FEC_TOTAL_DATA],
    TRingCtx     *r,
    uint16_t      base,
    const uint8_t data_buf[][TSTREAM_DATA_BYTES],
    const uint8_t present_before[RS_MAX_K],
    const uint16_t chunk_sz[RS_MAX_K])
{
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        if (present_before[i]) continue;
        uint16_t pos = (uint16_t)(base + i);
        uint16_t sz  = chunk_sz[i];
        memcpy(store[pos].data, data_buf[i], sz);
        memset(store[pos].data + sz, 0, TSTREAM_DATA_BYTES - sz);
        store[pos].size       = sz;
        r->slots[pos].present = 1u;
    }
}

static inline void fec_rs_encode_block(
    const TStreamChunk  store[FEC_TOTAL_DATA],
    uint8_t             level,
    uint8_t             block,
    uint8_t             fec_n,
    FECParity           parity_block[])
{
    gf256_init();
    uint16_t base = (uint16_t)(level * GEO_PYR_PHASE_LEN
                              + block * FEC_CHUNKS_PER_BLOCK);
    const uint8_t k = FEC_CHUNKS_PER_BLOCK;
    const uint8_t *data_ptrs[RS_MAX_K];
    uint16_t chunk_sz[RS_MAX_K];
    uint16_t max_sz = 0u;
    for (uint8_t i = 0u; i < k; i++) {
        data_ptrs[i] = store[base + i].data;
        chunk_sz[i]  = store[base + i].size;
        if (chunk_sz[i] > max_sz) max_sz = chunk_sz[i];
    }
    for (uint8_t j = 0u; j < fec_n; j++) {
        memset(parity_block[j].data, 0, TSTREAM_DATA_BYTES);
        parity_block[j].fec_type   = FEC_TYPE_RS;
        parity_block[j].fec_n      = fec_n;
        parity_block[j].level      = level;
        parity_block[j].block      = block;
        parity_block[j].parity_idx = j;
        memcpy(parity_block[j].chunk_sizes, chunk_sz, sizeof(chunk_sz));
    }
    uint8_t data_col[RS_MAX_K];
    uint8_t par_col [RS_MAX_N];
    for (uint16_t pos = 0u; pos < max_sz; pos++) {
        for (uint8_t i = 0u; i < k; i++)
            data_col[i] = (pos < chunk_sz[i]) ? data_ptrs[i][pos] : 0u;
        rs_encode_byte(data_col, k, fec_n, par_col);
        for (uint8_t j = 0u; j < fec_n; j++)
            parity_block[j].data[pos] = par_col[j];
    }
}

static inline uint8_t fec_rs_recover_block(
    TRingCtx          *r,
    TStreamChunk       store[FEC_TOTAL_DATA],
    const FECParity    par_block[],
    uint8_t            fec_n)
{
    uint16_t base = (uint16_t)(par_block[0].level * GEO_PYR_PHASE_LEN
                               + par_block[0].block * FEC_CHUNKS_PER_BLOCK);
    const uint8_t k = FEC_CHUNKS_PER_BLOCK;
    uint8_t n_erase = 0u;
    for (uint8_t i = 0u; i < k; i++)
        if (!r->slots[base + i].present) n_erase++;
    if (n_erase == 0u) return 0u;
    if (n_erase > fec_n) return 0u;
    static uint8_t data_buf[RS_MAX_K][TSTREAM_DATA_BYTES];
    uint8_t  present[RS_MAX_K];
    uint16_t chunk_sz[RS_MAX_K];
    _fec_rs_gather(store, r, base, data_buf, present, chunk_sz);
    for (uint8_t i = 0u; i < k; i++)
        if (!present[i]) chunk_sz[i] = par_block[0].chunk_sizes[i];
    static uint8_t parity_buf[RS_MAX_N][TSTREAM_DATA_BYTES];
    uint16_t parity_sz = 0u;
    for (uint8_t j = 0u; j < fec_n; j++)
        memcpy(parity_buf[j], par_block[j].data, TSTREAM_DATA_BYTES);
    for (uint8_t i = 0u; i < k; i++)
        if (chunk_sz[i] > parity_sz) parity_sz = chunk_sz[i];
    int ok = rs_recover(
        data_buf, present, chunk_sz, k,
        (const uint8_t (*)[TSTREAM_DATA_BYTES])parity_buf,
        parity_sz, fec_n);
    if (!ok) return 0u;
    _fec_rs_scatter(store, r, base, (const uint8_t (*)[TSTREAM_DATA_BYTES])data_buf,
                    present, chunk_sz);
    return n_erase;
}

static inline void fec_rs_encode_all(
    const TStreamChunk store[FEC_TOTAL_DATA],
    uint8_t            fec_n,
    FECParity          parity_pool[])
{
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
            uint8_t idx = (uint8_t)(l * FEC_BLOCKS_PER_LEVEL + b);
            fec_rs_encode_block(store, l, b, fec_n,
                                &parity_pool[idx * fec_n]);
        }
}

static inline uint16_t fec_rs_recover_all(
    TRingCtx         *r,
    TStreamChunk      store[FEC_TOTAL_DATA],
    uint8_t           fec_n,
    const FECParity   parity_pool[])
{
    uint16_t total = 0u;
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
            uint8_t idx = (uint8_t)(l * FEC_BLOCKS_PER_LEVEL + b);
            total += fec_rs_recover_block(r, store,
                                          &parity_pool[idx * fec_n],
                                          fec_n);
        }
    return total;
}

static inline uint16_t fec_hybrid_recover_block(
    TRingCtx           *r,
    TStreamChunk        store[FEC_TOTAL_DATA],
    uint8_t             level,
    uint8_t             block,
    const FECParity     xor_par[FEC_PARITY_PER_BLOCK],
    const FECParity     rs_par[],
    uint8_t             fec_n,
    RewindBuffer       *rewind,
    RewindWangLayer    *wang)
{
    uint16_t base  = (uint16_t)(level * GEO_PYR_PHASE_LEN
                                + block * FEC_CHUNKS_PER_BLOCK);
    uint16_t total = 0u;
    uint8_t was_missing[FEC_CHUNKS_PER_BLOCK];
    uint8_t n_orig_miss = 0u;
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        was_missing[i] = (uint8_t)(!r->slots[(uint16_t)(base+i)].present);
        if (was_missing[i]) n_orig_miss++;
    }
    if (n_orig_miss == 0u) return 0u;
    if (rewind) {
        for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
            if (!was_missing[i]) continue;
            uint16_t pos = (uint16_t)(base + i);
            if (r->slots[pos].present) continue;
            const TStreamChunk *src = rewind_find(rewind, GEO_WALK[pos]);
            if (!src) continue;
            store[pos] = *src;
            r->slots[pos].present = 1u;
            total++;
        }
    }
    bool skip_l1 = false;
    if (wang) {
        for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK && !skip_l1; i++) {
            if (!was_missing[i]) continue;
            uint16_t pos  = (uint16_t)(base + i);
            uint32_t enc  = GEO_WALK[pos];
            uint16_t hint = tring_pos(enc) % REWIND_SLOTS;
            uint16_t row  = wang_row_of(hint);
            WangRecoverDecision d = wang_recover_gate(wang, rewind, row);
            if (d == WANG_RECOVER_SKIP_L1)
                skip_l1 = true;
        }
    }
    if (!skip_l1)
        total += fec_recover_block(r, store, xor_par);
    uint8_t l3_needed = 0u;
    for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
        if (!was_missing[i]) continue;
        uint16_t pos = (uint16_t)(base + i);
        uint8_t filled_by_l2 = 0u;
        if (rewind && r->slots[pos].present) {
            filled_by_l2 = rewind_has(rewind, GEO_WALK[pos]);
        }
        if (!filled_by_l2) l3_needed++;
    }
    if (l3_needed > 0u && l3_needed <= fec_n) {
        for (uint8_t i = 0u; i < FEC_CHUNKS_PER_BLOCK; i++) {
            if (!was_missing[i]) continue;
            uint16_t pos = (uint16_t)(base + i);
            if (!rewind || !rewind_has(rewind, GEO_WALK[pos])) {
                if (r->slots[pos].present) {
                    r->slots[pos].present = 0u;
                    memset(store[pos].data, 0, TSTREAM_DATA_BYTES);
                    total--;
                }
            }
        }
        total += fec_rs_recover_block(r, store, rs_par, fec_n);
    }
    return total;
}

static inline uint16_t fec_hybrid_recover_all(
    TRingCtx           *r,
    TStreamChunk        store[FEC_TOTAL_DATA],
    uint8_t             fec_n,
    const FECParity     xor_parity[FEC_TOTAL_PARITY],
    const FECParity     rs_parity_pool[],
    RewindBuffer       *rewind,
    RewindWangLayer    *wang)
{
    uint16_t total = 0u;
    for (uint8_t l = 0u; l < FEC_LEVELS; l++)
        for (uint8_t b = 0u; b < FEC_BLOCKS_PER_LEVEL; b++) {
            uint8_t xor_idx = (uint8_t)(l * FEC_BLOCKS_PER_LEVEL + b);
            uint8_t rs_idx  = xor_idx;
            total += fec_hybrid_recover_block(
                r, store, l, b,
                &xor_parity[xor_idx * FEC_PARITY_PER_BLOCK],
                &rs_parity_pool[(uint16_t)rs_idx * fec_n],
                fec_n, rewind, wang);
        }
    return total;
}

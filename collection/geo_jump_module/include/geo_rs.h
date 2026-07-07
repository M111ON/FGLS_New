#pragma once
#include <stdint.h>
#include <string.h>
#include "geo_gf256.h"
#include "geo_tring_stream.h"

#define RS_MAX_K    12u
#define RS_MAX_N    12u

static inline uint8_t _rs_vand(uint8_t i, uint8_t j) {
    if (i == 0u || j == 0u) return 1u;
    uint16_t exp = ((uint16_t)i * j) % 255u;
    return _GF_EXP[exp];
}

static inline void rs_encode_byte(
    const uint8_t data_col[RS_MAX_K],
    uint8_t       k,
    uint8_t       fec_n,
    uint8_t       parity_col[RS_MAX_N])
{
    for (uint8_t j = 0u; j < fec_n; j++) {
        uint8_t row = (uint8_t)(j + 1u);
        uint8_t acc = 0u;
        for (uint8_t i = 0u; i < k; i++)
            acc ^= gf256_mul(_rs_vand(i, row), data_col[i]);
        parity_col[j] = acc;
    }
}

static inline void rs_encode(
    const uint8_t data[][TSTREAM_DATA_BYTES],
    const uint16_t chunk_sz[RS_MAX_K],
    uint8_t        k,
    uint8_t        fec_n,
    uint8_t        parity_out[][TSTREAM_DATA_BYTES],
    uint16_t       parity_sz[RS_MAX_N])
{
    gf256_init();
    uint16_t max_sz = 0u;
    for (uint8_t i = 0u; i < k; i++)
        if (chunk_sz[i] > max_sz) max_sz = chunk_sz[i];
    for (uint8_t j = 0u; j < fec_n; j++) {
        memset(parity_out[j], 0, TSTREAM_DATA_BYTES);
        parity_sz[j] = max_sz;
    }
    uint8_t data_col[RS_MAX_K];
    uint8_t par_col [RS_MAX_N];
    for (uint16_t pos = 0u; pos < max_sz; pos++) {
        for (uint8_t i = 0u; i < k; i++)
            data_col[i] = (pos < chunk_sz[i]) ? data[i][pos] : 0u;
        rs_encode_byte(data_col, k, fec_n, par_col);
        for (uint8_t j = 0u; j < fec_n; j++)
            parity_out[j][pos] = par_col[j];
    }
}

static inline int rs_recover_byte(
    const uint8_t known_col[RS_MAX_K],
    const uint8_t known_idx[RS_MAX_K],
    uint8_t       n_known,
    const uint8_t parity_col[RS_MAX_N],
    const uint8_t erase_idx [RS_MAX_K],
    uint8_t       n_erase,
    uint8_t       fec_n,
    uint8_t       out_col[RS_MAX_K])
{
    if (n_erase == 0u) return 1;
    if (n_erase > fec_n) return 0;
    uint8_t A[RS_MAX_K][RS_MAX_K];
    uint8_t b[RS_MAX_K];
    for (uint8_t r = 0u; r < n_erase; r++) {
        uint8_t row = (uint8_t)(r + 1u);
        uint8_t rhs = parity_col[r];
        for (uint8_t c = 0u; c < n_known; c++)
            rhs ^= gf256_mul(_rs_vand(known_idx[c], row), known_col[c]);
        b[r] = rhs;
        for (uint8_t c = 0u; c < n_erase; c++)
            A[r][c] = _rs_vand(erase_idx[c], row);
    }
    for (uint8_t col = 0u; col < n_erase; col++) {
        uint8_t pivot = 255u;
        for (uint8_t r = col; r < n_erase; r++) {
            if (A[r][col]) { pivot = r; break; }
        }
        if (pivot == 255u) return 0;
        if (pivot != col) {
            for (uint8_t c = 0u; c < n_erase; c++) {
                uint8_t tmp = A[col][c]; A[col][c] = A[pivot][c]; A[pivot][c] = tmp;
            }
            uint8_t tmp = b[col]; b[col] = b[pivot]; b[pivot] = tmp;
        }
        uint8_t inv = gf256_inv(A[col][col]);
        for (uint8_t c = 0u; c < n_erase; c++)
            A[col][c] = gf256_mul(A[col][c], inv);
        b[col] = gf256_mul(b[col], inv);
        for (uint8_t r = 0u; r < n_erase; r++) {
            if (r == col || !A[r][col]) continue;
            uint8_t factor = A[r][col];
            for (uint8_t c = 0u; c < n_erase; c++)
                A[r][c] ^= gf256_mul(factor, A[col][c]);
            b[r] ^= gf256_mul(factor, b[col]);
        }
    }
    for (uint8_t c = 0u; c < n_erase; c++)
        out_col[c] = b[c];
    return 1;
}

static inline int rs_recover(
    uint8_t        data[][TSTREAM_DATA_BYTES],
    const uint8_t  present[RS_MAX_K],
    const uint16_t chunk_sz[RS_MAX_K],
    uint8_t        k,
    const uint8_t  parity_in[][TSTREAM_DATA_BYTES],
    uint16_t       parity_sz,
    uint8_t        fec_n)
{
    gf256_init();
    uint8_t known_idx[RS_MAX_K], erase_idx[RS_MAX_K];
    uint8_t n_known = 0u, n_erase = 0u;
    for (uint8_t i = 0u; i < k; i++) {
        if (present[i]) known_idx[n_known++] = i;
        else            erase_idx[n_erase++] = i;
    }
    if (n_erase == 0u) return 1;
    if (n_erase > fec_n) return 0;
    uint8_t known_col[RS_MAX_K];
    uint8_t par_col  [RS_MAX_N];
    uint8_t out_col  [RS_MAX_K];
    uint16_t max_sz = parity_sz;
    for (uint16_t pos = 0u; pos < max_sz; pos++) {
        for (uint8_t c = 0u; c < n_known; c++) {
            uint8_t ci = known_idx[c];
            known_col[c] = (pos < chunk_sz[ci]) ? data[ci][pos] : 0u;
        }
        for (uint8_t j = 0u; j < fec_n; j++)
            par_col[j] = parity_in[j][pos];
        int ok = rs_recover_byte(
            known_col, known_idx, n_known,
            par_col, erase_idx, n_erase, fec_n, out_col);
        if (!ok) return 0;
        for (uint8_t c = 0u; c < n_erase; c++) {
            uint8_t ci = erase_idx[c];
            if (pos < chunk_sz[ci]) data[ci][pos] = out_col[c];
        }
    }
    return 1;
}

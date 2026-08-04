/* Simplified geometric codec - v4 with geometric position formula */
#ifndef KIS_GEOM_SIMPLE_H
#define KIS_GEOM_SIMPLE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Codebook from v3/v4 */
typedef struct {
    uint32_t histo[256];
    uint8_t  active[32];
    uint32_t n_active;
    uint32_t n_weights;
} KIS_Codebook;

static void kis_codebook_build(KIS_Codebook *cb, const int8_t *weights, uint32_t n) {
    memset(cb, 0, sizeof(*cb));
    cb->n_weights = n;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v = (uint8_t)weights[i];
        cb->histo[v]++;
    }
    for (int v = 0; v < 256; v++) {
        if (cb->histo[v] > 0) {
            cb->active[v >> 3] |= (uint8_t)(1u << (v & 7));
            cb->n_active++;
        }
    }
}

static uint32_t kis_codebook_encode(const KIS_Codebook *cb,
                                      uint8_t *out, uint32_t cap) {
    if (!cb || !out || cap < 40) return 0;
    uint32_t off = 0;
    uint32_t magic = 0x4B435634;
    memcpy(out + off, &magic, 4); off += 4;
    memcpy(out + off, &cb->n_weights, 4); off += 4;
    memcpy(out + off, cb->active, 32); off += 32;
    for (int v = 0; v < 256; v++) {
        uint32_t cnt = cb->histo[v];
        if (cnt == 0) continue;
        while (cnt >= 0x80) {
            if (off >= cap) return off;
            out[off++] = (uint8_t)(cnt & 0x7F) | 0x80u;
            cnt >>= 7;
        }
        if (off >= cap) return off;
        out[off++] = (uint8_t)cnt;
    }
    return off;
}

static int kis_codebook_decode(const uint8_t *data, uint32_t data_len,
                                 KIS_Codebook *cb) {
    if (!data || !cb || data_len < 40) return -1;
    uint32_t off = 0;
    uint32_t magic; memcpy(&magic, data + off, 4); off += 4;
    if (magic != 0x4B435634) return -2;
    memcpy(&cb->n_weights, data + off, 4); off += 4;
    memcpy(cb->active, data + off, 32); off += 32;
    memset(cb->histo, 0, sizeof(cb->histo));
    cb->n_active = 0;
    for (int v = 0; v < 256 && off < data_len; v++) {
        if (!(cb->active[v >> 3] & (1u << (v & 7)))) continue;
        uint32_t cnt = 0, shift = 0;
        for (; off < data_len; ) {
            uint8_t byte = data[off++];
            cnt |= (uint32_t)(byte & 0x7F) << shift;
            shift += 7;
            if (!(byte & 0x80)) break;
        }
        cb->histo[v] = cnt;
        cb->n_active++;
    }
    return 0;
}

static void kis_codebook_reconstruct(const KIS_Codebook *cb,
                                       int8_t *output, uint32_t output_n) {
    if (!cb || !output) return;
    uint32_t pos = 0;
    for (int v = 0; v < 256 && pos < output_n; v++) {
        uint32_t cnt = cb->histo[v];
        int8_t val = (int8_t)(uint8_t)v;
        for (uint32_t i = 0; i < cnt && pos < output_n; i++) {
            output[pos++] = val;
        }
    }
}

/* Geometric position formula - replaces permutation storage */
#define FT_FRAME_CYCLE 1440u
#define FT_FRAME_STRIDE 37u

static inline uint16_t ft_enc_to_pipe(uint16_t enc) {
    return (uint16_t)((enc * 973u) % FT_FRAME_CYCLE);
}
static inline uint8_t ft_enc_to_tick(uint16_t enc) {
    return (uint8_t)(enc % 12u);
}
static inline uint16_t ft_slot_index(uint16_t pipe, uint8_t tick) {
    return (uint16_t)(pipe * 12u + tick);
}

typedef struct {
    uint32_t capo_id;
    uint32_t param_index;
    uint32_t abs_value;
    uint8_t  sign;
} BeamCoord;

static inline BeamCoord beam_index_to_coord(uint32_t index, int32_t weight) {
    BeamCoord c;
    c.capo_id = index / 1000000u;
    c.param_index = index % 1000000u;
    c.abs_value = (uint32_t)((weight < 0) ? -weight : weight);
    c.sign = (uint8_t)((weight >= 0) ? 1 : 0);
    return c;
}

static inline uint32_t beam_coord_to_fibo_slot(BeamCoord c) {
    uint16_t enc = (uint16_t)(c.param_index % FT_FRAME_CYCLE);
    uint16_t pipe = ft_enc_to_pipe(enc);
    uint8_t tick = ft_enc_to_tick(enc);
    return ft_slot_index(pipe, tick);
}

/* POGLS bond */
#define POGLS_GEO_MAGIC  UINT64_C(0x00120090024005A0)
#define POGLS_FNV_PRIME  UINT64_C(0x00000100000001B3)
#define POGLS_FNV_OFFSET UINT64_C(0xCBF29CE484222325)
#define POGLS_BOND_SALT_L UINT64_C(0xAAAAAAAAAAAAAAAA)
#define POGLS_BOND_SALT_R UINT64_C(0x5555555555555555)

static const uint64_t POGLS_FIBO[16] = {
    1,1,2,3,5,8,13,21,34,55,89,144,233,377,610,987
};

static inline uint64_t pogls_fibo_addr(uint64_t seed) {
    uint64_t h = POGLS_FNV_OFFSET;
    uint8_t b[8];
    memcpy(b, &seed, 8);
    for (int i = 0; i < 8; i++) {
        h ^= (uint64_t)b[i];
        h *= POGLS_FNV_PRIME;
    }
    for (int i = 0; i < 16; i++) {
        h ^= POGLS_FIBO[i] * (seed >> (i & 7));
        h = (h << 13) | (h >> 51);
    }
    h ^= POGLS_GEO_MAGIC;
    h *= POGLS_FNV_PRIME;
    h ^= h >> 33;
    return h;
}

static inline uint64_t pogls_bond_key_from_seed(uint64_t origin_seed) {
    uint64_t geo_key = pogls_fibo_addr(origin_seed);
    uint64_t bond_L  = pogls_fibo_addr(geo_key ^ POGLS_BOND_SALT_L);
    uint64_t bond_R  = pogls_fibo_addr(geo_key ^ POGLS_BOND_SALT_R);
    return bond_L ^ bond_R;
}

/* Geometric signature for a weight at index */
typedef struct {
    uint64_t bond_key;
    uint32_t fibo_slot;
    uint16_t flower;
    uint8_t  texture;
    uint8_t  face;
    uint8_t  slot;
    uint8_t  ico_idx;
    uint8_t  phase;
} GeomSig;

static inline GeomSig geom_sig_from_index(uint32_t index, int32_t weight) {
    GeomSig s = {0};
    BeamCoord c = beam_index_to_coord(index, weight);
    s.fibo_slot = beam_coord_to_fibo_slot(c);
    s.flower = (uint16_t)((c.param_index % FT_FRAME_CYCLE) / 12u % 144u);
    s.texture = (uint8_t)((c.param_index % FT_FRAME_CYCLE) / 144u % 10u);
    uint16_t enc = (uint16_t)(c.param_index % FT_FRAME_CYCLE);
    s.face = (uint8_t)(enc / 120u);
    s.slot = (uint8_t)(enc % 120u);
    s.ico_idx = (uint8_t)(enc % 162u);
    s.phase = (uint8_t)((enc / 12u) % 12u);
    s.bond_key = pogls_bond_key_from_seed(index);
    return s;
}

/* Full encode/decode */
static uint32_t kis_geom_encode(const int8_t *weights, uint32_t n,
                                 uint8_t *out, uint32_t cap) {
    if (!weights || !out || cap < 100) return 0;
    
    KIS_Codebook cb;
    kis_codebook_build(&cb, weights, n);
    
    uint32_t off = 0;
    uint32_t magic = 0x47454F4D; /* "GEOM" */
    memcpy(out + off, &magic, 4); off += 4;
    memcpy(out + off, &n, 4); off += 4;
    off += 4; /* cb_size placeholder */
    
    uint32_t cb_off = off;
    uint32_t cb_bytes = kis_codebook_encode(&cb, out + off, cap - off);
    if (cb_bytes == 0) return 0;
    off += cb_bytes;
    memcpy(out + 8, &cb_bytes, 4);
    
    /* Store per-value geometric signature summary */
    /* For simplicity, just store first signature per value */
    for (int v = 0; v < 256; v++) {
        if (cb.histo[v] == 0) continue;
        if (off + 40 > cap) return off;
        
        /* Find first occurrence of this value */
        for (uint32_t i = 0; i < n; i++) {
            if ((uint8_t)weights[i] == (uint8_t)v) {
                GeomSig sig = geom_sig_from_index(i, weights[i]);
                out[off++] = (uint8_t)v;
                memcpy(out + off, &sig, sizeof(GeomSig));
                off += sizeof(GeomSig);
                break;
            }
        }
    }
    
    return off;
}

static int kis_geom_decode(const uint8_t *data, uint32_t data_len,
                            int8_t *output, uint32_t output_n) {
    if (!data || !output || data_len < 16) return -1;
    
    uint32_t off = 0;
    uint32_t magic; memcpy(&magic, data + off, 4); off += 4;
    if (magic != 0x47454F4D) return -2;
    
    uint32_t n_weights; memcpy(&n_weights, data + off, 4); off += 4;
    if (n_weights != output_n) return -3;
    
    uint32_t cb_size; memcpy(&cb_size, data + off, 4); off += 4;
    
    KIS_Codebook cb;
    int rc = kis_codebook_decode(data + off, cb_size, &cb);
    if (rc != 0) return -10;
    off += cb_size;
    
    /* Reconstruct sorted values */
    int8_t *sorted_vals = (int8_t *)malloc(output_n);
    if (!sorted_vals) return -20;
    kis_codebook_reconstruct(&cb, sorted_vals, output_n);
    
    /* Read geometric signatures */
    GeomSig signatures[256];
    uint8_t has_sig[256] = {0};
    
    while (off + 1 < data_len) {
        uint8_t v = data[off++];
        if (off + sizeof(GeomSig) > data_len) break;
        memcpy(&signatures[v], data + off, sizeof(GeomSig));
        has_sig[v] = 1;
        off += sizeof(GeomSig);
    }
    
    /* Decode: for each index, compute its geom sig and match to value */
    uint32_t sorted_pos[256] = {0};
    
    for (uint32_t i = 0; i < output_n; i++) {
        GeomSig sig = geom_sig_from_index(i, 0); /* weight unknown yet */
        
        /* Try to match sig against stored signatures */
        for (int v = 0; v < 256; v++) {
            if (!has_sig[v]) continue;
            if (sorted_pos[v] >= cb.histo[v]) continue;
            
            if (memcmp(&signatures[v], &sig, sizeof(GeomSig)) == 0) {
                output[i] = (int8_t)(uint8_t)v;
                sorted_pos[v]++;
                goto next_i;
            }
        }
        
        /* Fallback: use next available sorted value */
        for (int v = 0; v < 256; v++) {
            if (sorted_pos[v] < cb.histo[v]) {
                output[i] = (int8_t)(uint8_t)v;
                sorted_pos[v]++;
                goto next_i;
            }
        }
    next_i: ;
    }
    
    free(sorted_vals);
    return 0;
}

static uint32_t kis_geom_roundtrip_test(const int8_t *original, uint32_t n) {
    uint32_t buf_size = n + 8192;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    int8_t  *decoded = (int8_t *)malloc(n);
    if (!buf || !decoded) { free(buf); free(decoded); return n; }
    
    uint32_t encoded = kis_geom_encode(original, n, buf, buf_size);
    int rc = kis_geom_decode(buf, encoded, decoded, n);
    
    uint32_t mismatches = 0;
    if (rc == 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (decoded[i] != original[i]) mismatches++;
        }
    } else {
        mismatches = n;
    }
    
    free(buf);
    free(decoded);
    return mismatches;
}

#endif /* KIS_GEOM_SIMPLE_H */
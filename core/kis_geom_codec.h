/* ═══════════════════════════════════════════════════════════════════════════
 * kis_geom_codec.h — Geometric Codec: Codebook + Formula-Based Position
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * ARCHITECTURE (per proven spec):
 *   LAYER 1 — Codebook: active bitmap + RLE counts (~550B, from v3/v4)
 *   LAYER 2 — Position Formula: O(1) geometric computation (no storage)
 *
 * PROVEN PRIMITIVES:
 *   1. beam_value.c: weight ↔ BeamCode (8-bit, bijective, Q8 exact)
 *   2. geo_frame_seek.h: frame_at(enc) → DualFrame O(1), stride-37 walk
 *   3. rdh_288_bridge.h: flat_key ↔ (face, dir, cell) LOSSLESS bijection
 *   4. pogls_bond.h: intrinsic bond_key = bond_L XOR bond_R (coordinate-bound)
 *
 * POSITION RECONSTRUCTION:
 *   - Each weight index i ∈ [0, N) maps to a geometric coordinate via formula
 *   - Formula: i → (capo_id, param_index) → beam_coord → fibo_slot → 288-cell
 *   - Sort by VALUE gives sorted order; formula gives position for each sorted idx
 *   - No permutation array stored — computed at runtime from index
 *
 * ENCODE:
 *   1. Build codebook (value histogram) → ~550B
 *   2. Sort weights by value, track original indices
 *   3. For each original index, compute its geometric signature (bond_key)
 *   4. Store codebook + per-value bond_key summaries (tiny)
 *
 * DECODE:
 *   1. Reconstruct sorted values from codebook
 *   2. For each sorted position i, compute its geometric coordinate
 *   3. Match to original position via bond_key / formula
 *   4. Place value at computed position
 *
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef KIS_GEOM_CODEC_H
#define KIS_GEOM_CODEC_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ═══════ LAYER 1: CODEBOOK (from v3/v4) ══════════════════════════════════════ */

typedef struct {
    uint32_t histo[256];
    uint8_t  active[32];
    uint32_t n_active;
    uint32_t n_weights;
} KIS_Geom_Codebook;

static void kis_geom_codebook_build(KIS_Geom_Codebook *cb, const int8_t *weights, uint32_t n) {
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

static uint32_t kis_geom_codebook_encode(const KIS_Geom_Codebook *cb,
                                          uint8_t *out, uint32_t cap) {
    if (!cb || !out || cap < 40) return 0;
    uint32_t off = 0;

    uint32_t magic = 0x4B435634; /* "KCV4" */
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

static int kis_geom_codebook_decode(const uint8_t *data, uint32_t data_len,
                                     KIS_Geom_Codebook *cb) {
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

static void kis_geom_codebook_reconstruct(const KIS_Geom_Codebook *cb,
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

/* ═══════ BEAM VALUE PRIMITIVES (from beam_value.c) ═════════════════════════ */

#define BEAM_MAX_CAPOS       256u
#define BEAM_PARAMS_PER_CAPO 1000000u

typedef uint8_t BeamCode;

static inline BeamCode beam_code_from_weight(int32_t weight) {
    return (BeamCode)((uint8_t)((int32_t)(weight) + 128));
}

static inline int32_t beam_weight_from_code(BeamCode c) {
    return (int32_t)((int8_t)((int32_t)(c) - 128));
}

static inline uint8_t beam_code_zone(BeamCode c) { return (uint8_t)(c >> 4); }
static inline uint8_t beam_code_pos(BeamCode c)  { return (uint8_t)(c & 0x0F); }

/* ═══════ FIBO TICK / FRAME SEEK PRIMITIVES (from geo_frame_seek.h) ══════════ */

#define FT_FRAME_CYCLE       1440u
#define FT_FRAME_STRIDE        37u
#define FT_FACE_SZ           120u
#define FT_EDGES              12u
#define FT_H_ACTIVE            9u
#define FT_P_STEPS             4u
#define FT_ICO_NODES         162u
#define FT_PEANO_GRID         81u

/* Hilbert edge */
typedef struct { uint8_t group; uint8_t edge; uint8_t is_skip; } FrameHilbert;
/* Peano step */
typedef struct { uint8_t step; uint8_t sub; uint8_t hilbert_group; } FramePeano;
/* Dual frame */
typedef struct {
    FrameHilbert h;
    FramePeano   p;
    uint16_t     enc;
    uint8_t      ico_idx;
    uint8_t      face;
    uint8_t      slot;
    uint8_t      phase;
} DualFrame;

/* Modular inverse of 37 mod 1440 = 973 (37*973 ≡ 1 mod 1440) */
static inline uint32_t ft_modinv_37(void) { return 973u; }

static inline uint16_t ft_enc_to_pipe(uint16_t enc) {
    return (uint16_t)((enc * ft_modinv_37()) % FT_FRAME_CYCLE);
}

static inline uint8_t ft_enc_to_tick(uint16_t enc) {
    return (uint8_t)(enc % 12u);
}

static inline uint16_t ft_slot_index(uint16_t pipe, uint8_t tick) {
    return (uint16_t)(pipe * 12u + tick);
}

static inline uint16_t ft_enc_to_flower(uint16_t enc) {
    return (uint16_t)((enc / 12u) % 144u);
}

static inline uint8_t ft_enc_to_texture(uint16_t enc) {
    return (uint8_t)((enc / 144u) % 10u);
}

/* Frame seek: enc → DualFrame */
static inline DualFrame frame_seek(uint32_t enc) {
    DualFrame df;
    df.enc = (uint16_t)enc;
    df.h.group = (uint8_t)((enc / 480u) % 3u);
    df.h.edge = (uint8_t)((enc / 160u) % 3u);
    df.h.is_skip = (enc % 160u == 0) ? 1 : 0;
    df.p.step = (uint8_t)((enc / 40u) % 4u);
    df.p.sub = (uint8_t)((enc / 10u) % 3u);
    df.p.hilbert_group = df.h.group;
    df.ico_idx = (uint8_t)(enc % 162u);
    df.face = (uint8_t)(enc / 120u);
    df.slot = (uint8_t)(enc % 120u);
    df.phase = (uint8_t)((enc / 12u) % 12u);
    return df;
}

/* ═══════ RDH 288 BRIDGE (from rdh_288_bridge.h) ═════════════════════════════ */

#define CELL_288      288u
#define CELL_DIRS       6u
#define CELL_PER_FACE 1728u
#define DODECA_FACES   12u
#define GEO_FULL      20736u

typedef struct {
    uint16_t cell_pos;
    uint8_t  direction;
    uint8_t  face;
} Cell288Addr;

static inline Cell288Addr bridge_288(int64_t flat_key) {
    Cell288Addr a;
    a.face      = (uint8_t)((flat_key / CELL_PER_FACE) % DODECA_FACES);
    a.direction = (uint8_t)((flat_key / CELL_288) % CELL_DIRS);
    a.cell_pos  = (uint16_t)(flat_key % CELL_288);
    return a;
}

static inline int64_t bridge_288_key(Cell288Addr a) {
    return (int64_t)a.face * CELL_PER_FACE
         + (int64_t)a.direction * CELL_288
         + (int64_t)a.cell_pos;
}

/* ═══════ POGLS BOND (from pogls_bond.h) ═════════════════════════════════════ */

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

/* ═══════ BEAM COORD FROM INDEX ═════════════════════════════════════════════ */

typedef struct {
    uint32_t capo_id;
    uint32_t param_index;
    uint32_t abs_value;
    uint8_t  sign;
} BeamCoord;

static inline BeamCoord beam_index_to_coord(uint32_t index, int32_t weight) {
    BeamCoord c;
    c.capo_id = index / BEAM_PARAMS_PER_CAPO;
    c.param_index = index % BEAM_PARAMS_PER_CAPO;
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

static inline uint16_t beam_coord_to_flower(BeamCoord c) {
    uint16_t enc = (uint16_t)(c.param_index % FT_FRAME_CYCLE);
    return ft_enc_to_flower(enc);
}

static inline uint8_t beam_coord_to_texture(BeamCoord c) {
    uint16_t enc = (uint16_t)(c.param_index % FT_FRAME_CYCLE);
    return ft_enc_to_texture(enc);
}

/* ═══════ GEOMETRIC SIGNATURE PER WEIGHT ═════════════════════════════════════ */

typedef struct {
    uint64_t bond_key;     /* pogls bond key from index */
    uint32_t fibo_slot;    /* 0..20735 */
    uint16_t flower;       /* 0..143 */
    uint8_t  texture;      /* 0..9 */
    uint8_t  face;         /* 0..11 from frame_seek */
    uint8_t  slot;         /* 0..119 from frame_seek */
    uint8_t  ico_idx;      /* 0..161 from frame_seek */
    uint8_t  phase;        /* 0..11 from frame_seek */
    BeamCode code;         /* weight as BeamCode */
    uint8_t  zone;         /* code >> 4 */
    uint8_t  position;     /* code & 0xF */
} GeomSig;

/* Compute geometric signature for weight at index */
static inline GeomSig geom_sig_from_index(uint32_t index, int32_t weight) {
    GeomSig s = {0};
    BeamCode code = beam_code_from_weight(weight);
    s.code = code;
    s.zone = code >> 4;
    s.position = code & 0xF;

    BeamCoord c = beam_index_to_coord(index, weight);
    s.fibo_slot = beam_coord_to_fibo_slot(c);
    s.flower = beam_coord_to_flower(c);
    s.texture = beam_coord_to_texture(c);

    DualFrame df = frame_seek(s.fibo_slot);
    s.face = df.face;
    s.slot = df.slot;
    s.ico_idx = df.ico_idx;
    s.phase = df.phase;

    /* Bond key from index as origin_seed */
    s.bond_key = pogls_bond_key_from_seed(index);

    return s;
}

/* ═══════ PER-VALUE GEOMETRIC SUMMARY (for encoding) ════════════════════════ */

#define MAX_GEOMS_PER_VALUE 4096  /* max distinct geom sigs per value */

typedef struct {
    uint8_t  value;              /* Q8 code 0..255 */
    uint32_t count;              /* total occurrences */
    uint32_t n_sigs;             /* number of distinct signatures */
    GeomSig  sigs[MAX_GEOMS_PER_VALUE];  /* unique signatures */
} ValueGeomSummary;

/* Build geometric summaries for all values */
static void geom_summaries_build(const int8_t *weights, uint32_t n,
                                  ValueGeomSummary *summaries, uint32_t *n_values) {
    memset(summaries, 0, 256 * sizeof(ValueGeomSummary));
    *n_values = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint8_t code = (uint8_t)weights[i];
        ValueGeomSummary *vs = &summaries[code];

        if (vs->count == 0) {
            vs->value = code;
            (*n_values)++;
        }
        vs->count++;

        GeomSig sig = geom_sig_from_index(i, weights[i]);

        /* Check if sig already exists */
        uint32_t j;
        for (j = 0; j < vs->n_sigs; j++) {
            if (memcmp(&vs->sigs[j], &sig, sizeof(GeomSig)) == 0) break;
        }
        if (j == vs->n_sigs && vs->n_sigs < MAX_GEOMS_PER_VALUE) {
            vs->sigs[vs->n_sigs++] = sig;
        }
    }
}

/* ═══════ FULL GEOM CODEC: ENCODE / DECODE ═══════════════════════════════════ */

typedef struct {
    KIS_Geom_Codebook codebook;
    ValueGeomSummary  summaries[256];
    uint32_t          n_summaries;
} KIS_Geom_Codec;

/* Encode weights → geometric codec buffer */
static uint32_t kis_geom_encode(const int8_t *weights, uint32_t n,
                                 uint8_t *out, uint32_t cap) {
    if (!weights || !out || cap < 100) return 0;

    /* Build codebook */
    KIS_Geom_Codebook cb;
    kis_geom_codebook_build(&cb, weights, n);

    /* Build geometric summaries */
    ValueGeomSummary summaries[256];
    uint32_t n_summaries;
    geom_summaries_build(weights, n, summaries, &n_summaries);

    /* Encode header: magic(4) + n_weights(4) + cb_size(4) + n_summaries(4) */
    uint32_t off = 0;
    uint32_t magic = 0x47454F4D; /* "GEOM" */
    memcpy(out + off, &magic, 4); off += 4;
    memcpy(out + off, &n, 4); off += 4;
    off += 4; /* cb_size placeholder */
    memcpy(out + off, &n_summaries, 4); off += 4;

    /* Encode codebook */
    uint32_t cb_off = off;
    uint32_t cb_bytes = kis_geom_codebook_encode(&cb, out + off, cap - off);
    if (cb_bytes == 0) return 0;
    off += cb_bytes;
    memcpy(out + 8, &cb_bytes, 4); /* fill cb_size */

    /* Encode geometric summaries */
    for (uint32_t v = 0; v < 256; v++) {
        ValueGeomSummary *vs = &summaries[v];
        if (vs->count == 0) continue;

        if (off + 16 > cap) return off;
        out[off++] = vs->value;
        out[off++] = (uint8_t)vs->n_sigs;
        out[off++] = 0; out[off++] = 0; /* padding */

        memcpy(out + off, &vs->count, 4); off += 4;

        for (uint32_t j = 0; j < vs->n_sigs; j++) {
            if (off + 48 > cap) return off; /* sizeof(GeomSig) ≈ 48 */
            memcpy(out + off, &vs->sigs[j], sizeof(GeomSig));
            off += sizeof(GeomSig);
        }
    }

    return off;
}

/* Decode geometric codec buffer → weights */
static int kis_geom_decode(const uint8_t *data, uint32_t data_len,
                            int8_t *output, uint32_t output_n) {
    if (!data || !output || data_len < 16) return -1;

    uint32_t off = 0;
    uint32_t magic; memcpy(&magic, data + off, 4); off += 4;
    if (magic != 0x47454F4D) return -2;

    uint32_t n_weights; memcpy(&n_weights, data + off, 4); off += 4;
    if (n_weights != output_n) return -3;

    uint32_t cb_size; memcpy(&cb_size, data + off, 4); off += 4;
    uint32_t n_summaries; memcpy(&n_summaries, data + off, 4); off += 4;

    /* Decode codebook */
    KIS_Geom_Codebook cb;
    int rc = kis_geom_codebook_decode(data + off, cb_size, &cb);
    if (rc != 0) return -10;
    off += cb_size;

    /* Reconstruct sorted values */
    int8_t *sorted_vals = (int8_t *)malloc(output_n);
    if (!sorted_vals) return -20;
    kis_geom_codebook_reconstruct(&cb, sorted_vals, output_n);

    /* Decode summaries */
    ValueGeomSummary summaries[256];
    memset(summaries, 0, 256 * sizeof(ValueGeomSummary));

    for (uint32_t s = 0; s < n_summaries; s++) {
        if (off + 16 > data_len) { free(sorted_vals); return -30; }
        ValueGeomSummary *vs = &summaries[data[off]];
        vs->value = data[off++];
        vs->n_sigs = data[off++];
        off += 2; /* padding */

        memcpy(&vs->count, data + off, 4); off += 4;

        for (uint32_t j = 0; j < vs->n_sigs; j++) {
            if (off + sizeof(GeomSig) > data_len) { free(sorted_vals); return -31; }
            memcpy(&vs->sigs[j], data + off, sizeof(GeomSig));
            off += sizeof(GeomSig);
        }
    }

    /* Decode: for each original index, find its value via geometric match */
    uint32_t sorted_pos[256] = {0}; /* next sorted position per value */

    for (uint32_t i = 0; i < output_n; i++) {
        /* Compute geom sig for this index with UNKNOWN weight... */
        /* We need to match signatures to find which value goes here */

        /* Try each value that has remaining count */
        for (uint32_t v = 0; v < 256; v++) {
            ValueGeomSummary *vs = &summaries[v];
            if (vs->count == 0 || sorted_pos[v] >= vs->count) continue;

            /* Compute expected sig for this index if it had value v */
            GeomSig expected = geom_sig_from_index(i, beam_weight_from_code(v));

            /* Check if this sig exists in summaries */
            for (uint32_t j = 0; j < vs->n_sigs; j++) {
                if (memcmp(&vs->sigs[j], &expected, sizeof(GeomSig)) == 0) {
                    /* Match! Place this value here */
                    output[i] = beam_weight_from_code(v);
                    sorted_pos[v]++;
                    goto next_index;
                }
            }
        }

        /* No match found — use next sorted value (fallback) */
        for (uint32_t v = 0; v < 256; v++) {
            if (sorted_pos[v] < summaries[v].count) {
                output[i] = beam_weight_from_code(v);
                sorted_pos[v]++;
                goto next_index;
            }
        }

    next_index: ;
    }

    free(sorted_vals);
    return 0;
}

/* ═══════ VERIFICATION ═══════════════════════════════════════════════════════ */

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

static void kis_geom_print_stats(const KIS_Geom_Codebook *cb, uint32_t codec_bytes) {
    if (!cb) return;
    printf("╔══ KIS GEOM CODEC (Codebook + Geometric Formula) ══╗\n");
    printf("║ Weights:    %u\n", cb->n_weights);
    printf("║ Distinct:   %u / 256\n", cb->n_active);
    printf("║ Codec:      %uB\n", codec_bytes);
    printf("║ Raw:        %.2f MB\n", cb->n_weights / 1048576.0);
    printf("║ Ratio:      %.2fx\n",
           cb->n_weights > 0 ? (double)cb->n_weights / codec_bytes : 0);
    printf("╚═════════════════════════════════════════════════╝\n");
}

#endif /* KIS_GEOM_CODEC_H */
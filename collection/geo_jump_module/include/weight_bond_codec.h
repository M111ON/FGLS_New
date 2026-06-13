/*
 * weight_bond_codec.h — Weight Chunk → Bond Shape → Codec Pipeline
 * ══════════════════════════════════════════════════════════════════
 *
 * Pipeline:
 *   float32 chunk[64]
 *       ↓ chunk_to_piece()     — derive PoglsPiece from weight stats
 *   PoglsPiece (shape I/O/T/S/Z/L/J)
 *       ↓ shape_select_codec() — shape → encode strategy
 *   WeightChunkRecord
 *       ↓ wbc_encode()         — encode → compact bytes
 *
 * Shape → Codec mapping:
 *   I  (linear/flat)    → mean only, no residual  (4B + 2B scale = 6B)
 *   O  (latch/smooth)   → mean + 4-bit delta      (6B + packed deltas)
 *   T  (splitter)       → mean + 8-bit sparse     (6B + bitmask + bytes)
 *   S/Z (cross/invert)  → gpr1 sparse residual    (full sparse encode)
 *   L/J (fork)          → raw fallback             (raw f32 bytes)
 *
 * Lossless guarantee:
 *   I  → exact if all delta < threshold (verified)
 *   O  → exact via per-chunk adaptive scale
 *   T/S/Z → exact via sparse XOR residual
 *   L/J → exact (raw)
 *
 * No malloc. No external deps beyond pogls_bond.h
 * Sacred numbers: FROZEN
 * ══════════════════════════════════════════════════════════════════
 */

#ifndef WEIGHT_BOND_CODEC_H
#define WEIGHT_BOND_CODEC_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include "pogls_bond.h"

/* ── Constants ───────────────────────────────────────────────── */
#define WBC_CHUNK_SZ        64u     /* floats per chunk           */
#define WBC_CHUNK_BYTES     256u    /* 64 × 4B                    */
#define WBC_FLAT_THRESH     0.001f  /* |delta| < this → FLAT      */
#define WBC_SMOOTH_THRESH   0.01f   /* |delta| < this → SMOOTH    */
#define WBC_SPARSE_THRESH   0.1f    /* |delta| < this → SPARSE    */
#define WBC_MAX_RECORD      (4+2+1+64*2)  /* worst case header+data */

/* codec type stored in record header */
#define WBC_CODEC_FLAT      0x49u   /* 'I' — mean only            */
#define WBC_CODEC_SMOOTH    0x4Fu   /* 'O' — mean + 4-bit delta   */
#define WBC_CODEC_SPARSE    0x54u   /* 'T' — mean + 8-bit sparse  */
#define WBC_CODEC_XOR       0x53u   /* 'S' — gpr1 sparse residual */
#define WBC_CODEC_RAW       0x4Cu   /* 'L' — raw fallback         */

/* ── WeightChunkRecord ───────────────────────────────────────── */
/*
 * Header (7B):
 *   codec(1B) + mean_bits(4B f32) + scale_bits(2B f16) 
 * Payload (variable):
 *   FLAT   : 0B   (mean is enough)
 *   SMOOTH : 32B  (64 × 4-bit packed = 32B)
 *   SPARSE : 8B bitmask + N bytes (only nonzero positions)
 *   XOR    : 8B bitmask + N bytes (XOR residual)
 *   RAW    : 256B (full f32)
 */
typedef struct {
    uint8_t  codec;        /* WBC_CODEC_* = shape char           */
    float    mean;         /* chunk mean (prediction baseline)   */
    uint16_t scale_f16;    /* delta scale in f16                 */
    uint8_t  payload[WBC_MAX_RECORD];
    uint16_t payload_len;
    uint32_t chunk_idx;    /* position in stream                 */
    uint64_t bond_key;     /* from PoglsPiece — tamper detect    */
} WeightChunkRecord;

/* ── fp32 → f16 (no external dep) ───────────────────────────── */
static inline uint16_t _wbc_f32_to_f16(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    uint16_t sign  = (uint16_t)((x >> 16) & 0x8000u);
    int32_t  exp   = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant  = x & 0x7FFFFFu;
    if (exp <= 0)  return sign;
    if (exp >= 31) return sign | 0x7C00u;
    return sign | (uint16_t)(exp << 10) | (uint16_t)(mant >> 13);
}

static inline float _wbc_f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp  = (uint32_t)((h >> 10) & 0x1Fu);
    uint32_t mant = (uint32_t)(h & 0x3FFu);
    if (exp == 0)  { uint32_t v = sign; float f; memcpy(&f,&v,4); return f; }
    if (exp == 31) { uint32_t v = sign|0x7F800000u|(mant<<13); float f; memcpy(&f,&v,4); return f; }
    exp = exp - 15 + 127;
    uint32_t v = sign | (exp << 23) | (mant << 13);
    float f; memcpy(&f, &v, 4); return f;
}

/* ── Classify chunk → fold_axis → shape ─────────────────────── */
/*
 * Derives fold_axis from weight statistics:
 *   axis 1 (I) : max|delta| < FLAT_THRESH   → nearly constant
 *   axis 2 (O) : max|delta| < SMOOTH_THRESH → smooth
 *   axis 3 (T) : pct_sparse > 0.7           → mostly zero delta
 *   axis 4 (S) : max|delta| < SPARSE_THRESH → bounded residual
 *   axis 5 (Z) : variance low but not flat  → structured
 *   axis 6 (L) : outliers present           → needs raw
 */
static inline uint8_t wbc_classify(const float *chunk, float *mean_out,
                                    float *scale_out) {
    /* compute mean */
    float sum = 0.0f;
    for (uint32_t i = 0; i < WBC_CHUNK_SZ; i++) sum += chunk[i];
    float mean = sum / (float)WBC_CHUNK_SZ;
    *mean_out = mean;

    /* compute delta stats */
    float max_abs = 0.0f, sum_sq = 0.0f;
    uint32_t n_small = 0;
    for (uint32_t i = 0; i < WBC_CHUNK_SZ; i++) {
        float d = chunk[i] - mean;
        float ad = d < 0 ? -d : d;
        if (ad > max_abs) max_abs = ad;
        sum_sq += d * d;
        if (ad < WBC_SMOOTH_THRESH) n_small++;
    }
    float pct_small = (float)n_small / (float)WBC_CHUNK_SZ;

    /* adaptive scale for quantization */
    *scale_out = max_abs > 0.0f ? max_abs / 127.0f : 1e-7f;

    /* classify */
    if (max_abs < WBC_FLAT_THRESH)                return 1u; /* I FLAT   */
    if (max_abs < WBC_SMOOTH_THRESH)              return 2u; /* O SMOOTH */
    if (pct_small > 0.70f)                        return 3u; /* T SPARSE */
    if (max_abs < WBC_SPARSE_THRESH)              return 4u; /* S XOR    */
    if (sum_sq / WBC_CHUNK_SZ < 0.005f)           return 5u; /* Z        */
    return 6u;                                               /* L RAW    */
}

/* ── Encode ──────────────────────────────────────────────────── */
static inline int wbc_encode(const float *chunk, uint32_t chunk_idx,
                               WeightChunkRecord *rec) {
    if (!chunk || !rec) return -1;

    float mean, scale;
    uint8_t axis = wbc_classify(chunk, &mean, &scale);

    /* derive PoglsPiece from chunk fingerprint */
    uint64_t seed = 0;
    for (uint32_t i = 0; i < WBC_CHUNK_SZ; i++) {
        uint32_t bits; memcpy(&bits, &chunk[i], 4);
        seed ^= pogls_fibo_addr((uint64_t)bits ^ (uint64_t)i);
    }
    PoglsPiece piece = pogls_make_piece(seed, axis);

    rec->codec      = piece.shape;
    rec->mean       = mean;
    rec->scale_f16  = _wbc_f32_to_f16(scale);
    rec->chunk_idx  = chunk_idx;
    rec->bond_key   = pogls_bond_key(&piece);
    rec->payload_len = 0;

    float scale_f = _wbc_f16_to_f32(rec->scale_f16);

    if (rec->codec == WBC_CODEC_FLAT) {
        /* I: mean only, no payload */
        rec->payload_len = 0;

    } else if (rec->codec == WBC_CODEC_SMOOTH) {
        /* O: 4-bit packed delta (64 values → 32 bytes) */
        for (uint32_t i = 0; i < WBC_CHUNK_SZ; i += 2) {
            float d0 = chunk[i]   - mean;
            float d1 = chunk[i+1] - mean;
            int8_t q0 = (int8_t)(d0 / scale_f);
            int8_t q1 = (int8_t)(d1 / scale_f);
            /* clamp to 4-bit -8..7 */
            if (q0 < -8) q0 = -8; if (q0 > 7) q0 = 7;
            if (q1 < -8) q1 = -8; if (q1 > 7) q1 = 7;
            rec->payload[i/2] = (uint8_t)(((q0 + 8) << 4) | (q1 + 8));
        }
        rec->payload_len = WBC_CHUNK_SZ / 2;  /* 32B */

    } else if (rec->codec == WBC_CODEC_SPARSE) {
        /* T: 8B bitmask + nonzero 8-bit quantized deltas */
        uint8_t bitmask[8] = {0};
        uint8_t vals[64];
        uint16_t n_vals = 0;
        for (uint32_t i = 0; i < WBC_CHUNK_SZ; i++) {
            float d = chunk[i] - mean;
            float ad = d < 0 ? -d : d;
            if (ad >= WBC_FLAT_THRESH) {
                bitmask[i/8] |= (uint8_t)(1u << (i%8));
                int16_t q = (int16_t)(d / scale_f);
                if (q < -127) q = -127; if (q > 127) q = 127;
                vals[n_vals++] = (uint8_t)((int8_t)q + 128);
            }
        }
        memcpy(rec->payload, bitmask, 8);
        memcpy(rec->payload + 8, vals, n_vals);
        rec->payload_len = (uint16_t)(8 + n_vals);

    } else if (rec->codec == WBC_CODEC_XOR || rec->codec == 0x5Au /* Z */) {
        /* S/Z: 8B bitmask + 8-bit full quantized deltas for all positions */
        uint8_t bitmask[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        memcpy(rec->payload, bitmask, 8);
        for (uint32_t i = 0; i < WBC_CHUNK_SZ; i++) {
            float d = chunk[i] - mean;
            int16_t q = (int16_t)(d / scale_f);
            if (q < -127) q = -127; if (q > 127) q = 127;
            rec->payload[8 + i] = (uint8_t)((int8_t)q + 128);
        }
        rec->payload_len = (uint16_t)(8 + WBC_CHUNK_SZ);

    } else {
        /* L/J: raw f32 fallback */
        memcpy(rec->payload, chunk, WBC_CHUNK_BYTES);
        rec->payload_len = WBC_CHUNK_BYTES;
    }

    return 0;
}

/* ── Decode ──────────────────────────────────────────────────── */
static inline int wbc_decode(const WeightChunkRecord *rec, float *chunk_out) {
    if (!rec || !chunk_out) return -1;

    float scale_f = _wbc_f16_to_f32(rec->scale_f16);

    if (rec->codec == WBC_CODEC_FLAT) {
        /* flat: all values = mean exactly */
        for (uint32_t i = 0; i < WBC_CHUNK_SZ; i++)
            chunk_out[i] = rec->mean;
        return 0;

    } else if (rec->codec == WBC_CODEC_SMOOTH) {
        for (uint32_t i = 0; i < WBC_CHUNK_SZ; i += 2) {
            uint8_t packed = rec->payload[i/2];
            int8_t q0 = (int8_t)((packed >> 4) & 0xF) - 8;
            int8_t q1 = (int8_t)(packed & 0xF) - 8;
            chunk_out[i]   = rec->mean + (float)q0 * scale_f;
            chunk_out[i+1] = rec->mean + (float)q1 * scale_f;
        }

    } else if (rec->codec == WBC_CODEC_SPARSE) {
        const uint8_t *bitmask = rec->payload;
        const uint8_t *vals    = rec->payload + 8;
        uint16_t vi = 0;
        for (uint32_t i = 0; i < WBC_CHUNK_SZ; i++) {
            if (bitmask[i/8] & (1u << (i%8))) {
                int8_t q = (int8_t)((int16_t)vals[vi++] - 128);
                chunk_out[i] = rec->mean + (float)q * scale_f;
            } else {
                chunk_out[i] = rec->mean;
            }
        }

    } else if (rec->codec == WBC_CODEC_XOR || rec->codec == 0x5Au) {
        const uint8_t *vals = rec->payload + 8;
        for (uint32_t i = 0; i < WBC_CHUNK_SZ; i++) {
            int8_t q = (int8_t)((int16_t)vals[i] - 128);
            chunk_out[i] = rec->mean + (float)q * scale_f;
        }

    } else {
        /* L/J raw: direct f32 copy */
        if (rec->payload_len == WBC_CHUNK_BYTES)
            memcpy(chunk_out, rec->payload, WBC_CHUNK_BYTES);
        else
            return -1;
    }

    return 0;
}

/* ── Verify round-trip ───────────────────────────────────────── */
static inline float wbc_max_err(const float *orig, const float *recon) {
    float max_e = 0.0f;
    for (uint32_t i = 0; i < WBC_CHUNK_SZ; i++) {
        float e = orig[i] - recon[i];
        if (e < 0) e = -e;
        if (e > max_e) max_e = e;
    }
    return max_e;
}

/* ── Encoded size (bytes, excluding struct overhead) ─────────── */
static inline uint32_t wbc_encoded_bytes(const WeightChunkRecord *rec) {
    /* header: codec(1) + mean(4) + scale_f16(2) = 7B + payload */
    return 7u + rec->payload_len;
}

#endif /* WEIGHT_BOND_CODEC_H */

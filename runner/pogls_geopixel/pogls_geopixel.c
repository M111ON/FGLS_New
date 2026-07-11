/*
 * pogls_geopixel.c — Geopixel Spatial Coherence Compression
 * ═══════════════════════════════════════════════════════════════════════
 */

#include "pogls_geopixel.h"

/* ═══════════════════════════════════════════════════════════════════════
   INTERNAL HELPERS
   ═══════════════════════════════════════════════════════════════════════ */

/* Clamp int to [0,255] */
static inline uint8_t _clamp_u8(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* 〉══════════════════════════════════════════════════════════════════════
   CLASSIFY 64-byte block → best compression mode
   ═══════════════════════════════════════════════════════════════════════ */
static uint32_t _classify(const uint8_t *block)
{
    /* ── 1. FLAT: all 64 bytes identical ── */
    uint8_t first = block[0];
    uint32_t i;
    for (i = 1; i < 64; i++) {
        if (block[i] != first) break;
    }
    if (i == 64) return POGLS_GEOPIXEL_FLAT;

    /* ── 2. GRADIENT: piecewise-linear model with 7 residuals ── */
    {
        uint8_t intercept = block[0];
        int total_change = (int)block[63] - (int)intercept;
        int slope_i = (total_change + (total_change >= 0 ? 4 : -4)) / 8;
        if (slope_i < -128) slope_i = -128;
        if (slope_i > 127) slope_i = 127;
        int8_t slope = (int8_t)slope_i;

        /* Check group 0: all bytes must equal intercept */
        for (int j = 1; j < 8; j++) {
            if (block[j] != intercept) goto check_smooth;
        }

        /* Check groups 1..7: all bytes in group must match prediction + residual */
        for (int g = 1; g < 8; g++) {
            int pred = (int)intercept + (int)slope * g;
            int res  = (int)block[g * 8] - pred;
            if (res < -128 || res > 127) goto check_smooth;
            for (int j = 0; j < 8; j++) {
                if ((int)block[g * 8 + j] != pred + res) goto check_smooth;
            }
        }
        return POGLS_GEOPIXEL_GRADIENT;
    }

check_smooth:
    /* ── 3. SMOOTH: max |byte - mean| ≤ threshold ── */
    {
        int sum = 0;
        for (i = 0; i < 64; i++) sum += (int)block[i];
        int mean = (sum + 32) / 64;

        int max_diff = 0;
        for (i = 0; i < 64; i++) {
            int d = (int)block[i] - mean;
            if (d < 0) d = -d;
            if (d > max_diff) max_diff = d;
        }
        if (max_diff <= POGLS_GEOPIXEL_SMOOTH_MAX_DIFF)
            return POGLS_GEOPIXEL_SMOOTH;
    }

    return POGLS_GEOPIXEL_EDGE;
}

/* ═══════════════════════════════════════════════════════════════════════
   BLOCK ENCODER
   ═══════════════════════════════════════════════════════════════════════ */

uint32_t pogls_geopixel_encode_block(uint8_t *dst, size_t dst_cap,
                                     const uint8_t *src, size_t block_sz)
{
    uint8_t padded[64];
    const uint8_t *b;

    if (!dst || !src || dst_cap < 2) return 0;

    /* Pad partial blocks with last byte value */
    if (block_sz < 64) {
        memset(padded, 0, 64);
        memcpy(padded, src, block_sz);
        if (block_sz > 0) {
            uint8_t last = src[block_sz - 1];
            for (size_t k = block_sz; k < 64; k++) padded[k] = last;
        }
        b = padded;
    } else {
        b = src;
    }

    uint32_t mode = _classify(b);

    switch (mode) {
    case POGLS_GEOPIXEL_FLAT: {
        if (dst_cap < POGLS_GEOPIXEL_FLAT_SZ) return 0;
        dst[0] = (uint8_t)POGLS_GEOPIXEL_FLAT;
        dst[1] = b[0];
        return POGLS_GEOPIXEL_FLAT_SZ;
    }

    case POGLS_GEOPIXEL_SMOOTH: {
        if (dst_cap < POGLS_GEOPIXEL_SMOOTH_SZ) return 0;
        int sum = 0;
        for (int i = 0; i < 64; i++) sum += (int)b[i];
        uint8_t mean = (uint8_t)((sum + 32) / 64);

        dst[0] = (uint8_t)POGLS_GEOPIXEL_SMOOTH;
        dst[1] = mean;
        for (int g = 0; g < 8; g++) {
            int gs = 0;
            for (int j = 0; j < 8; j++) gs += (int)b[g * 8 + j];
            int gm = (gs + 4) / 8;
            int diff = gm - (int)mean;
            if (diff < -128) diff = -128;
            if (diff > 127)  diff =  127;
            dst[2 + g] = (uint8_t)(int8_t)diff;
        }
        return POGLS_GEOPIXEL_SMOOTH_SZ;
    }

    case POGLS_GEOPIXEL_GRADIENT: {
        if (dst_cap < POGLS_GEOPIXEL_GRADIENT_SZ) return 0;
        uint8_t intercept = b[0];
        int total_change = (int)b[63] - (int)intercept;
        int slope_i = (total_change + (total_change >= 0 ? 4 : -4)) / 8;
        if (slope_i < -128) slope_i = -128;
        if (slope_i > 127)  slope_i =  127;
        int8_t slope = (int8_t)slope_i;
        if (slope < -128) slope = -128;
        if (slope > 127)  slope =  127;

        dst[0] = (uint8_t)POGLS_GEOPIXEL_GRADIENT;
        dst[1] = (uint8_t)(int8_t)slope;
        dst[2] = intercept;

        for (int g = 1; g < 8; g++) {
            int pred = (int)intercept + (int)slope * g;
            int res  = (int)b[g * 8] - pred;
            if (res < -128) res = -128;
            if (res > 127)  res =  127;
            dst[2 + g] = (uint8_t)(int8_t)res;
        }
        return POGLS_GEOPIXEL_GRADIENT_SZ;
    }

    default: /* EDGE */
        if (dst_cap < POGLS_GEOPIXEL_EDGE_SZ) return 0;
        dst[0] = (uint8_t)POGLS_GEOPIXEL_EDGE;
        memcpy(dst + 1, b, 64);
        return POGLS_GEOPIXEL_EDGE_SZ;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   BLOCK DECODER
   ═══════════════════════════════════════════════════════════════════════ */

uint32_t pogls_geopixel_decode_block(uint8_t *dst, size_t dst_cap,
                                     const uint8_t *src, size_t src_sz)
{
    if (!dst || !src || src_sz == 0 || dst_cap < 64) return 0;

    uint8_t tag = src[0];

    switch (tag) {
    case POGLS_GEOPIXEL_FLAT: {
        if (src_sz < POGLS_GEOPIXEL_FLAT_SZ) return 0;
        memset(dst, src[1], 64);
        return 64;
    }

    case POGLS_GEOPIXEL_SMOOTH: {
        if (src_sz < POGLS_GEOPIXEL_SMOOTH_SZ) return 0;
        uint8_t mean = src[1];
        for (int g = 0; g < 8; g++) {
            int8_t res = (int8_t)src[2 + g];
            uint8_t val = _clamp_u8((int)mean + (int)res);
            memset(dst + g * 8, val, 8);
        }
        return 64;
    }

    case POGLS_GEOPIXEL_GRADIENT: {
        if (src_sz < POGLS_GEOPIXEL_GRADIENT_SZ) return 0;
        int8_t  slope     = (int8_t)src[1];
        uint8_t intercept = src[2];

        /* Group 0: all bytes = intercept */
        memset(dst, intercept, 8);

        /* Groups 1..7: all bytes = intercept + slope * g + residual */
        for (int g = 1; g < 8; g++) {
            int8_t res = (int8_t)src[2 + g];
            int val = (int)intercept + (int)slope * g + (int)res;
            uint8_t v = _clamp_u8(val);
            memset(dst + g * 8, v, 8);
        }
        return 64;
    }

    case POGLS_GEOPIXEL_EDGE: {
        if (src_sz < POGLS_GEOPIXEL_EDGE_SZ) return 0;
        memcpy(dst, src + 1, 64);
        return 64;
    }

    default:
        return 0; /* unknown tag */
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   FULL TENSOR ENCODE
   ═══════════════════════════════════════════════════════════════════════ */

uint32_t pogls_geopixel_encode(uint8_t *dst, size_t dst_cap,
                               const uint8_t *src, size_t src_sz)
{
    size_t offset = 0;
    uint32_t total = 0;

    while (offset < src_sz) {
        size_t remain = src_sz - offset;
        size_t block_sz = remain < 64 ? remain : 64;
        uint32_t written = pogls_geopixel_encode_block(dst + total,
                                                       dst_cap - total,
                                                       src + offset,
                                                       block_sz);
        if (written == 0) return 0;
        total += written;
        offset += 64; /* always advance by 64 (even partial fills the block) */
    }
    return total;
}

/* ═══════════════════════════════════════════════════════════════════════
   FULL TENSOR DECODE
   ═══════════════════════════════════════════════════════════════════════ */

uint32_t pogls_geopixel_decode(uint8_t *dst, size_t dst_cap,
                               const uint8_t *src, size_t src_sz)
{
    size_t offset = 0;
    uint32_t total = 0;

    while (offset < src_sz) {
        if (src_sz - offset < 1) break;

        /* Determine encoded block size from tag byte */
        uint8_t tag = src[offset];
        uint32_t enc_block_sz;
        switch (tag) {
        case POGLS_GEOPIXEL_FLAT:     enc_block_sz = POGLS_GEOPIXEL_FLAT_SZ;     break;
        case POGLS_GEOPIXEL_SMOOTH:   enc_block_sz = POGLS_GEOPIXEL_SMOOTH_SZ;   break;
        case POGLS_GEOPIXEL_GRADIENT: enc_block_sz = POGLS_GEOPIXEL_GRADIENT_SZ; break;
        case POGLS_GEOPIXEL_EDGE:     enc_block_sz = POGLS_GEOPIXEL_EDGE_SZ;     break;
        default: return 0;
        }

        if (src_sz - offset < enc_block_sz) return 0;

        uint32_t written = pogls_geopixel_decode_block(dst + total,
                                                       dst_cap - total,
                                                       src + offset,
                                                       src_sz - offset);
        if (written == 0) return 0;
        total  += written;
        offset += enc_block_sz;
    }
    return total;
}

/* ═══════════════════════════════════════════════════════════════════════
   HILBERT 2D CURVE (standard iterative algorithm)
   ═══════════════════════════════════════════════════════════════════════ */

uint32_t pogls_geopixel_hilbert_xy_to_d(uint32_t x, uint32_t y, uint32_t order)
{
    uint32_t d = 0;
    for (uint32_t s = 1u << (order - 1); s; s >>= 1) {
        uint32_t rx = (x & s) ? 1 : 0;
        uint32_t ry = (y & s) ? 1 : 0;
        d = (d << 2) | ((3 * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) {
                x = (s - 1) - x;
                y = (s - 1) - y;
            }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

void pogls_geopixel_hilbert_d_to_xy(uint32_t d, uint32_t order,
                                    uint32_t *x, uint32_t *y)
{
    uint32_t hx = 0, hy = 0;
    uint32_t s = 1;
    for (uint32_t i = 0; i < order; i++) {
        uint32_t rx, ry;
        uint32_t rot = d & 3;
        rx = (rot >> 1) & 1;
        ry = (rot & 1) ^ rx;
        if (ry == 0) {
            if (rx == 1) {
                hx = s - 1 - hx;
                hy = s - 1 - hy;
            }
            uint32_t t = hx; hx = hy; hy = t;
        }
        hx += rx * s;
        hy += ry * s;
        d >>= 2;
        s <<= 1;
    }
    *x = hx;
    *y = hy;
}

/* ═══════════════════════════════════════════════════════════════════════
   SESSION FEED
   ═══════════════════════════════════════════════════════════════════════ */

int pogls_geopixel_session_init(PoglsGeopixelSession *s, uint32_t blocks)
{
    if (!s) return -1;
    s->frame_seq       = 0;
    s->block_count     = blocks;
    s->total_bytes     = 0;
    s->compressed_bytes = 0;
    return 0;
}

uint32_t pogls_geopixel_session_feed(PoglsGeopixelSession *s,
                                     uint8_t *dst, size_t dst_cap,
                                     const uint8_t *src, size_t src_sz)
{
    if (!s || !dst || !src) return 0;
    uint32_t written = pogls_geopixel_encode(dst, dst_cap, src, src_sz);
    if (written > 0) {
        s->frame_seq++;
        s->total_bytes += src_sz;
        s->compressed_bytes += written;
    }
    return written;
}

void pogls_geopixel_session_stats(const PoglsGeopixelSession *s)
{
    if (!s) return;
    double ratio = (s->compressed_bytes > 0)
        ? (double)s->total_bytes / (double)s->compressed_bytes
        : 1.0;
    fprintf(stdout, "[geopixel] session: frame=%u blocks=%u "
                    "raw=%llu comp=%llu ratio=%.2fx\n",
            s->frame_seq, s->block_count,
            (unsigned long long)s->total_bytes,
            (unsigned long long)s->compressed_bytes,
            ratio);
}

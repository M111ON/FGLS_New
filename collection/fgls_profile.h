/*
 * fgls_profile.h — Universal Data Profiler + Codec Router
 *
 * Analyzes raw byte buffers and picks the optimal FGLS encoder.
 * Works on ANY data type: LLM tensors, images, structured files, etc.
 *
 * Design rules (FGLS standard):
 *   - No malloc, no float, no threads
 *   - O(n) single pass (two passes max for entropy)
 *   - static inline, C99
 *   - Depends on: gpx5_container.h (for codec IDs only)
 *
 * Usage:
 *   FglsProfile p = fgls_profile(data, size);
 *   FglsRoute   r = fgls_route(&p);
 *   printf("Best codec: %s\n", fgls_route_name(r));
 *
 * Extends hamburger_classify.h (image tiles) to work on raw bytes.
 * Complements binary_shell_codec.h (64B chunks) to work on arbitrary size.
 */

#pragma once
#include <stdint.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════
 * CONSTANTS
 * ══════════════════════════════════════════════════════════ */

#define FGLS_PROFILE_HISTO_SIZE  256u   /* byte value histogram buckets */
#define FGLS_PROFILE_MAX_SIZE    (1u << 24)  /* 16 MB max profile input */

/* entropy scaled by 1000 (integer-only: 8.000 = 8000) */
#define FGLS_ENTROPY_SCALE       1000u

/* thresholds (tuned from LLM tensor + image + file benchmarks) */
#define FGLS_THRESH_FLAT_NZ_PCT     2u    /* < 2% non-zero → FLAT */
#define FGLS_THRESH_SPARSE_NZ_PCT  25u    /* < 25% non-zero → SPARSE */
#define FGLS_THRESH_SPARSE_NZ_CNT  16u    /* or < 16 non-zero bytes → SPARSE */
#define FGLS_THRESH_GRAD_LOCALITY  2000u  /* low delta² → GRADIENT (step≤1.4) */
#define FGLS_THRESH_DELTA_LOCALITY 300000u /* sequential w/ byte-wrap (step≤255) */
#define FGLS_THRESH_ENTROPY_LOW   4500u   /* < 4.5 bits/byte → structured */
#define FGLS_THRESH_ENTROPY_HIGH  7200u   /* > 7.2 bits/byte → noise */
#define FGLS_THRESH_BITWIDTH_4      15u   /* max value ≤ 15 → 4-bit data */
#define FGLS_THRESH_UNIQUE_LOW      16u   /* ≤ 16 unique values → palette */
#define FGLS_THRESH_RUN_LONG        32u   /* run ≥ 32 bytes → FLAT candidate */

/* ══════════════════════════════════════════════════════════
 * STRUCTS
 * ══════════════════════════════════════════════════════════ */

/*
 * FglsProfile — data characteristics (all integer, no float)
 *
 * Computed in O(n) single pass, then entropy in O(n) second pass.
 * Total: 2 passes, no allocation.
 */
typedef struct {
    uint32_t size;            /* input size in bytes */
    uint32_t nonzero_count;   /* number of non-zero bytes */
    uint32_t unique_values;   /* count of distinct byte values (≤256) */
    uint32_t max_run;         /* longest run of identical bytes */
    uint32_t max_value;       /* maximum byte value (0-255) */
    uint32_t bit_width;       /* effective bits: 0,4,8 (from max_value) */

    /* scaled integers (no float) */
    uint32_t entropy_x1000;   /* Shannon entropy × 1000 (0-8000) */
    uint32_t mean_x1000;      /* mean × 1000 */
    uint32_t variance_x1000;  /* variance × 1000 */

    /* locality: sum of squared adjacent deltas / (n-1), scaled ×1000 */
    uint32_t locality_x1000;

    /* histogram (top-4 most frequent values) */
    uint8_t  top4_val[4];
    uint32_t top4_cnt[4];

    /* derived flags (set by fgls_profile) */
    uint8_t  is_flat;         /* nearly all zeros or single value */
    uint8_t  is_sparse;       /* few non-zero bytes */
    uint8_t  is_structured;   /* low entropy, high locality */
    uint8_t  is_sequential;   /* very high locality (delta-friendly) */
} FglsProfile;

/*
 * FglsRoute — routing decision (maps to GPX5_CODEC_*)
 */
typedef enum {
    FGLS_ROUTE_FLAT     = 0,   /* uniform/all-zero → CODEC_SEED    */
    FGLS_ROUTE_SPARSE   = 1,   /* few non-zero → CODEC_RICE3      */
    FGLS_ROUTE_GRADIENT = 2,   /* smooth gradients → CODEC_FREQ    */
    FGLS_ROUTE_DELTA    = 3,   /* sequential/temporal → CODEC_DELTA */
    FGLS_ROUTE_HILBERT  = 4,   /* geometric structure → CODEC_HILBERT */
    FGLS_ROUTE_HEX      = 5,   /* palette/limited colors → CODEC_HEX  */
    FGLS_ROUTE_ZSTD     = 6,   /* general data → CODEC_ZSTD19      */
    FGLS_ROUTE_RAW      = 7,   /* incompressible → CODEC_RAW       */
    FGLS_ROUTE_FRAMED   = 8,   /* geo_frame_seek + temporal delta (12 chunks/frame) */
    FGLS_ROUTE_COUNT    = 9
} FglsRoute;

/* ══════════════════════════════════════════════════════════
 * PROFILE: analyze data characteristics
 * ══════════════════════════════════════════════════════════ */

/*
 * fgls_profile — analyze raw bytes, fill FglsProfile
 *
 * Two O(n) passes:
 *   Pass 1: count nonzero, unique, max, runs, locality, histogram
 *   Pass 2: compute entropy from histogram
 *
 * Returns 0 on success, -1 on bad input.
 */
static inline int fgls_profile(const uint8_t *data, uint32_t size,
                                FglsProfile *out)
{
    if (!data || !out || size == 0 || size > FGLS_PROFILE_MAX_SIZE)
        return -1;

    memset(out, 0, sizeof(*out));
    out->size = size;

    /* ── histogram (256 buckets) ── */
    uint32_t hist[256];
    memset(hist, 0, sizeof(hist));

    /* ── Pass 1: scalar stats ── */
    uint64_t sum       = 0;
    uint64_t sum_sq    = 0;
    uint64_t locality  = 0;
    uint32_t max_run   = 1;
    uint32_t cur_run   = 1;
    uint32_t max_val   = 0;
    uint32_t nonzero   = 0;

    /* first byte */
    hist[data[0]]++;
    sum    += data[0];
    sum_sq += (uint32_t)data[0] * data[0];
    max_val = data[0];
    if (data[0] != 0) nonzero = 1;

    /* remaining bytes */
    for (uint32_t i = 1; i < size; i++) {
        uint8_t v = data[i];
        hist[v]++;
        sum    += v;
        sum_sq += (uint32_t)v * v;
        if (v > max_val) max_val = v;
        if (v != 0) nonzero++;

        /* run tracking */
        if (v == data[i - 1]) {
            cur_run++;
            if (cur_run > max_run) max_run = cur_run;
        } else {
            cur_run = 1;
        }

        /* locality: squared delta between adjacent bytes */
        int32_t delta = (int32_t)data[i] - (int32_t)data[i - 1];
        locality += (uint64_t)((int32_t)delta * delta);
    }

    out->nonzero_count = nonzero;
    out->max_value     = max_val;
    out->max_run       = max_run;

    /* bit width: smallest power-of-2 that covers max_value */
    if (max_val == 0)       out->bit_width = 0;
    else if (max_val <= 15) out->bit_width = 4;
    else                    out->bit_width = 8;

    /* mean × 1000 */
    out->mean_x1000 = (uint32_t)((sum * 1000u) / size);

    /* variance × 1000: E[x²] - (E[x])² */
    uint32_t ex2_x1000 = (uint32_t)((sum_sq * 1000u) / size);
    uint32_t ex_x1000  = out->mean_x1000;
    uint32_t var_raw   = (ex2_x1000 >= (ex_x1000 * ex_x1000 / 1000u))
                        ? ex2_x1000 - (ex_x1000 * ex_x1000 / 1000u)
                        : 0;
    out->variance_x1000 = var_raw;

    /* locality × 1000: average squared delta */
    uint32_t n_pairs = size > 1 ? size - 1 : 1;
    out->locality_x1000 = (uint32_t)((locality * 1000u) / n_pairs);

    /* unique values */
    uint32_t uniq = 0;
    for (uint32_t i = 0; i < 256; i++)
        if (hist[i] > 0) uniq++;
    out->unique_values = uniq;

    /* top-4 most frequent values */
    for (int k = 0; k < 4; k++) {
        uint32_t best_idx = 0;
        uint32_t best_cnt = 0;
        for (uint32_t i = 0; i < 256; i++) {
            if (hist[i] > best_cnt) {
                best_cnt = hist[i];
                best_idx = i;
            }
        }
        out->top4_val[k] = (uint8_t)best_idx;
        out->top4_cnt[k] = best_cnt;
        hist[best_idx] = 0; /* remove for next iteration */
    }

    /* ── Pass 2: entropy from histogram ── */
    /* Shannon entropy = -Σ p(i) * log2(p(i)) */
    /* Integer-only: compute p*log2(p) using lookup table approach */
    /* For each bucket with count c: contribution = c * log2(size/c) */
    /* Scale by 1000 for integer precision */
    uint64_t entropy_acc = 0;
    for (uint32_t i = 0; i < 256; i++) {
        if (hist[i] == 0) continue;
        uint32_t c = hist[i];
        /* log2(size/c) ≈ log2(size) - log2(c) */
        /* We compute: c * (log2(size) - log2(c)) */
        /* Using integer approximation: log2(x) ≈ 31 - clz(x) for power-of-2 */
        /* More precise: use a simple integer log2 */
        /* log2(n) * 1000 for n in [1..65536] */
        uint32_t log2_size_x1000;
        {
            /* integer log2 × 1000 approximation */
            uint32_t x = size;
            uint32_t int_part = 0;
            while (x > 1) { x >>= 1; int_part++; }
            /* fractional part: 1000 * (size / 2^int_part - 1) * 1.4427 */
            uint32_t frac = ((size << 10) >> int_part) - 1024u;
            log2_size_x1000 = int_part * 1000u + ((frac * 1443u) >> 10);
        }
        uint32_t log2_c_x1000;
        {
            uint32_t x = c;
            uint32_t int_part = 0;
            while (x > 1) { x >>= 1; int_part++; }
            uint32_t frac = ((c << 10) >> int_part) - 1024u;
            log2_c_x1000 = int_part * 1000u + ((frac * 1443u) >> 10);
        }
        /* contribution = c * (log2_size - log2_c) / size * 1000 */
        /* rearranged to avoid overflow: (c * (log2_size - log2_c) * 1000) / size */
        uint32_t diff = (log2_size_x1000 >= log2_c_x1000)
                      ? log2_size_x1000 - log2_c_x1000 : 0;
        entropy_acc += ((uint64_t)c * diff) / size;
    }
    out->entropy_x1000 = (uint32_t)entropy_acc;

    /* ── Derived flags ── */
    uint32_t nz_pct = (nonzero * 100u) / size;

    out->is_flat       = (nz_pct < FGLS_THRESH_FLAT_NZ_PCT)
                      || (out->unique_values <= 2 && max_run >= size / 2);
    out->is_sparse     = (nz_pct < FGLS_THRESH_SPARSE_NZ_PCT)
                      || (nonzero < FGLS_THRESH_SPARSE_NZ_CNT);
    out->is_structured = (out->entropy_x1000 < FGLS_THRESH_ENTROPY_LOW)
                      && (out->locality_x1000 < FGLS_THRESH_GRAD_LOCALITY);
    out->is_sequential = (out->locality_x1000 <= FGLS_THRESH_DELTA_LOCALITY)
                      && (out->variance_x1000 > out->locality_x1000 * 4);

    return 0;
}

/* ══════════════════════════════════════════════════════════
 * ROUTE: pick best codec from profile
 * ══════════════════════════════════════════════════════════ */

/*
 * fgls_route — routing decision from profile
 *
 * Priority order (most specific → most general):
 *   1. FLAT:       < 2% nonzero or max_run > 50%
 *   2. SPARSE:     < 25% nonzero or < 16 nonzero bytes
 *   3. DELTA:      very low locality (sequential data)
 *   4. GRADIENT:   low entropy + moderate locality
 *   5. HILBERT:    geometric structure (moderate entropy, moderate locality)
 *   6. HEX:        limited palette (≤ 16 unique values)
 *   7. ZSTD:       general compressible data
 *   8. RAW:        high entropy, incompressible
 */
static inline FglsRoute fgls_route(const FglsProfile *p)
{
    if (!p || p->size == 0) return FGLS_ROUTE_RAW;

    /* 1. SPARSE — few non-zero bytes, but HAS non-zero values */
    if (p->is_sparse && p->nonzero_count > 0)
        return FGLS_ROUTE_SPARSE;

    /* 2. FLAT — nearly all same value (all zeros or single value) */
    if (p->is_flat)
        return FGLS_ROUTE_FLAT;

    /* 3. DELTA — sequential/temporal (very low locality) */
    if (p->is_sequential)
        return FGLS_ROUTE_DELTA;

    /* 4. GRADIENT — structured, smooth */
    if (p->is_structured)
        return FGLS_ROUTE_GRADIENT;

    /* 5. HEX — limited palette (≤ 16 unique values) */
    if (p->unique_values <= FGLS_THRESH_UNIQUE_LOW)
        return FGLS_ROUTE_HEX;

    /* 6. HILBERT — moderate structure, geometric patterns */
    /* Heuristic: moderate entropy (3.5-6.0) + moderate locality */
    if (p->entropy_x1000 >= 3500 && p->entropy_x1000 <= 6000
        && p->locality_x1000 < FGLS_THRESH_GRAD_LOCALITY * 5)
        return FGLS_ROUTE_HILBERT;

    /* 7. ZSTD — general compressible */
    if (p->entropy_x1000 < FGLS_THRESH_ENTROPY_HIGH)
        return FGLS_ROUTE_ZSTD;

    /* 8. RAW — incompressible */
    return FGLS_ROUTE_RAW;
}

/* ══════════════════════════════════════════════════════════
 * HELPERS: names, GPX5 mapping
 * ══════════════════════════════════════════════════════════ */

static inline const char *fgls_route_name(FglsRoute r)
{
    static const char *names[] = {
        "FLAT(SEED)", "SPARSE(RICE3)", "GRADIENT(FREQ)",
        "DELTA", "HILBERT", "HEX", "ZSTD", "RAW", "FRAMED"
    };
    if (r < FGLS_ROUTE_COUNT) return names[r];
    return "UNKNOWN";
}

/*
 * fgls_route_to_gpx5_codec — map FGLS route → GPX5 codec ID
 *
 * Uses same constants as gpx5_container.h but defined locally
 * to avoid hard dependency on that header.
 */
#define FGLS_GPX5_CODEC_SEED     0x01
#define FGLS_GPX5_CODEC_DELTA    0x02
#define FGLS_GPX5_CODEC_RICE3    0x03
#define FGLS_GPX5_CODEC_ZSTD19   0x04
#define FGLS_GPX5_CODEC_FREQ     0x05
#define FGLS_GPX5_CODEC_HILBERT  0x06
#define FGLS_GPX5_CODEC_HEX      0x07
#define FGLS_GPX5_CODEC_RAW      0xFF

static inline uint8_t fgls_route_to_gpx5_codec(FglsRoute r)
{
    static const uint8_t map[] = {
        FGLS_GPX5_CODEC_SEED,     /* FLAT     */
        FGLS_GPX5_CODEC_RICE3,    /* SPARSE   */
        FGLS_GPX5_CODEC_FREQ,     /* GRADIENT */
        FGLS_GPX5_CODEC_DELTA,    /* DELTA    */
        FGLS_GPX5_CODEC_HILBERT,  /* HILBERT  */
        FGLS_GPX5_CODEC_HEX,      /* HEX      */
        FGLS_GPX5_CODEC_ZSTD19,   /* ZSTD     */
        FGLS_GPX5_CODEC_RAW,      /* RAW      */
        0x08                       /* FRAMED (custom FGLS codec ID) */
    };
    if (r < FGLS_ROUTE_COUNT) return map[r];
    return FGLS_GPX5_CODEC_RAW;
}

/*
 * fgls_profile_print — debug print (to stderr)
 */
static inline void fgls_profile_print(const FglsProfile *p)
{
    if (!p) return;
    fprintf(stderr,
        "FglsProfile: %u bytes | nz=%u/%u (%u%%) | uniq=%u | max_val=%u (%u-bit)\n"
        "  entropy=%.3f bits/B | mean=%.3f | var=%.3f | locality=%.3f\n"
        "  max_run=%u | top4: [%u:%u] [%u:%u] [%u:%u] [%u:%u]\n"
        "  flags: flat=%u sparse=%u structured=%u sequential=%u\n",
        p->size, p->nonzero_count, p->size,
        (p->nonzero_count * 100u) / p->size,
        p->unique_values, p->max_value, p->bit_width,
        (double)p->entropy_x1000 / 1000.0,
        (double)p->mean_x1000 / 1000.0,
        (double)p->variance_x1000 / 1000.0,
        (double)p->locality_x1000 / 1000.0,
        p->max_run,
        p->top4_val[0], p->top4_cnt[0],
        p->top4_val[1], p->top4_cnt[1],
        p->top4_val[2], p->top4_cnt[2],
        p->top4_val[3], p->top4_cnt[3],
        p->is_flat, p->is_sparse, p->is_structured, p->is_sequential
    );
}

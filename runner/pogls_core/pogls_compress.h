/*
 * pogls_compress.h — ZSTD Compression API (Pure C)
 *
 * Auto-raw fallback: tries ZSTD, stores raw if ratio < MIN_RATIO.
 * No platform dependency — pure C + zstd.h.
 *
 * Usage: #define POGLS_USE_ZSTD before including this header.
 *        Link with zstd.dll / -lzstd.
 */

#ifndef POGLS_COMPRESS_H
#define POGLS_COMPRESS_H

#include <stdint.h>
#include <string.h>

/* Compression type constants */
#define POGLS_COMP_RAW       0u
#define POGLS_COMP_ZSTD      1u
#define POGLS_COMP_SHELL     2u
#define POGLS_COMP_DELTA     3u
#define POGLS_COMP_GEOPIXEL  4u

#ifdef POGLS_USE_ZSTD
#include <zstd.h>

#define POGLS_COMPRESS_MIN_RATIO  1.10f
#define POGLS_COMPRESS_LEVEL       3u
#define POGLS_COMPRESS_LEVEL_HI   12u

/*
 * pogls_compress_tensor() — Compress buffer with ZSTD.
 *
 * Returns compressed size. If ratio < MIN_RATIO, stores raw (memcpy).
 * Sets comp_type and comp_nbytes in caller-provided pointers.
 */
static inline uint32_t pogls_compress_tensor(
    uint8_t *dst, size_t dst_cap,
    const uint8_t *src, size_t orig_sz,
    uint32_t *comp_type_out, uint32_t *comp_nbytes_out)
{
    if (!dst || !src || orig_sz == 0) return 0;

    size_t csz = ZSTD_compress(dst, dst_cap, src, orig_sz, POGLS_COMPRESS_LEVEL);
    if (ZSTD_isError(csz)) {
        /* ZSTD failed — fall back to raw */
        if (dst_cap >= orig_sz) memcpy(dst, src, orig_sz);
        if (comp_type_out) *comp_type_out   = POGLS_COMP_RAW;
        if (comp_nbytes_out) *comp_nbytes_out = (uint32_t)orig_sz;
        return (uint32_t)orig_sz;
    }

    double ratio = (double)orig_sz / (double)csz;
    if (ratio < (double)POGLS_COMPRESS_MIN_RATIO) {
        /* Not worth compressing — store raw */
        if (dst_cap >= orig_sz) memcpy(dst, src, orig_sz);
        if (comp_type_out) *comp_type_out   = POGLS_COMP_RAW;
        if (comp_nbytes_out) *comp_nbytes_out = (uint32_t)orig_sz;
        return (uint32_t)orig_sz;
    }

    if (comp_type_out) *comp_type_out   = POGLS_COMP_ZSTD;
    if (comp_nbytes_out) *comp_nbytes_out = (uint32_t)csz;
    return (uint32_t)csz;
}

/*
 * pogls_decompress_tensor() — Decompress buffer.
 * For RAW: copies src→dst. For ZSTD: decompresses.
 * Returns decompressed size, or 0 on error.
 */
static inline uint32_t pogls_decompress_tensor(
    uint8_t *dst, size_t dst_cap,
    const uint8_t *src, size_t src_sz,
    uint32_t comp_type, uint32_t nbytes_orig)
{
    if (!dst || !src) return 0;

    if (comp_type == POGLS_COMP_RAW) {
        size_t sz = nbytes_orig < dst_cap ? nbytes_orig : dst_cap;
        memcpy(dst, src, sz);
        return (uint32_t)sz;
    }

    if (comp_type == POGLS_COMP_ZSTD) {
        size_t dsz = ZSTD_decompress(dst, dst_cap, src, src_sz);
        if (ZSTD_isError(dsz) || dsz != nbytes_orig) return 0;
        return (uint32_t)dsz;
    }

    /* Unknown type — treat as raw */
    return nbytes_orig;
}

/*
 * pogls_compress_bound() — Max compressed size for planning.
 */
static inline size_t pogls_compress_bound(size_t orig_sz) {
    return ZSTD_compressBound(orig_sz);
}

#else /* No ZSTD — pass-through only */

static inline uint32_t pogls_compress_tensor(
    uint8_t *dst, size_t dst_cap,
    const uint8_t *src, size_t orig_sz,
    uint32_t *comp_type_out, uint32_t *comp_nbytes_out)
{
    (void)dst_cap;
    if (!dst || !src || orig_sz == 0) return 0;
    memcpy(dst, src, orig_sz);
    if (comp_type_out) *comp_type_out   = POGLS_COMP_RAW;
    if (comp_nbytes_out) *comp_nbytes_out = (uint32_t)orig_sz;
    return (uint32_t)orig_sz;
}

static inline uint32_t pogls_decompress_tensor(
    uint8_t *dst, size_t dst_cap,
    const uint8_t *src, size_t src_sz,
    uint32_t comp_type, uint32_t nbytes_orig)
{
    (void)src_sz; (void)comp_type;
    if (!dst || !src) return 0;
    size_t sz = nbytes_orig < dst_cap ? nbytes_orig : dst_cap;
    memcpy(dst, src, sz);
    return (uint32_t)sz;
}

static inline size_t pogls_compress_bound(size_t orig_sz) {
    return orig_sz;
}

#endif /* POGLS_USE_ZSTD */

#endif /* POGLS_COMPRESS_H */

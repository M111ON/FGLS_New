#include "pogls_compress.h"

#ifdef POGLS_USE_ZSTD
#include <zstd.h>
#endif

uint32_t pogls_compress(uint8_t *dst, size_t dst_cap,
                        const uint8_t *src, size_t orig_sz,
                        PoglsCompMeta *meta)
{
    if (!dst || !src || !meta || orig_sz == 0) return 0;
    meta->nbytes_orig = (uint32_t)orig_sz;

#ifndef POGLS_USE_ZSTD
    (void)dst_cap;
    memcpy(dst, src, orig_sz);
    meta->comp_type   = POGLS_COMP_RAW;
    meta->comp_nbytes = (uint32_t)orig_sz;
    return (uint32_t)orig_sz;
#else
    size_t csz = ZSTD_compress(dst, dst_cap, src, orig_sz, POGLS_COMPRESS_LEVEL);
    if (ZSTD_isError(csz)) {
        if (dst_cap >= orig_sz) memcpy(dst, src, orig_sz);
        meta->comp_type   = POGLS_COMP_RAW;
        meta->comp_nbytes = (uint32_t)orig_sz;
        return (uint32_t)orig_sz;
    }

    double ratio = (double)orig_sz / (double)csz;
    if (ratio < (double)POGLS_COMPRESS_MIN_RATIO) {
        if (dst_cap >= orig_sz) memcpy(dst, src, orig_sz);
        meta->comp_type   = POGLS_COMP_RAW;
        meta->comp_nbytes = (uint32_t)orig_sz;
        return (uint32_t)orig_sz;
    }

    meta->comp_type   = POGLS_COMP_ZSTD;
    meta->comp_nbytes = (uint32_t)csz;
    return (uint32_t)csz;
#endif
}

uint32_t pogls_decompress(uint8_t *dst, size_t dst_cap,
                          const uint8_t *src,
                          const PoglsCompMeta *meta)
{
    if (!dst || !src || !meta) return 0;

    if (meta->comp_type == POGLS_COMP_RAW) {
        size_t sz = meta->nbytes_orig < dst_cap ? meta->nbytes_orig : dst_cap;
        memcpy(dst, src, sz);
        return (uint32_t)sz;
    }

#ifndef POGLS_USE_ZSTD
    (void)dst_cap;
    memcpy(dst, src, meta->nbytes_orig);
    return meta->nbytes_orig;
#else
    if (meta->comp_type == POGLS_COMP_ZSTD) {
        size_t dsz = ZSTD_decompress(dst, dst_cap, src, (size_t)meta->comp_nbytes);
        if (ZSTD_isError(dsz) || dsz != meta->nbytes_orig) return 0;
        return (uint32_t)dsz;
    }
    return (uint32_t)meta->nbytes_orig;
#endif
}

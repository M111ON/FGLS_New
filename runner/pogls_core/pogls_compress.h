#ifndef POGLS_COMPRESS_H
#define POGLS_COMPRESS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compression flags (per-tensor) */
#define POGLS_COMP_RAW       0u
#define POGLS_COMP_ZSTD      1u
#define POGLS_COMP_SHELL     2u
#define POGLS_COMP_DELTA     3u
#define POGLS_COMP_GEOPIXEL  4u

#define POGLS_COMPRESS_MIN_RATIO 1.10f
#define POGLS_COMPRESS_LEVEL     3u
#define POGLS_COMPRESS_LEVEL_HI  12u

typedef struct {
    uint32_t comp_type;
    uint32_t comp_nbytes;
    uint32_t nbytes_orig;
} PoglsCompMeta;

uint32_t pogls_compress(uint8_t *dst, size_t dst_cap,
                        const uint8_t *src, size_t orig_sz,
                        PoglsCompMeta *meta);

uint32_t pogls_decompress(uint8_t *dst, size_t dst_cap,
                          const uint8_t *src,
                          const PoglsCompMeta *meta);

#ifdef __cplusplus
}
#endif

#endif /* POGLS_COMPRESS_H */

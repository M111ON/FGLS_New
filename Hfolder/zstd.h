#ifndef ZSTD_H
#define ZSTD_H

#include <stddef.h>

/* Basic API */
size_t ZSTD_compress(void* dst, size_t dstCapacity,
                     const void* src, size_t srcSize,
                     int compressionLevel);

size_t ZSTD_decompress(void* dst, size_t dstCapacity,
                       const void* src, size_t compressedSize);

size_t ZSTD_compressBound(size_t srcSize);

unsigned ZSTD_isError(size_t code);

/* Advanced API for Multi-threading */
typedef struct ZSTD_CCtx_s ZSTD_CCtx;
ZSTD_CCtx* ZSTD_createCCtx(void);
size_t ZSTD_freeCCtx(ZSTD_CCtx* cctx);

typedef enum {
    ZSTD_c_compressionLevel = 100,
    ZSTD_c_nbWorkers = 400
} ZSTD_cParameter;

size_t ZSTD_CCtx_setParameter(ZSTD_CCtx* cctx, ZSTD_cParameter param, int value);

typedef struct {
    const void* src;
    size_t size;
    size_t pos;
} ZSTD_inBuffer;

typedef struct {
    void*  dst;
    size_t size;
    size_t pos;
} ZSTD_outBuffer;

typedef enum {
    ZSTD_e_continue = 0,
    ZSTD_e_flush = 1,
    ZSTD_e_end = 2
} ZSTD_EndDirective;

size_t ZSTD_compressStream2(ZSTD_CCtx* cctx,
                            ZSTD_outBuffer* output,
                            ZSTD_inBuffer* input,
                            ZSTD_EndDirective endOp);

#endif

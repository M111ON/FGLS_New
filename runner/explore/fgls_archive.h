/*
 * fgls_archive.h — FGLS Archive Format v3
 *
 * .fgls = compressed GGUF that stores only MAIN+MIRROR weights
 * v3: body (GGUF header + tensor data) zstd-compressed as ONE stream
 * v2: raw body — extract still reads v2 (codec falls back to RAW)
 *
 * Format:
 *   [FGLS_Header 72B] [TensorTable] [Body = GGUF_header + tensor_data]
 *   v3 body is zstd-compressed; v2 body is raw.
 *
 * Header reserved[16] (v3):
 *   reserved[0..7]  = body_raw_size  (uncompressed body bytes, uint64 LE)
 *   reserved[8]     = body_codec     (0=RAW, 1=ZSTD)
 *   reserved[9..15] = 0
 */

#ifndef FGLS_ARCHIVE_H
#define FGLS_ARCHIVE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define FGLS_MAGIC      0x534C4746u  /* "FGLS" */
#define FGLS_VERSION    3
#define FGLS_HEADER_SZ  72  /* sizeof(FGLS_Header) — must match struct exactly */

#define FGLS_CODEC_RAW       0
#define FGLS_CODEC_ZSTD      1
#define FGLS_CODEC_UNIVERSAL 2  /* Global S-Curve + Per-cell Delta + zstd */

/* Phase classification — proven from bake3 */
static inline int fgls_keep_w(int8_t w) {
    if (w > -8 && w < 8)  return 0;  /* PROBE → discard */
    if (w > 0)            return 1;  /* MAIN → keep */
    if (w >= -32)         return 1;  /* MIRROR → keep */
    return 0;                         /* CANCEL → discard */
}

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_tensors;
    uint32_t n_baked;
    uint64_t orig_size;          /* original GGUF file size */
    uint64_t arch_size;          /* archive file size */
    uint64_t orig_data_start;    /* tensor data offset in original (= GGUF header size) */
    uint64_t kept_weights;       /* total weights kept (MAIN+MIRROR) */
    uint64_t total_weights;      /* total Q8_0 weights */
    uint8_t  reserved[16];
} FGLS_Header;

typedef struct {
    uint16_t name_len;
    /* char name[name_len] follows */
    uint8_t  ndim;
    uint32_t dims[4];
    uint8_t  ggml_type;          /* 8=Q8_0 */
    uint64_t n_elements;
    uint64_t orig_offset;        /* byte offset in original GGUF data section */
    uint64_t arch_offset;        /* byte offset in archive body (relative to body start) */
    uint64_t arch_size;          /* byte size in archive body */
    uint32_t n_blocks;           /* Q8_0 blocks (n_elements/32) */
    uint32_t flags;              /* bit0=is_baked */
} FGLS_TensorEntry;
#pragma pack(pop)

/* Header reserved-area accessors (v3) */
static inline uint64_t fgls_body_raw_size(const FGLS_Header *h) {
    uint64_t v; memcpy(&v, h->reserved, 8); return v;
}
static inline uint8_t fgls_body_codec(const FGLS_Header *h) {
    return h->reserved[8];
}
static inline void fgls_set_body_info(FGLS_Header *h, uint64_t raw_size, uint8_t codec) {
    memset(h->reserved, 0, 16);
    memcpy(h->reserved, &raw_size, 8);
    h->reserved[8] = codec;
}

/*
 * Archive body layout (v3):
 *   [GGUF header: orig_data_start bytes]  ← stored uncompressed inside body stream
 *   [tensor data: per-tensor offsets are relative to this point]
 *
 * Per baked Q8_0 tensor:
 *   [scales: 2 bytes × n_blocks]
 *   [bitmap: 4 bytes × n_blocks]  (bit=1 means kept)
 *   [non-zero weights: variable]
 */
#define FGLS_BLOCK_SZ       34
#define FGLS_WEIGHTS_PER_BLOCK 32

#endif /* FGLS_ARCHIVE_H */

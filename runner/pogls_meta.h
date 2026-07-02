/*
 * pogls_meta.h — POGLS v2 Tensor Metadata Extension
 *
 * Extends the flat .pogls format (v1) with self-describing tensor metadata:
 *   - Per-tensor: name, dtype, dimensions, compression info
 *   - Model-level: architecture, n_layers, n_heads, n_embd, n_ff
 *
 * File layout (v2):
 *   [Header V2     128B]  magic + version + n_tensors + flags +
 *                         tensor_meta_off + tensor_meta_count +
 *                         model_meta_off + model_meta_sz + reserved
 *   [Index       331776B]  20736 × 16B (same as v1 — backward compatible)
 *   [Tensor Meta  ...]    tensor_meta_count × PoglsTensorMeta (64B each)
 *   [Model Meta   ...]    model_meta_sz bytes (json-like or binary)
 *   [Data            ]    tensor raw data (same as v1)
 *
 * v1 readers: read magic + version(1) → skip to data, ignore meta.
 * v2 readers: read magic + version(2) → use meta_offset for metadata.
 */

#ifndef POGLS_META_H
#define POGLS_META_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ═══════════════════════════════════════════════════════════════════
   Constants
   ═══════════════════════════════════════════════════════════════════ */

#define POGLS_META_MAGIC     0x53474F50u    /* "POGS" */
#define POGLS_META_VERSION   2u
#ifndef POGLS_MAX_ADDR
#ifndef POGLS_MAX_ADDR
#define POGLS_MAX_ADDR       20736u
#endif
#endif
#define POGLS_HEADER_SZ      128u
#define POGLS_INDEX_SZ       ((uint64_t)POGLS_MAX_ADDR * 16u)  /* 331776 */
#define POGLS_META_ENTRY_SZ  64u            /* DiamondBlock aligned */

/* Compression flags (per-tensor) */
#define POGLS_COMP_RAW       0u
#define POGLS_COMP_ZSTD      1u
#define POGLS_COMP_SHELL     2u
#define POGLS_COMP_DELTA     3u

/* Header flags */
#define POGLS_FLAG_HAS_TMETA 0x0001u   /* tensor_meta section present */
#define POGLS_FLAG_HAS_MMETA 0x0002u   /* model_meta section present */
#define POGLS_FLAG_MCOMPRESS 0x0004u   /* model-level compression (all tensors) */

/* ═══════════════════════════════════════════════════════════════════
   Per-Tensor Metadata Entry (64B DiamondBlock aligned)
   ═══════════════════════════════════════════════════════════════════
 * One entry per tensor, NOT per address — stored as a flat array.
 * addr field maps back to the 0..20735 index slot.
 * name is human-readable (not truncated — use full pascal-style if >16).
 */
typedef struct {
    uint32_t addr;              /* address in 0..20735 */
    uint32_t dtype;             /* ggml_dtype: 0=F32, 1=F16, 8=Q8_0, etc */
    uint32_t ndim;              /* number of dimensions (0-4) */
    uint32_t nbytes_orig;       /* original (uncompressed) tensor size */
    uint32_t comp_type;         /* POGLS_COMP_RAW/ZSTD/SHELL/DELTA */
    uint32_t comp_nbytes;       /* stored size if compressed, 0 if raw */
    uint32_t dims[4];           /* dimensions (ne[0..3]) */
    char     name[16];          /* truncated tensor name */
    uint32_t _pad0[2];          /* pad to 64B */
} PoglsTensorMeta;              /* 64B */

/* ═══════════════════════════════════════════════════════════════════
   Model-Level Metadata
   ═══════════════════════════════════════════════════════════════════
 * Binary key-value pairs for architecture description.
 * Start with fixed fields, extend with kv pairs.
 */
typedef struct {
    uint32_t n_layers;
    uint32_t n_heads;
    uint32_t n_head_kv;
    uint32_t n_embd;
    uint32_t n_ff;
    uint32_t n_expert;          /* MoE: 0 = not MoE */
    uint32_t n_expert_used;
    uint32_t ftype;             /* GGML ftype */
    uint64_t n_params;          /* total parameter count */
    uint32_t n_tensors;         /* total tensor count (for verification) */
    char     arch[16];          /* architecture string (e.g. "llama", "lfm2") */
    char     desc[64];          /* human-readable description */
} PoglsModelMeta;               /* 140B */

/* ═══════════════════════════════════════════════════════════════════
   V2 Header
   ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t magic;             /* POGLS_META_MAGIC */
    uint32_t version;           /* POGLS_META_VERSION */
    uint32_t n_tensors;         /* number of tensors stored */
    uint32_t flags;             /* POGLS_FLAG_* */

    /* Tensor metadata section (between index and data) */
    uint64_t tensor_meta_off;   /* file offset to PoglsTensorMeta array */
    uint32_t tensor_meta_count; /* number of entries in meta array */
    uint32_t _pad0;             /* pad to align model_meta_off to 8 */

    /* Model metadata section */
    uint64_t model_meta_off;    /* file offset to PoglsModelMeta */
    uint32_t model_meta_sz;     /* bytes of model meta (0 if absent) */

    uint8_t  _pad[84];          /* pad to 128B */
} PoglsStoreHeader;             /* 128B */

/* ═══════════════════════════════════════════════════════════════════
   API
   ═══════════════════════════════════════════════════════════════════ */

/* Initialize v2 header */
static inline void pogls_meta_header_init(PoglsStoreHeader *hdr) {
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic   = POGLS_META_MAGIC;
    hdr->version = POGLS_META_VERSION;
}

/* Compute data section offset (after header + index + meta) */
static inline uint64_t pogls_meta_data_off(const PoglsStoreHeader *hdr) {
    uint64_t off = sizeof(*hdr) + POGLS_INDEX_SZ; /* 128 + 331776 */
    if (hdr->tensor_meta_off > 0) {
        off = hdr->tensor_meta_off +
              (uint64_t)hdr->tensor_meta_count * POGLS_META_ENTRY_SZ;
    }
    if (hdr->model_meta_off > 0) {
        uint64_t mend = hdr->model_meta_off + hdr->model_meta_sz;
        if (mend > off) off = mend;
    }
    return off;
}

/* Init tensor metadata entry */
static inline void pogls_meta_entry_init(PoglsTensorMeta *e) {
    memset(e, 0, sizeof(*e));
}

/* --- Read/write helpers --- */

/* Read v1 or v2 file: header + index + meta + data start offset */
/* Returns 0 on success, -1 on error */
/* v1 (64B header): data_off = sizeof(PoglsStore) = 331840 */
/* v2 (128B header): data_off = header + index + meta */
static inline int pogls_meta_read(const char *path,
                                   PoglsStoreHeader *hdr_out,
                                   uint8_t *idx_out,       /* 331776B, or NULL */
                                   void *meta_out,         /* tensor_meta_count × 64B, or NULL */
                                   uint64_t *data_off_out) /* data section offset */
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    /* Read magic + version first (8 bytes — same offset in v1 and v2) */
    uint32_t magic, version;
    if (fread(&magic, 4, 1, f) != 1) { fclose(f); return -1; }
    if (fread(&version, 4, 1, f) != 1) { fclose(f); return -1; }
    if (magic != POGLS_META_MAGIC) { fclose(f); return -1; }

    if (version == 1) {
        /* v1: 64B header = just magic+version+n_tensors+flags+pad */
        /* Skip rest of v1 header (56 more bytes to reach 64) */
        if (fseek(f, 64 - 8, SEEK_CUR) != 0) { fclose(f); return -1; }

        if (hdr_out) {
            memset(hdr_out, 0, sizeof(*hdr_out));
            hdr_out->magic   = magic;
            hdr_out->version = version;
            /* Can't read n_tensors from v1 header without full struct */
            /* Re-read from start to get n_tensors */
            fseek(f, 8, SEEK_SET); /* skip magic+version already read */
            uint32_t nt, fl;
            fread(&nt, 4, 1, f);
            fread(&fl, 4, 1, f);
            hdr_out->n_tensors = nt;
            hdr_out->flags     = fl;
        }

        /* Read index (after 64B v1 header) */
        if (idx_out) {
            if (fread(idx_out, POGLS_INDEX_SZ, 1, f) != 1) { fclose(f); return -1; }
        } else {
            if (fseek(f, POGLS_INDEX_SZ, SEEK_CUR) != 0) { fclose(f); return -1; }
        }

        /* v1 has no meta section */
        if (data_off_out) *data_off_out = (uint64_t)64 + POGLS_INDEX_SZ;

        fclose(f);
        return 0;
    }

    /* v2+ */
    /* Re-read full header from start */
    PoglsStoreHeader hdr;
    fseek(f, 0, SEEK_SET);
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return -1; }
    if (hdr_out) *hdr_out = hdr;

    /* Read index */
    if (idx_out) {
        if (fread(idx_out, POGLS_INDEX_SZ, 1, f) != 1) { fclose(f); return -1; }
    } else {
        if (fseek(f, POGLS_INDEX_SZ, SEEK_CUR) != 0) { fclose(f); return -1; }
    }

    /* Read tensor meta */
    if (meta_out && hdr.tensor_meta_count > 0) {
        uint64_t sz = (uint64_t)hdr.tensor_meta_count * POGLS_META_ENTRY_SZ;
        if (fread(meta_out, sz, 1, f) != 1) { fclose(f); return -1; }
    }

    /* Data offset */
    if (data_off_out) *data_off_out = pogls_meta_data_off(&hdr);

    fclose(f);
    return 0;
}

/* Write v2 file with metadata */
static inline int pogls_meta_write(const char *path,
                                    const PoglsStoreHeader *hdr,
                                    const uint8_t *idx,         /* 331776B */
                                    const PoglsTensorMeta *meta,/* tensor_meta_count entries */
                                    const uint8_t *model_meta,  /* model_meta_sz bytes */
                                    const uint8_t *data,        /* tensor data */
                                    uint64_t data_sz)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    /* Write header */
    if (fwrite(hdr, sizeof(*hdr), 1, f) != 1) { fclose(f); return -1; }

    /* Write index */
    uint64_t idx_sz = POGLS_INDEX_SZ;
    if (idx) {
        if (fwrite(idx, idx_sz, 1, f) != 1) { fclose(f); return -1; }
    } else {
        uint8_t zero[4096] = {0};
        for (uint64_t w = 0; w < idx_sz; w += sizeof(zero)) {
            uint64_t chunk = idx_sz - w;
            if (chunk > sizeof(zero)) chunk = sizeof(zero);
            if (fwrite(zero, chunk, 1, f) != 1) { fclose(f); return -1; }
        }
    }

    /* Write tensor meta */
    if (meta && hdr->tensor_meta_count > 0) {
        uint64_t meta_sz = (uint64_t)hdr->tensor_meta_count * POGLS_META_ENTRY_SZ;
        if (fwrite(meta, meta_sz, 1, f) != 1) { fclose(f); return -1; }
    }

    /* Write model meta */
    if (model_meta && hdr->model_meta_sz > 0) {
        if (fwrite(model_meta, hdr->model_meta_sz, 1, f) != 1) { fclose(f); return -1; }
    }

    /* Write tensor data */
    if (data && data_sz > 0) {
        if (fwrite(data, data_sz, 1, f) != 1) { fclose(f); return -1; }
    }

    fclose(f);
    return 0;
}

/* --- Lookup helpers --- */

/* Find tensor meta by address (linear scan — meta_count ≤ n_tensors) */
static inline const PoglsTensorMeta *pogls_meta_find(
    const PoglsTensorMeta *meta, uint32_t count, uint32_t addr)
{
    for (uint32_t i = 0; i < count; i++)
        if (meta[i].addr == addr)
            return &meta[i];
    return NULL;
}

/* Find tensor meta by name (linear scan) */
static inline const PoglsTensorMeta *pogls_meta_find_name(
    const PoglsTensorMeta *meta, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++)
        if (strncmp(meta[i].name, name, 16) == 0)
            return &meta[i];
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
   Compression Helpers (require zstd, guarded by POGLS_USE_ZSTD)
   ═══════════════════════════════════════════════════════════════════
 * Strategy (from benchmark on Q4_K_M weights):
 *   - Q4 quantized weights: 1.00× ratio — store raw, skip compression
 *   - Sparse biases: zstd gives 3.3× — good
 *   - Dense f32/f16: try zstd, keep if ratio >= MIN_RATIO
 *
 * Usage: #define POGLS_USE_ZSTD before including this header,
 *        link with zstd.dll.
 */

#ifdef POGLS_USE_ZSTD
#include <zstd.h>

#define POGLS_COMPRESS_MIN_RATIO 1.10f   /* skip if below this */
#define POGLS_COMPRESS_LEVEL     3u
#define POGLS_COMPRESS_LEVEL_HI  12u     /* for off-line archiving */

/* Compress a tensor. Returns compressed size, or 0 on error.
 * If ratio < MIN_RATIO, sets comp_type to RAW and returns orig_sz (no compress).
 * Caller should check meta->comp_type to know if compression happened.
 */
static inline uint32_t pogls_compress_tensor(
    uint8_t *dst,           /* output buffer: ZSTD_compressBound(orig_sz) */
    size_t dst_cap,
    const uint8_t *src,     /* original tensor data */
    size_t orig_sz,
    PoglsTensorMeta *meta)  /* filled with comp_type/comp_nbytes */
{
    if (!dst || !src || !meta || orig_sz == 0) return 0;

    size_t csz = ZSTD_compress(dst, dst_cap, src, orig_sz, POGLS_COMPRESS_LEVEL);
    if (ZSTD_isError(csz)) {
        if (dst_cap >= orig_sz) memcpy(dst, src, orig_sz);
        meta->comp_type   = POGLS_COMP_RAW;
        meta->comp_nbytes = (uint32_t)orig_sz;
        return (uint32_t)orig_sz;
    }

    double ratio = (double)orig_sz / (double)csz;
    if (ratio < (double)POGLS_COMPRESS_MIN_RATIO) {
        /* Not worth compressing — store raw */
        if (dst_cap >= orig_sz) memcpy(dst, src, orig_sz);
        meta->comp_type   = POGLS_COMP_RAW;
        meta->comp_nbytes = (uint32_t)orig_sz;
        return (uint32_t)orig_sz;
    }

    meta->comp_type   = POGLS_COMP_ZSTD;
    meta->comp_nbytes = (uint32_t)csz;
    return (uint32_t)csz;
}

/* Decompress a tensor. Returns decompressed size, or 0 on error.
 * For RAW tensors, copies from src to dst.
 */
static inline uint32_t pogls_decompress_tensor(
    uint8_t *dst,
    size_t dst_cap,
    const uint8_t *src,
    const PoglsTensorMeta *meta)
{
    if (!dst || !src || !meta) return 0;

    if (meta->comp_type == POGLS_COMP_RAW) {
        size_t sz = meta->nbytes_orig < dst_cap ? meta->nbytes_orig : dst_cap;
        memcpy(dst, src, sz);
        return (uint32_t)sz;
    }

    if (meta->comp_type == POGLS_COMP_ZSTD) {
        size_t dsz = ZSTD_decompress(dst, dst_cap, src, (size_t)meta->comp_nbytes);
        if (ZSTD_isError(dsz) || dsz != meta->nbytes_orig) return 0;
        return (uint32_t)dsz;
    }

    /* Unknown type — treat as raw */
    return (uint32_t)meta->nbytes_orig;
}

#endif /* POGLS_USE_ZSTD */

#endif /* POGLS_META_H */

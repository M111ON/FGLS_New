#ifndef KV_SID_EVICT_H
#define KV_SID_EVICT_H

/*
 * KV SID Compression — DGLS Binary Shell Codec compress/decompress
 * for KV cache tensors.
 *
 * Replaces the old raw-backup-buffer approach (which DOUBLED RAM)
 * with lossless DGLS shell compression.
 *
 * Uses Binary Shell Codec (binary_shell_codec.h) which applies:
 *   - FLAT (2B) for all-zero 64B chunks (unused context positions)
 *   - SPARSE (3+2*nz B) for chunks with ≤16 non-zero bytes
 *   - DENSE (6+csz B) other chunks via Zstd level 3
 *
 * Compression ratio depends on KV data:
 *   - Unused cache regions: ~32:1 (FLAT)
 *   - Active fp16 cache:    ~1.5-2x (Zstd on fp16 arrays)
 *   - Overall partial-fill: 2-5x typical
 *
 * ⚠ Peak RAM during decode is UNCHANGED (original KV buffer stays).
 *   RAM saving comes from:
 *   1. No more full-size backup buffers (vs old KV_ARCHIVE)
 *   2. Physical page decommit via VirtualAlloc MEM_RESET
 *      (reduces working set physical RAM)
 *
 * Include paths needed:
 *   -I../collection/dgls/diamond/include
 *   -I../collection/dgls/geo/include
 * Linking: -lzstd
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include "binary_shell_codec.h"

#define KV_SID_MAX_LAYERS 64

/* Magic header for compressed blob */
#define KV_SNAP_MAGIC 0x504E414B  /* "KSNAP" little-endian */
#define KV_COMPRESSED_MAGIC 0x4B56434D  /* "KVC" */

/* Compression result constants */
#define KV_OK        0
#define KV_ERR       -1
#define KV_SKIPPED   1   /* compression ratio < 1.1, stored uncompressed */

typedef struct {
    /* ggml_tensor pointers (for tensor->data access) */
    void    *k_tensor;          /* ggml_tensor* for K cache */
    void    *v_tensor;          /* ggml_tensor* for V cache */

    /* Original data pointers (into the big KV buffer, views) */
    void    *k_data;            /* original K data pointer */
    void    *v_data;            /* original V data pointer */
    size_t   k_size;            /* K data size in bytes */
    size_t   v_size;            /* V data size in bytes */

    /* Compressed snapshots (NULL = no snapshot taken) */
    void    *snap_k;            /* compressed K data */
    size_t   snap_k_size;       /* compressed K size */
    void    *snap_v;            /* compressed V data */
    size_t   snap_v_size;       /* compressed V size */

    /* Eviction state */
    int      evicted;           /* 1 = tensor->data swapped to working buf */
    void    *work_k;            /* working K buffer (zeroed, for evict) */
    void    *work_v;            /* working V buffer (zeroed, for evict) */
    int      layer_id;          /* model layer index */
} KVSidLayer;

typedef struct {
    KVSidLayer layers[KV_SID_MAX_LAYERS];
    int        n_layers;        /* number of tracked layers */
    int        n_evicted;       /* currently evicted count */
    size_t     total_k_bytes;   /* total K cache bytes */
    size_t     total_v_bytes;   /* total V cache bytes */
    size_t     total_snap_bytes;/* total compressed snapshot bytes */
    int        enabled;         /* 0=disabled, 1=enabled */
} KVSidCtx;

/* Initialize context */
static inline void kv_sid_init(KVSidCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
#ifdef KV_ARCHIVE
    ctx->enabled = 1;
    fprintf(stderr, "[kv-sid] init: enabled, max %d layers\n", KV_SID_MAX_LAYERS);
#endif
}


/* =============================================================
 * Low-level compress / decompress helpers
 * ============================================================= */

/* Compress raw data with Binary Shell Codec.
 * out: output buffer (allocated via malloc, caller must free)
 * out_size: [out] compressed size in bytes
 * Returns KV_OK, KV_SKIPPED (stored uncompressed if ratio<1.1), or KV_ERR.
 *
 * Format: [4B magic][8B orig_size][4B n_chunks][chunks...] */
static inline int kv_compress_buf(const void *data, size_t size,
                                  void **out, size_t *out_size)
{
    uint64_t n_chunks = (size + 63) / 64;       /* round up to 64B */
    uint64_t max_enc  = n_chunks * 70 + 16;      /* worst case per chunk */
    uint8_t *enc = (uint8_t *)malloc((size_t)max_enc);
    if (!enc) return KV_ERR;

    size_t enc_pos = 16; /* skip header */

    for (uint64_t ci = 0; ci < n_chunks; ci++) {
        size_t off = (size_t)(ci * 64);
        size_t remain = size > off ? size - off : 0;
        uint8_t chunk64[64]; /* padded */
        memset(chunk64, 0, 64);
        memcpy(chunk64, (const uint8_t *)data + off,
               remain < 64 ? remain : 64);

        enc_pos += bin_encode_chunk(enc + enc_pos, chunk64, &(BinChunkResult){0});
    }

    /* Write header */
    *(uint32_t *)(enc + 0)  = KV_COMPRESSED_MAGIC;
    *(uint64_t *)(enc + 4)  = (uint64_t)size;
    *(uint32_t *)(enc + 12) = (uint32_t)n_chunks;

    size_t total = enc_pos;

    /* Fallback: if compression ratio < 1.1, store raw */
    double ratio = (double)size / (double)(total > 16 ? total - 16 : 1);
    if (total >= size || ratio < 1.1) {
        free(enc);
        /* Store uncompressed: [magic][size] then raw data */
        uint8_t *raw_out = (uint8_t *)malloc(16 + size);
        if (!raw_out) return KV_ERR;
        *(uint32_t *)(raw_out + 0) = KV_COMPRESSED_MAGIC;
        *(uint64_t *)(raw_out + 4) = (uint64_t)size;
        *(uint32_t *)(raw_out + 12) = 0; /* n_chunks=0 means uncompressed */
        memcpy(raw_out + 16, data, size);
        *out = raw_out;
        *out_size = 16 + size;
        return KV_SKIPPED;
    }

    *out = enc;
    *out_size = total;
    return KV_OK;
}

/* Decompress blob back to original data.
 * Returns pointer to malloc'd original data (caller must free), or NULL on error.
 * out_size: [out] original size */
static inline void *kv_decompress_buf(const void *compressed, size_t comp_size,
                                       size_t *out_size)
{
    if (comp_size < 16) return NULL;
    const uint8_t *enc = (const uint8_t *)compressed;

    uint32_t magic = *(const uint32_t *)(enc + 0);
    if (magic != KV_COMPRESSED_MAGIC) return NULL;

    size_t orig_size = (size_t)(*(const uint64_t *)(enc + 4));
    uint32_t n_chunks = *(const uint32_t *)(enc + 12);

    if (out_size) *out_size = orig_size;

    /* Uncompressed path */
    if (n_chunks == 0) {
        if (comp_size < 16 + orig_size) return NULL;
        uint8_t *raw = (uint8_t *)malloc(orig_size);
        if (!raw) return NULL;
        memcpy(raw, enc + 16, orig_size);
        return raw;
    }

    /* Compressed path */
    uint8_t *dec = (uint8_t *)malloc(orig_size > 0 ? orig_size : 1);
    if (!dec) return NULL;

    size_t dec_pos = 0;
    size_t comp_pos = 16; /* start after header */

    for (uint32_t ci = 0; ci < n_chunks; ci++) {
        if (comp_pos >= comp_size) { free(dec); return NULL; }
        uint8_t chunk_out[64];
        uint32_t consumed = bin_decode_chunk(enc + comp_pos, chunk_out);
        if (consumed == 0) { free(dec); return NULL; }
        size_t copy = (orig_size - dec_pos) > 64 ? 64 : (orig_size - dec_pos);
        memcpy(dec + dec_pos, chunk_out, copy);
        dec_pos += copy;
        comp_pos += (size_t)consumed;
    }

    return dec;
}


/* =============================================================
 * Snapshot API — compress layer data to compact storage
 * ============================================================= */

/* Snapshot (compress) a single layer's K/V from tensor->data */
static inline int kv_snapshot_layer(KVSidCtx *ctx, int layer_idx) {
    if (!ctx->enabled) return KV_ERR;
    if (layer_idx < 0 || layer_idx >= ctx->n_layers) return KV_ERR;
    KVSidLayer *L = &ctx->layers[layer_idx];

    /* Free old snapshots if any */
    free(L->snap_k); L->snap_k = NULL; L->snap_k_size = 0;
    free(L->snap_v); L->snap_v = NULL; L->snap_v_size = 0;

    /* Get current tensor->data pointers */
    void **k_field = (void **)((char *)L->k_tensor + 248);
    void **v_field = (void **)((char *)L->v_tensor + 248);

    int ret = KV_OK;

    if (*k_field && L->k_size > 0) {
        int r = kv_compress_buf(*k_field, L->k_size,
                                &L->snap_k, &L->snap_k_size);
        if (r != KV_ERR) ctx->total_snap_bytes += L->snap_k_size;
        if (r == KV_ERR) ret = KV_ERR;
    }
    if (*v_field && L->v_size > 0) {
        int r = kv_compress_buf(*v_field, L->v_size,
                                &L->snap_v, &L->snap_v_size);
        if (r != KV_ERR) ctx->total_snap_bytes += L->snap_v_size;
        if (r == KV_ERR) ret = KV_ERR;
    }

    return ret;
}

/* Snapshot (compress) ALL layers */
static inline int kv_snapshot_all(KVSidCtx *ctx) {
    if (!ctx->enabled) return KV_ERR;
    ctx->total_snap_bytes = 0;
    int errs = 0;
    for (int i = 0; i < ctx->n_layers; i++) {
        if (kv_snapshot_layer(ctx, i) != KV_OK) errs++;
    }
    size_t total_orig = ctx->total_k_bytes + ctx->total_v_bytes;
    double ratio = total_orig > 0 ?
        (double)total_orig / (double)(ctx->total_snap_bytes > 0 ? ctx->total_snap_bytes : 1) : 0;
    fprintf(stderr, "[kv-sid] snapshot %d layers: orig=%zu snap=%zu ratio=%.2fx (errs=%d)\n",
        ctx->n_layers, total_orig, ctx->total_snap_bytes, ratio, errs);
    return errs == 0 ? KV_OK : KV_ERR;
}

/* Restore (decompress) a single layer's K/V back to tensor->data */
static inline int kv_restore_layer(KVSidCtx *ctx, int layer_idx) {
    if (!ctx->enabled) return KV_ERR;
    if (layer_idx < 0 || layer_idx >= ctx->n_layers) return KV_ERR;
    KVSidLayer *L = &ctx->layers[layer_idx];

    void **k_field = (void **)((char *)L->k_tensor + 248);
    void **v_field = (void **)((char *)L->v_tensor + 248);

    int ret = KV_OK;

    if (L->snap_k && L->snap_k_size > 0) {
        size_t dec_size = 0;
        void *dec = kv_decompress_buf(L->snap_k, L->snap_k_size, &dec_size);
        if (dec) {
            /* If layer is evicted, decompress to working buffer */
            if (L->evicted && L->work_k) {
                memcpy(L->work_k, dec, dec_size < L->k_size ? dec_size : L->k_size);
            } else {
                memcpy(*k_field, dec, dec_size < L->k_size ? dec_size : L->k_size);
            }
            free(dec);
        } else {
            ret = KV_ERR;
        }
    }

    if (L->snap_v && L->snap_v_size > 0) {
        size_t dec_size = 0;
        void *dec = kv_decompress_buf(L->snap_v, L->snap_v_size, &dec_size);
        if (dec) {
            if (L->evicted && L->work_v) {
                memcpy(L->work_v, dec, dec_size < L->v_size ? dec_size : L->v_size);
            } else {
                memcpy(*v_field, dec, dec_size < L->v_size ? dec_size : L->v_size);
            }
            free(dec);
        } else {
            ret = KV_ERR;
        }
    }

    return ret;
}

/* Restore (decompress) ALL layers */
static inline int kv_restore_all(KVSidCtx *ctx) {
    if (!ctx->enabled) return KV_ERR;
    int errs = 0;
    for (int i = 0; i < ctx->n_layers; i++) {
        if (kv_restore_layer(ctx, i) != KV_OK) errs++;
    }
    fprintf(stderr, "[kv-sid] restore %d layers (errs=%d)\n", ctx->n_layers, errs);
    return errs == 0 ? KV_OK : KV_ERR;
}

/* Free snapshot blobs (but keep layer registration) */
static inline void kv_sid_free_snapshots(KVSidCtx *ctx) {
    for (int i = 0; i < ctx->n_layers; i++) {
        KVSidLayer *L = &ctx->layers[i];
        free(L->snap_k); L->snap_k = NULL; L->snap_k_size = 0;
        free(L->snap_v); L->snap_v = NULL; L->snap_v_size = 0;
    }
    ctx->total_snap_bytes = 0;
}


/* =============================================================
 * Eviction API — pointer swap for "forgetting" behavior
 * (needs working buffers, does NOT compress)
 * ============================================================= */

/* Evict a single layer — zero-copy pointer swap to zeroed working buf.
 * SNAPSHOTS automatically before evict if a compressed snapshot
 * doesn't already exist. */
static inline int kv_sid_evict_layer(KVSidCtx *ctx, int layer_idx) {
    if (layer_idx < 0 || layer_idx >= ctx->n_layers) return -1;
    KVSidLayer *L = &ctx->layers[layer_idx];
    if (L->evicted) return 0;

    /* Snapshot first if not already done */
    if (!L->snap_k && !L->snap_v && L->k_size > 0) {
        kv_snapshot_layer(ctx, layer_idx);
    }

    void **k_field = (void **)((char *)L->k_tensor + 248);
    void **v_field = (void **)((char *)L->v_tensor + 248);

    /* Allocate working buffers (first evict) or reuse */
    if (!L->work_k && L->k_size > 0)
        L->work_k = calloc(1, L->k_size);
    if (!L->work_v && L->v_size > 0)
        L->work_v = calloc(1, L->v_size);

    if (L->k_tensor && L->work_k) {
        L->k_data = *k_field;
        *k_field = L->work_k;
    }
    if (L->v_tensor && L->work_v) {
        L->v_data = *v_field;
        *v_field = L->work_v;
    }

    L->evicted = 1;
    ctx->n_evicted++;
    return 0;
}

/* Restore a single layer — swap pointer back to original */
static inline int kv_sid_restore_layer(KVSidCtx *ctx, int layer_idx) {
    if (layer_idx < 0 || layer_idx >= ctx->n_layers) return -1;
    KVSidLayer *L = &ctx->layers[layer_idx];
    if (!L->evicted) return 0;

    void **k_field = (void **)((char *)L->k_tensor + 248);
    void **v_field = (void **)((char *)L->v_tensor + 248);

    if (L->k_tensor && L->k_data) {
        /* Restore original data from snapshot first if we have one */
        *k_field = L->k_data;
    }
    if (L->v_tensor && L->v_data) {
        *v_field = L->v_data;
    }

    L->evicted = 0;
    ctx->n_evicted--;
    return 0;
}

/* Evict oldest N layers */
static inline void kv_sid_evict_oldest(KVSidCtx *ctx, int n) {
    if (n <= 0 || n > ctx->n_layers) n = ctx->n_layers;
    for (int i = 0; i < n; i++) {
        if (!ctx->layers[i].evicted)
            kv_sid_evict_layer(ctx, i);
    }
    fprintf(stderr, "[kv-sid] evict oldest %d (total: %d)\n", n, ctx->n_evicted);
}

/* Evict ALL layers */
static inline void kv_sid_evict_all(KVSidCtx *ctx) {
    for (int i = 0; i < ctx->n_layers; i++) {
        if (!ctx->layers[i].evicted)
            kv_sid_evict_layer(ctx, i);
    }
    fprintf(stderr, "[kv-sid] evict all: %d layers\n", ctx->n_evicted);
}

/* Restore ALL layers */
static inline void kv_sid_restore_all(KVSidCtx *ctx) {
    for (int i = ctx->n_layers - 1; i >= 0; i--) {
        if (ctx->layers[i].evicted)
            kv_sid_restore_layer(ctx, i);
    }
    fprintf(stderr, "[kv-sid] restore all: 0 evicted\n");
}


/* =============================================================
 * Working set reduction — free physical pages after snapshot
 * ============================================================= */

#ifdef _WIN32
/* Decommit physical pages of the original KV buffer (Windows).
 * After snapshot(), we can tell the OS to reclaim physical RAM
 * backing the original buffer pages. The virtual address space
 * stays reserved; on next access, pages fault in as zero.
 *
 * This reduces WORKING SET (physical RAM) without needing to
 * free/reallocate the buffer. */
static inline int kv_sid_decommit(void *base, size_t size) {
    if (!base || size == 0) return KV_ERR;
    /* Use MEM_RESET to hint that pages can be discarded */
    void *ret = VirtualAlloc(base, size, MEM_RESET, PAGE_NOACCESS);
    if (!ret) {
        /* MEM_RESET may fail if base isn't aligned; try MEM_DECOMMIT */
        ret = VirtualAlloc(base, size, MEM_DECOMMIT, PAGE_READWRITE);
    }
    return ret ? KV_OK : KV_ERR;
}

/* Recommit pages before access (restore from snapshot first) */
static inline int kv_sid_recommit(void *base, size_t size) {
    if (!base || size == 0) return KV_ERR;
    void *ret = VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE);
    return ret ? KV_OK : KV_ERR;
}
#else
/* POSIX madvise fallback */
#include <sys/mman.h>
static inline int kv_sid_decommit(void *base, size_t size) {
    if (!base || size == 0) return KV_ERR;
    int ret = madvise(base, size, MADV_DONTNEED);
    return ret == 0 ? KV_OK : KV_ERR;
}
static inline int kv_sid_recommit(void *base, size_t size) {
    (void)base; (void)size;
    return KV_OK; /* MADV_DONTNEED pages fault back in automatically */
}
#endif


/* =============================================================
 * Registration
 * ============================================================= */

/* Register layers. NO backup allocation — only stores pointers.
 * snapshots are created on first evict or explicit kv_snapshot_all(). */
static inline void kv_sid_register_layers(KVSidCtx *ctx,
    void **k_tensor, void **v_tensor,
    void **k_data, void **v_data,
    size_t *k_size, size_t *v_size,
    int *layer_id, int n)
{
    if (n > KV_SID_MAX_LAYERS) n = KV_SID_MAX_LAYERS;
    ctx->n_layers = n;
    ctx->total_k_bytes = 0;
    ctx->total_v_bytes = 0;
    ctx->total_snap_bytes = 0;

    for (int i = 0; i < n; i++) {
        KVSidLayer *L = &ctx->layers[i];
        L->k_tensor     = k_tensor[i];
        L->v_tensor     = v_tensor[i];
        L->k_data       = k_data[i];
        L->v_data       = v_data[i];
        L->k_size       = k_size[i];
        L->v_size       = v_size[i];
        L->layer_id     = layer_id[i];
        L->evicted      = 0;
        L->work_k       = NULL;
        L->work_v       = NULL;
        L->snap_k       = NULL;
        L->snap_v       = NULL;
        L->snap_k_size  = 0;
        L->snap_v_size  = 0;
        ctx->total_k_bytes += k_size[i];
        ctx->total_v_bytes += v_size[i];
    }
    size_t total = ctx->total_k_bytes + ctx->total_v_bytes;
    fprintf(stderr, "[kv-sid] registered %d layers: K=%zu V=%zu total=%zu\n",
        n, ctx->total_k_bytes, ctx->total_v_bytes, total);
}


/* =============================================================
 * Cleanup
 * ============================================================= */

static inline void kv_sid_free(KVSidCtx *ctx) {
    for (int i = 0; i < ctx->n_layers; i++) {
        KVSidLayer *L = &ctx->layers[i];
        free(L->snap_k);    L->snap_k = NULL;    L->snap_k_size = 0;
        free(L->snap_v);    L->snap_v = NULL;    L->snap_v_size = 0;
        free(L->work_k);    L->work_k = NULL;
        free(L->work_v);    L->work_v = NULL;
    }
    fprintf(stderr, "[kv-sid] freed %d layers\n", ctx->n_layers);
    ctx->n_layers = 0;
    ctx->n_evicted = 0;
    ctx->total_snap_bytes = 0;
}

/* Get backup pointers (for backward compat) */
static inline int kv_sid_get_backup(const KVSidCtx *ctx, int layer_idx,
    void **backup_k, void **backup_v)
{
    if (layer_idx < 0 || layer_idx >= ctx->n_layers) return -1;
    const KVSidLayer *L = &ctx->layers[layer_idx];
    if (backup_k) *backup_k = L->work_k;
    if (backup_v) *backup_v = L->work_v;
    return 0;
}

/* Get ggml_tensor pointers */
static inline int kv_sid_get_tensors(const KVSidCtx *ctx, int layer_idx,
    void **k_tensor, void **v_tensor)
{
    if (layer_idx < 0 || layer_idx >= ctx->n_layers) return -1;
    const KVSidLayer *L = &ctx->layers[layer_idx];
    if (k_tensor) *k_tensor = L->k_tensor;
    if (v_tensor) *v_tensor = L->v_tensor;
    return 0;
}

/* Print status */
static inline void kv_sid_print_status(const KVSidCtx *ctx) {
    size_t total = ctx->total_k_bytes + ctx->total_v_bytes;
    double ratio = ctx->total_snap_bytes > 0 ?
        (double)total / (double)ctx->total_snap_bytes : 0;
    fprintf(stderr, "[kv-sid] status: %d/%d evicted, total=%zu snap=%zu ratio=%.2fx\n",
        ctx->n_evicted, ctx->n_layers,
        total, ctx->total_snap_bytes, ratio);
}

#endif /* KV_SID_EVICT_H */

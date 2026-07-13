#ifndef KV_PAGE_STORE_H
#define KV_PAGE_STORE_H

/*
 * KV Page Store — token-position-granular KV cache paging
 *
 * Architecture:
 *   KV cache positions are divided into "pages" of KV_PAGE_SIZE tokens.
 *   Each page spans ALL attention layers for that position range.
 *
 *   Live cache = llama.cpp's normal KV buffer (contiguous per layer).
 *   Backup store = separate mmap region for compressed page snapshots.
 *
 *   On evict:  compress page (K+V for all attn layers) → store in mmap → zero live positions
 *   On restore: decompress from mmap → write back to live cache positions
 *
 * Integration with FGLS/POGLS/DGLS:
 *   - DRamTile: mmap backing, deterministic slot addressing, O(1) lookup
 *   - Binary Shell Codec: FLAT/SPARSE/DENSE compression (reuse from kv_sid_evict)
 *   - POGLS: page_id → addr encode/decode for visualization
 *
 * The model never knows — it reads from the same tensor->data pointers.
 * We only touch data content at specific position offsets (memcpy/memset).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "binary_shell_codec.h"

#define KV_PAGE_SIZE        128     /* tokens per page (power of 2) */
#define KV_PAGE_MAX_PAGES   256     /* max pages (n_ctx / KV_PAGE_SIZE) */
#define KV_PAGE_MAX_LAYERS  64      /* max attention layers */
#define KV_PAGE_SLOT_ALIGN  64      /* cache-line alignment */

/* Compression magic */
#define KV_PAGE_MAGIC       0x50474B56  /* "VKGP" little-endian */

/* ── Page slot in mmap ────────────────────────────────────────
 *
 * Each page slot has a fixed maximum size (uncompressed page size).
 * This guarantees dt_put never fails due to size mismatch.
 * Compressed data is stored within the slot; remaining bytes unused.
 *
 * Slot layout:
 *   [4B magic][4B compressed_size][4B original_size][compressed_data...]
 */
#define KV_PAGE_SLOT_META  16  /* header bytes before compressed data */

typedef struct {
    /* mmap backing */
    uint8_t *mmap_base;
    size_t   mmap_capacity;
    size_t   mmap_used;
    int      is_mmap;

    /* Per-page metadata */
    size_t   slot_size;                        /* bytes per slot (uncompressed page size, aligned) */
    int      page_valid[KV_PAGE_MAX_PAGES];    /* 1 = slot has valid snapshot */
    size_t   page_comp_size[KV_PAGE_MAX_PAGES];/* compressed data size in slot */
    uint32_t page_lru[KV_PAGE_MAX_PAGES];      /* LRU tick for eviction */
    int      page_evicted[KV_PAGE_MAX_PAGES];  /* 1 = page zeroed in live cache */

    /* Layer info (filled during init) */
    void    *k_tensors[KV_PAGE_MAX_LAYERS];    /* ggml_tensor* for K per attn layer */
    void    *v_tensors[KV_PAGE_MAX_LAYERS];    /* ggml_tensor* for V per attn layer */
    void    *k_data[KV_PAGE_MAX_LAYERS];       /* K data base pointer per attn layer */
    void    *v_data[KV_PAGE_MAX_LAYERS];       /* V data base pointer per attn layer */
    size_t   k_nb1[KV_PAGE_MAX_LAYERS];        /* K stride nb[1] per attn layer */
    size_t   v_nb1[KV_PAGE_MAX_LAYERS];        /* V stride nb[1] per attn layer */
    size_t   k_size[KV_PAGE_MAX_LAYERS];       /* K total size per attn layer */
    size_t   v_size[KV_PAGE_MAX_LAYERS];       /* V total size per attn layer */
    int      n_embd_k[KV_PAGE_MAX_LAYERS];     /* ne[0] per attn layer */
    int      layer_id[KV_PAGE_MAX_LAYERS];     /* model layer index */
    int      n_attn_layers;                     /* number of registered attn layers */
    int      n_ctx;                             /* context size */

    /* Stats */
    int      n_pages;           /* total pages (n_ctx / KV_PAGE_SIZE) */
    int      n_evicted;         /* currently evicted pages */
    uint32_t lru_tick;          /* global LRU counter */
    size_t   total_snap_bytes;  /* total compressed snapshot bytes */
    size_t   total_orig_bytes;  /* total original bytes (all pages) */
    int      enabled;
} KVPageStore;


/* =============================================================
 * Low-level: mmap region for page slots
 * ============================================================= */

static inline int kv_page_mmap_init(KVPageStore *s, size_t capacity) {
#ifdef _WIN32
    s->mmap_base = (uint8_t*)VirtualAlloc(NULL, capacity,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s->is_mmap = (s->mmap_base != NULL);
#else
    s->mmap_base = (uint8_t*)mmap(NULL, capacity,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    s->is_mmap = (s->mmap_base != MAP_FAILED);
    if (!s->is_mmap) s->mmap_base = NULL;
#endif
    if (!s->mmap_base) {
        fprintf(stderr, "[kv-page] mmap failed for %zu bytes, falling back to malloc\n", capacity);
        s->mmap_base = (uint8_t*)malloc(capacity);
        s->is_mmap = 0;
    }
    if (!s->mmap_base) return -1;
    s->mmap_capacity = capacity;
    s->mmap_used = 0;
    return 0;
}

static inline void kv_page_mmap_destroy(KVPageStore *s) {
    if (!s->mmap_base) return;
#ifdef _WIN32
    VirtualFree(s->mmap_base, 0, MEM_RELEASE);
#else
    if (s->is_mmap)
        munmap(s->mmap_base, s->mmap_capacity);
    else
        free(s->mmap_base);
#endif
    s->mmap_base = NULL;
    s->mmap_capacity = 0;
    s->mmap_used = 0;
}


/* =============================================================
 * Low-level: compress / decompress (reuse Binary Shell Codec)
 * ============================================================= */

static inline int kv_page_compress(const void *data, size_t size,
                                   void **out, size_t *out_size)
{
    uint64_t n_chunks = (size + 63) / 64;
    uint64_t max_enc  = n_chunks * 70 + 16;
    uint8_t *enc = (uint8_t *)malloc((size_t)max_enc);
    if (!enc) return -1;

    size_t enc_pos = 16; /* skip header */

    for (uint64_t ci = 0; ci < n_chunks; ci++) {
        size_t off = (size_t)(ci * 64);
        size_t remain = size > off ? size - off : 0;
        uint8_t chunk64[64];
        memset(chunk64, 0, 64);
        memcpy(chunk64, (const uint8_t *)data + off,
               remain < 64 ? remain : 64);
        enc_pos += bin_encode_chunk(enc + enc_pos, chunk64, &(BinChunkResult){0});
    }

    *(uint32_t *)(enc + 0)  = KV_PAGE_MAGIC;
    *(uint64_t *)(enc + 4)  = (uint64_t)size;
    *(uint32_t *)(enc + 12) = (uint32_t)n_chunks;

    size_t total = enc_pos;
    double ratio = (double)size / (double)(total > 16 ? total - 16 : 1);

    /* Fallback: store uncompressed if ratio < 1.1 */
    if (total >= size || ratio < 1.1) {
        free(enc);
        uint8_t *raw_out = (uint8_t *)malloc(16 + size);
        if (!raw_out) return -1;
        *(uint32_t *)(raw_out + 0) = KV_PAGE_MAGIC;
        *(uint64_t *)(raw_out + 4) = (uint64_t)size;
        *(uint32_t *)(raw_out + 12) = 0; /* n_chunks=0 → uncompressed */
        memcpy(raw_out + 16, data, size);
        *out = raw_out;
        *out_size = 16 + size;
        return 1; /* KV_SKIPPED */
    }

    *out = enc;
    *out_size = total;
    return 0; /* KV_OK */
}

static inline void *kv_page_decompress(const void *compressed, size_t comp_size,
                                       size_t *out_size)
{
    if (comp_size < 16) return NULL;
    const uint8_t *enc = (const uint8_t *)compressed;

    uint32_t magic = *(const uint32_t *)(enc + 0);
    if (magic != KV_PAGE_MAGIC) return NULL;

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
    size_t comp_pos = 16;

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
 * POGLS address helpers (page_id encode/decode for visualization)
 * ============================================================= */

/* Encode page_id to a 64-bit address (base62-compatible).
 * Format: page_id in lower 32 bits, magic in upper bits. */
static inline uint64_t kv_page_pogls_addr(int page_id) {
    return ((uint64_t)0x5047 << 32) | (uint32_t)page_id;
}


/* =============================================================
 * Init
 * ============================================================= */

static inline int kv_page_init(KVPageStore *s, int n_ctx) {
    memset(s, 0, sizeof(*s));
    s->n_ctx = n_ctx;
    s->n_pages = n_ctx / KV_PAGE_SIZE;
    if (s->n_pages > KV_PAGE_MAX_PAGES) s->n_pages = KV_PAGE_MAX_PAGES;

    /* Slot size = uncompressed page size per layer × n_attn_layers
     * We don't know n_attn_layers yet, so allocate generous max:
     * 64 layers × 2 (K+V) × 128 tokens × 512 embd × 2 bytes = 16 MB per slot
     * That's too big. Instead, compute after registration.
     * For now, set slot_size to 0; kv_page_register_layers() will finalize. */
    s->slot_size = 0;
    s->n_attn_layers = 0;
    s->enabled = 0;

    fprintf(stderr, "[kv-page] init: n_ctx=%d, n_pages=%d, page_size=%d\n",
        n_ctx, s->n_pages, KV_PAGE_SIZE);
    return 0;
}


/* =============================================================
 * Registration — call after KV tensors are available
 * ============================================================= */

static inline void kv_page_register_layers(KVPageStore *s,
    void **k_tensors, void **v_tensors,
    void **k_data, void **v_data,
    size_t *k_nb1, size_t *v_nb1,
    size_t *k_size, size_t *v_size,
    int *n_embd_k, int *layer_id,
    int n_layers)
{
    if (n_layers > KV_PAGE_MAX_LAYERS) n_layers = KV_PAGE_MAX_LAYERS;
    s->n_attn_layers = n_layers;

    for (int i = 0; i < n_layers; i++) {
        s->k_tensors[i] = k_tensors[i];
        s->v_tensors[i] = v_tensors[i];
        s->k_data[i]    = k_data[i];
        s->v_data[i]    = v_data[i];
        s->k_nb1[i]     = k_nb1[i];
        s->v_nb1[i]     = v_nb1[i];
        s->k_size[i]    = k_size[i];
        s->v_size[i]    = v_size[i];
        s->n_embd_k[i]  = n_embd_k[i];
        s->layer_id[i]  = layer_id[i];
    }

    /* Compute slot size: one page = PAGE_SIZE × nb[1] per layer per direction */
    size_t page_bytes = 0;
    for (int i = 0; i < n_layers; i++) {
        page_bytes += (size_t)KV_PAGE_SIZE * s->k_nb1[i];  /* K page */
        page_bytes += (size_t)KV_PAGE_SIZE * s->v_nb1[i];  /* V page */
    }
    /* Align to cache line */
    s->slot_size = (page_bytes + KV_PAGE_SLOT_ALIGN - 1) & ~(size_t)(KV_PAGE_SLOT_ALIGN - 1);
    s->slot_size += KV_PAGE_SLOT_META;  /* add header space */

    /* Total mmap needed */
    size_t total_mmap = (size_t)s->n_pages * s->slot_size;
    /* Add 25% headroom */
    total_mmap = total_mmap + total_mmap / 4;
    if (total_mmap < 4u * 1024 * 1024) total_mmap = 4u * 1024 * 1024;

    if (kv_page_mmap_init(s, total_mmap) != 0) {
        fprintf(stderr, "[kv-page] mmap init failed!\n");
        return;
    }

    s->total_orig_bytes = (size_t)s->n_pages * page_bytes;
    s->enabled = 1;

    fprintf(stderr, "[kv-page] registered %d attn layers, %d pages, "
        "page_bytes=%zu, slot_size=%zu, mmap=%zu bytes (%.1f MB)\n",
        n_layers, s->n_pages, page_bytes, s->slot_size, total_mmap,
        (double)total_mmap / (1024.0 * 1024.0));
}


/* =============================================================
 * Core: extract page from live KV cache → compress → store in mmap
 * ============================================================= */

/* Get pointer to page slot in mmap */
static inline uint8_t *kv_page_slot_ptr(KVPageStore *s, int page_id) {
    return s->mmap_base + (size_t)page_id * s->slot_size;
}

/* Snapshot a single page: compress K+V for positions [start, end) across all attn layers
 * and store in mmap slot.  Returns 0 on success. */
static inline int kv_page_snapshot(KVPageStore *s, int page_id) {
    if (!s->enabled || page_id < 0 || page_id >= s->n_pages) return -1;
    if (s->page_valid[page_id]) return 0;  /* already valid */

    int start_pos = page_id * KV_PAGE_SIZE;
    int end_pos   = start_pos + KV_PAGE_SIZE;
    if (end_pos > s->n_ctx) end_pos = s->n_ctx;
    int page_tokens = end_pos - start_pos;
    if (page_tokens <= 0) return -1;

    /* Build contiguous page buffer: [K_l0, V_l0, K_l1, V_l1, ...] */
    size_t page_total = 0;
    for (int l = 0; l < s->n_attn_layers; l++) {
        page_total += (size_t)page_tokens * s->k_nb1[l];  /* K */
        page_total += (size_t)page_tokens * s->v_nb1[l];  /* V */
    }

    uint8_t *page_buf = (uint8_t *)malloc(page_total);
    if (!page_buf) return -1;

    size_t off = 0;
    for (int l = 0; l < s->n_attn_layers; l++) {
        uint8_t *kbase = (uint8_t *)s->k_data[l];
        uint8_t *vbase = (uint8_t *)s->v_data[l];
        /* K page: contiguous block at kbase + start_pos * nb[1] */
        memcpy(page_buf + off, kbase + start_pos * s->k_nb1[l],
               (size_t)page_tokens * s->k_nb1[l]);
        off += (size_t)page_tokens * s->k_nb1[l];
        /* V page */
        memcpy(page_buf + off, vbase + start_pos * s->v_nb1[l],
               (size_t)page_tokens * s->v_nb1[l]);
        off += (size_t)page_tokens * s->v_nb1[l];
    }

    /* Compress */
    void *comp = NULL;
    size_t comp_size = 0;
    int r = kv_page_compress(page_buf, page_total, &comp, &comp_size);
    free(page_buf);

    if (r < 0 || !comp) return -1;

    /* Check slot has room */
    if (KV_PAGE_SLOT_META + comp_size > s->slot_size) {
        free(comp);
        fprintf(stderr, "[kv-page] page %d: compressed %zu > slot %zu, skip\n",
            page_id, comp_size + KV_PAGE_SLOT_META, s->slot_size);
        return -1;
    }

    /* Write to mmap slot */
    uint8_t *slot = kv_page_slot_ptr(s, page_id);
    *(uint32_t *)(slot + 0) = KV_PAGE_MAGIC;
    *(uint32_t *)(slot + 4) = (uint32_t)comp_size;
    *(uint32_t *)(slot + 8) = (uint32_t)page_total;
    memcpy(slot + KV_PAGE_SLOT_META, comp, comp_size);

    free(comp);

    s->page_valid[page_id] = 1;
    s->page_comp_size[page_id] = comp_size;
    s->page_lru[page_id] = ++s->lru_tick;
    s->total_snap_bytes += comp_size;

    double ratio = (double)page_total / (double)(comp_size > 0 ? comp_size : 1);
    fprintf(stderr, "[kv-page] snap page %d [pos %d..%d]: orig=%zu comp=%zu ratio=%.2fx\n",
        page_id, start_pos, end_pos - 1, page_total, comp_size, ratio);
    return 0;
}


/* =============================================================
 * Core: evict page — snapshot + zero out live positions
 * ============================================================= */

static inline int kv_page_evict(KVPageStore *s, int page_id) {
    if (!s->enabled || page_id < 0 || page_id >= s->n_pages) return -1;
    if (s->page_evicted[page_id]) return 0;

    /* Snapshot first if not already valid */
    if (!s->page_valid[page_id]) {
        if (kv_page_snapshot(s, page_id) != 0) return -1;
    }

    /* Zero out positions in live KV cache */
    int start_pos = page_id * KV_PAGE_SIZE;
    int end_pos   = start_pos + KV_PAGE_SIZE;
    if (end_pos > s->n_ctx) end_pos = s->n_ctx;
    int page_tokens = end_pos - start_pos;

    for (int l = 0; l < s->n_attn_layers; l++) {
        uint8_t *kbase = (uint8_t *)s->k_data[l];
        uint8_t *vbase = (uint8_t *)s->v_data[l];
        memset(kbase + start_pos * s->k_nb1[l], 0,
               (size_t)page_tokens * s->k_nb1[l]);
        memset(vbase + start_pos * s->v_nb1[l], 0,
               (size_t)page_tokens * s->v_nb1[l]);
    }

    s->page_evicted[page_id] = 1;
    s->n_evicted++;

    fprintf(stderr, "[kv-page] evict page %d [pos %d..%d]: zeroed %d positions\n",
        page_id, start_pos, end_pos - 1, page_tokens);
    return 0;
}


/* =============================================================
 * Core: restore page — decompress from mmap → write back to live cache
 * ============================================================= */

static inline int kv_page_restore(KVPageStore *s, int page_id) {
    if (!s->enabled || page_id < 0 || page_id >= s->n_pages) return -1;
    if (!s->page_valid[page_id]) return -1;
    if (!s->page_evicted[page_id]) return 0;  /* already live */

    /* Read compressed data from mmap slot */
    uint8_t *slot = kv_page_slot_ptr(s, page_id);
    uint32_t magic = *(uint32_t *)(slot + 0);
    if (magic != KV_PAGE_MAGIC) return -1;

    uint32_t comp_size = *(uint32_t *)(slot + 4);
    uint32_t orig_size = *(uint32_t *)(slot + 8);

    /* Decompress */
    size_t dec_size = 0;
    void *dec = kv_page_decompress(slot + KV_PAGE_SLOT_META, comp_size, &dec_size);
    if (!dec) return -1;

    /* Verify size */
    size_t expected = 0;
    int start_pos = page_id * KV_PAGE_SIZE;
    int end_pos   = start_pos + KV_PAGE_SIZE;
    if (end_pos > s->n_ctx) end_pos = s->n_ctx;
    int page_tokens = end_pos - start_pos;

    for (int l = 0; l < s->n_attn_layers; l++) {
        expected += (size_t)page_tokens * s->k_nb1[l];
        expected += (size_t)page_tokens * s->v_nb1[l];
    }

    if (dec_size < expected) {
        free(dec);
        return -1;
    }

    /* Write back to live KV cache */
    size_t off = 0;
    for (int l = 0; l < s->n_attn_layers; l++) {
        uint8_t *kbase = (uint8_t *)s->k_data[l];
        uint8_t *vbase = (uint8_t *)s->v_data[l];
        memcpy(kbase + start_pos * s->k_nb1[l],
               (uint8_t *)dec + off,
               (size_t)page_tokens * s->k_nb1[l]);
        off += (size_t)page_tokens * s->k_nb1[l];
        memcpy(vbase + start_pos * s->v_nb1[l],
               (uint8_t *)dec + off,
               (size_t)page_tokens * s->v_nb1[l]);
        off += (size_t)page_tokens * s->v_nb1[l];
    }

    free(dec);

    s->page_evicted[page_id] = 0;
    s->n_evicted--;
    s->page_lru[page_id] = ++s->lru_tick;

    fprintf(stderr, "[kv-page] restore page %d [pos %d..%d]: wrote back %d positions\n",
        page_id, start_pos, end_pos - 1, page_tokens);
    return 0;
}


/* =============================================================
 * LRU eviction — find oldest page and evict it
 * ============================================================= */

static inline int kv_page_find_lru(KVPageStore *s) {
    int best = -1;
    uint32_t best_tick = UINT32_MAX;
    for (int i = 0; i < s->n_pages; i++) {
        if (s->page_evicted[i]) continue;     /* already evicted */
        if (!s->page_valid[i]) continue;      /* never snapshotted */
        if (s->page_lru[i] < best_tick) {
            best_tick = s->page_lru[i];
            best = i;
        }
    }
    return best;
}

/* Evict oldest N pages */
static inline int kv_page_evict_oldest(KVPageStore *s, int n) {
    int evicted = 0;
    for (int i = 0; i < n; i++) {
        int lru = kv_page_find_lru(s);
        if (lru < 0) break;
        if (kv_page_evict(s, lru) == 0) evicted++;
    }
    fprintf(stderr, "[kv-page] evicted %d pages (total evicted: %d)\n",
        evicted, s->n_evicted);
    return evicted;
}


/* =============================================================
 * Access page — restore if evicted, update LRU
 * ============================================================= */

static inline int kv_page_access(KVPageStore *s, int page_id) {
    if (!s->enabled || page_id < 0 || page_id >= s->n_pages) return -1;

    if (s->page_evicted[page_id]) {
        return kv_page_restore(s, page_id);
    }

    /* Update LRU */
    s->page_lru[page_id] = ++s->lru_tick;
    return 0;
}


/* =============================================================
 * Snapshot all pages (initial backup)
 * ============================================================= */

static inline int kv_page_snapshot_all(KVPageStore *s) {
    int errs = 0;
    for (int i = 0; i < s->n_pages; i++) {
        if (kv_page_snapshot(s, i) != 0) errs++;
    }
    fprintf(stderr, "[kv-page] snapshot all: %d pages, total_snap=%zu bytes (errs=%d)\n",
        s->n_pages, s->total_snap_bytes, errs);
    return errs == 0 ? 0 : -1;
}


/* =============================================================
 * Restore all evicted pages
 * ============================================================= */

static inline int kv_page_restore_all(KVPageStore *s) {
    int errs = 0;
    for (int i = s->n_pages - 1; i >= 0; i--) {
        if (s->page_evicted[i]) {
            if (kv_page_restore(s, i) != 0) errs++;
        }
    }
    fprintf(stderr, "[kv-page] restore all: 0 evicted (errs=%d)\n", errs);
    return errs == 0 ? 0 : -1;
}


/* =============================================================
 * Print status
 * ============================================================= */

static inline void kv_page_print_status(const KVPageStore *s) {
    double ratio = s->total_snap_bytes > 0 ?
        (double)s->total_orig_bytes / (double)s->total_snap_bytes : 0;
    fprintf(stderr, "[kv-page] status: %d pages, %d evicted, "
        "orig=%zu snap=%zu ratio=%.2fx, slot=%zu mmap=%zu\n",
        s->n_pages, s->n_evicted,
        s->total_orig_bytes, s->total_snap_bytes, ratio,
        s->slot_size, s->mmap_capacity);
}


/* =============================================================
 * Cleanup
 * ============================================================= */

static inline void kv_page_destroy(KVPageStore *s) {
    kv_page_mmap_destroy(s);
    fprintf(stderr, "[kv-page] destroyed\n");
}


#endif /* KV_PAGE_STORE_H */

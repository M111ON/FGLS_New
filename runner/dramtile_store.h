#ifndef DRAMTILE_STORE_H
#define DRAMTILE_STORE_H

/*
 * DRamTile store — zero-copy geometry-addressed tensor store
 *
 * Maps tensor data into a large mmap'd DRAM region addressed by the
 * geo_dram_tile Hilbert-curve scheme.  Each tensor's offset is
 * deterministic:  dram_addr(name) → hash → slot with O(1) lookup.
 *
 * On swap: tensor->data = dt_ptr → zero-copy pointer into mmap.
 * No memcpy of weight data at swap time — only pointer exchange.
 *
 * Backed by one large mmap (or VirtualAlloc on Windows) so the OS
 * can lazily page-fault the data in.  Deterministic placement means
 * the same tensor always lands at the same address → no WAL needed.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
#endif

#include "geo_dram_tile.h"

/* Hash table size — must be > max expected tensors (~300 for 8B) */
#define DT_HASH_SLOTS  512

typedef struct {
    uint32_t dram_addr;   /* 0..DRAM_FULL-1, 0 = unused */
    size_t   offset;      /* byte offset in mmap_base */
    size_t   size;        /* stored bytes */
} DRamTileHashEntry;

typedef struct {
    uint8_t          *base;         /* mmap/VirtualAlloc base */
    size_t            capacity;     /* total mapped bytes */
    size_t            used;         /* pool_used bytes */
    int               is_mmap;      /* 1 if VirtualAlloc, 0 if heap fallback */
    DRamTileHashEntry hash[DT_HASH_SLOTS];
    uint32_t          n_stored;
} DRamTileStore;

/* Compute dram_addr from tensor name via FNV-1a → anchor/x/y/layer */
static inline uint32_t dt_name_to_addr(const char *name) {
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    uint32_t anchor = h % DRAM_ANCHORS;
    uint32_t x = (h >> 8) % DRAM_GRID_X;
    uint32_t y = (h >> 16) % DRAM_GRID_Y;
    uint32_t layer = (h >> 24) % DRAM_LAYERS;
    return dram_addr(anchor, x, y, layer);
}

/* Initialize DRamTile store with a large anonymous mmap */
static inline int dt_store_init(DRamTileStore *store, size_t min_bytes) {
    memset(store, 0, sizeof(*store));
    size_t cap = min_bytes < 4UL * 1024 * 1024 * 1024
               ? 4UL * 1024 * 1024 * 1024   /* 4GB default */
               : min_bytes;
#ifdef _WIN32
    store->base = (uint8_t*)VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    store->is_mmap = (store->base != NULL);
#else
    store->base = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    store->is_mmap = (store->base != MAP_FAILED);
    if (!store->is_mmap) store->base = NULL;
#endif
    if (!store->base) {
        store->base = (uint8_t*)malloc(cap);
        if (!store->base) return -1;
        store->is_mmap = 0;
    }
    store->capacity = cap;
    store->used = 0;
    return 0;
}

/* Store tensor data and return zero-copy pointer.
 * Returns NULL on failure (falls through to caller for heap fallback). */
static inline uint8_t *dt_put(DRamTileStore *store,
                               const char *name,
                               const uint8_t *data, size_t sz) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;

    if (store->hash[slot].dram_addr == addr) {
        size_t off = store->hash[slot].offset;
        if (store->hash[slot].size != sz) return NULL;
        memcpy(store->base + off, data, sz);
        return store->base + off;
    }

    /* Align to 64 bytes for cache-line friendliness */
    size_t off = (store->used + 63) & ~63;
    if (off + sz > store->capacity) return NULL;
    memcpy(store->base + off, data, sz);
    store->used = off + sz;

    store->hash[slot].dram_addr = addr;
    store->hash[slot].offset    = off;
    store->hash[slot].size      = sz;
    store->n_stored++;
    return store->base + off;
}

/* Lookup tensor data by name — O(1) hash lookup, returns direct pointer. */
static inline uint8_t *dt_get(DRamTileStore *store, const char *name) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    if (store->hash[slot].dram_addr == addr)
        return store->base + store->hash[slot].offset;
    return NULL;
}

static inline size_t dt_get_size(DRamTileStore *store, const char *name) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    if (store->hash[slot].dram_addr == addr)
        return store->hash[slot].size;
    return 0;
}

/* Destroy DRamTile store */
static inline void dt_store_destroy(DRamTileStore *store) {
    if (!store || !store->base) return;
#ifdef _WIN32
    VirtualFree(store->base, 0, MEM_RELEASE);
#else
    if (store->is_mmap)
        munmap(store->base, store->capacity);
    else
        free(store->base);
#endif
    memset(store, 0, sizeof(*store));
}

#endif /* DRAMTILE_STORE_H */

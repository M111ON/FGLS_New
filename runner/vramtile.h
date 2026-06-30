#pragma once
#ifndef VRAMTILE_H
#define VRAMTILE_H

#include "dramtile_store.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define VRT_MAX_SLOTS      DT_HASH_SLOTS
#define VRT_DEFAULT_VRAM   (256UL << 20)

#define VRT_CPU_ONLY   0
#define VRT_IN_VRAM    1

typedef struct {
    uint32_t  state;
    uint32_t  vram_offset;
    size_t    size;
    uint32_t  access_tick;
    uint32_t  flags;
    uint32_t  dram_addr;
} VRamEntry;

typedef struct {
    DRamTileStore  store;
    DRamTileStore *external_src;   /* if set, read CPU data from here instead of store */
    uint8_t       *vram_base;
    size_t         vram_capacity;
    size_t         vram_used;
    VRamEntry      vram_hash[VRT_MAX_SLOTS];
    int            vram_is_gpu;
    uint32_t       tick;
    uint32_t       n_promoted;
    uint32_t       n_evicted;
    uint32_t       n_uploads;
} VRamTileStore;

typedef int (*VramUploadFn)(void *gpu_dst, const void *cpu_src, size_t sz, void *user);

/* ── Init ────────────────────────────────────────────────── */

static inline int vrt_init_twin(VRamTileStore *vrt,
                                 const char *filepath, size_t max_bytes,
                                 size_t vram_bytes, int is_gpu)
{
    memset(vrt, 0, sizeof(*vrt));
    int r = filepath
        ? dt_store_init_twin(&vrt->store, filepath, max_bytes)
        : dt_store_init(&vrt->store, max_bytes);
    if (r != 0) return r;

    vrt->vram_capacity = vram_bytes > 0 ? vram_bytes : VRT_DEFAULT_VRAM;
#ifdef _WIN32
    vrt->vram_base = (uint8_t*)VirtualAlloc(NULL, vrt->vram_capacity,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    vrt->vram_base = (uint8_t*)mmap(NULL, vrt->vram_capacity,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (!vrt->vram_base) { dt_store_destroy(&vrt->store); return -1; }
    vrt->vram_is_gpu = is_gpu;
    return 0;
}

/* ── Init with external DRamTile source (no internal store) ─ */
static inline int vrt_init_external(VRamTileStore *vrt,
                                     DRamTileStore *external,
                                     size_t vram_bytes, int is_gpu)
{
    memset(vrt, 0, sizeof(*vrt));
    vrt->external_src = external;
    vrt->vram_capacity = vram_bytes > 0 ? vram_bytes : VRT_DEFAULT_VRAM;
#ifdef _WIN32
    vrt->vram_base = (uint8_t*)VirtualAlloc(NULL, vrt->vram_capacity,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    vrt->vram_base = (uint8_t*)mmap(NULL, vrt->vram_capacity,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (!vrt->vram_base) return -1;
    vrt->vram_is_gpu = is_gpu;
    return 0;
}

/* ── Query ───────────────────────────────────────────────── */

static inline int vrt_is_in_vram(VRamTileStore *vrt, const char *name) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % VRT_MAX_SLOTS;
    return (vrt->vram_hash[slot].state == VRT_IN_VRAM &&
            vrt->vram_hash[slot].dram_addr == addr);
}

static inline uint8_t *vrt_get_vram_ptr(VRamTileStore *vrt, const char *name) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % VRT_MAX_SLOTS;
    VRamEntry *ve = &vrt->vram_hash[slot];
    if (ve->state == VRT_IN_VRAM && ve->dram_addr == addr) {
        ve->access_tick = ++vrt->tick;
        return vrt->vram_base + ve->vram_offset;
    }
    return NULL;
}

static inline uint8_t *vrt_get_cpu_ptr(VRamTileStore *vrt, const char *name) {
    DRamTileStore *src = vrt->external_src ? vrt->external_src : &vrt->store;
    return dt_get(src, name);
}

static inline uint8_t *vrt_get_ptr(VRamTileStore *vrt, const char *name) {
    uint8_t *vp = vrt_get_vram_ptr(vrt, name);
    if (vp) return vp;
    return vrt_get_cpu_ptr(vrt, name);
}

static inline size_t vrt_get_size(VRamTileStore *vrt, const char *name) {
    DRamTileStore *src = vrt->external_src ? vrt->external_src : &vrt->store;
    return dt_get_size(src, name);
}

/* ── Internal: free-vram-chunk tracking (linked list) ──────
 *   Solves fragmentation from bump-alloc + partial eviction.
 *   VramChunk is stored inline in vram_base (no extra alloc).
 */
typedef struct VramChunk {
    struct VramChunk *next;
    size_t            offset;
    size_t            size;
} VramChunk;

/* Walk the VRAM hash to reconstruct free list */
static inline void vrt_rebuild_free(VRamTileStore *vrt) {
    /* Mark all VRAM as one free chunk, then subtract allocated ranges.
       Simpler: just reset vram_used and rebuild. For defrag, call vrt_compact(). */
}

/* ── Compact VRAM: defragment allocated entries ────────────
 *   Moves all live VRAM entries to the front, closes holes.
 *   Updates vram_hash offsets accordingly.
 *   Returns number of bytes reclaimed.
 *
 *   O(n) where n = live entries (typically < 300).
 *   Call when eviction count exceeds threshold.
 */
static inline size_t vrt_compact(VRamTileStore *vrt) {
    /* Collect live entries */
    typedef struct { int slot; size_t sz; uint32_t off; } LiveEntry;
    LiveEntry live[VRT_MAX_SLOTS];
    int n_live = 0;

    for (int i = 0; i < VRT_MAX_SLOTS; i++) {
        if (vrt->vram_hash[i].state == VRT_IN_VRAM) {
            live[n_live].slot = i;
            live[n_live].sz = vrt->vram_hash[i].size;
            live[n_live].off = vrt->vram_hash[i].vram_offset;
            n_live++;
        }
    }

    if (n_live == 0) {
        size_t old = vrt->vram_used;
        vrt->vram_used = 0;
        return old;
    }

    /* Sort by offset (bubble sort, n_live small) */
    for (int i = 0; i < n_live - 1; i++) {
        for (int j = 0; j < n_live - 1 - i; j++) {
            if (live[j].off > live[j+1].off) {
                LiveEntry t = live[j]; live[j] = live[j+1]; live[j+1] = t;
            }
        }
    }

    /* Compact: move each entry to its ideal position */
    size_t cur = 0;
    size_t reclaimed = 0;
    for (int i = 0; i < n_live; i++) {
        size_t aligned = (cur + 63) & ~63;
        if (aligned != live[i].off) {
            memmove(vrt->vram_base + aligned,
                    vrt->vram_base + live[i].off,
                    live[i].sz);
            vrt->vram_hash[live[i].slot].vram_offset = (uint32_t)aligned;
        }
        cur = aligned + live[i].sz;
    }
    size_t new_used = (cur + 63) & ~63;
    reclaimed = vrt->vram_used > new_used ? vrt->vram_used - new_used : 0;
    vrt->vram_used = new_used;
    return reclaimed;
}

/* ── LRU find ────────────────────────────────────────────── */

static inline int vrt_find_lru(VRamTileStore *vrt) {
    int lru_slot = -1;
    uint32_t oldest_tick = UINT32_MAX;
    for (int i = 0; i < VRT_MAX_SLOTS; i++) {
        if (vrt->vram_hash[i].state == VRT_IN_VRAM &&
            vrt->vram_hash[i].access_tick < oldest_tick) {
            oldest_tick = vrt->vram_hash[i].access_tick;
            lru_slot = i;
        }
    }
    return lru_slot;
}

/* ── Evict one (LRU) ──────────────────────────────────────── */
static inline int vrt_evict_one(VRamTileStore *vrt) {
    int slot = vrt_find_lru(vrt);
    if (slot < 0) return 0;

    VRamEntry *ve = &vrt->vram_hash[slot];
    size_t sz = ve->size;
    uint32_t off = ve->vram_offset;

    ve->state = VRT_CPU_ONLY;
    ve->dram_addr = 0;
    ve->vram_offset = 0;
    ve->size = 0;
    ve->access_tick = 0;

    vrt->n_evicted++;
    vrt->n_promoted--;

    /* Zero the evicted VRAM region (helps detect stale GPU reads) */
    memset(vrt->vram_base + off, 0xFE, sz);

    /* vram_used is not adjusted here — instead we track the hole.
       The next alloc will either fit in a hole or we compact.
       For simplicity, reduce vram_used only if hole is at end. */
    if (off + sz >= vrt->vram_used)
        vrt->vram_used = off;
    return 1;
}

/* ── Make room for sz bytes ──────────────────────────────── */
static inline int vrt_make_room(VRamTileStore *vrt, size_t sz) {
    /* Pre-compact if many evictions happened */
    if (vrt->n_evicted > 16) {
        vrt_compact(vrt);
    }

    /* Try simple bump-first: if space at end, no eviction */
    size_t aligned = (vrt->vram_used + 63) & ~63;
    if (aligned + sz <= vrt->vram_capacity)
        return 0; /* no eviction needed */

    /* Evict oldest until sz fits */
    int evicted = 0;
    while (1) {
        size_t cur_aligned = (vrt->vram_used + 63) & ~63;
        if (cur_aligned + sz <= vrt->vram_capacity) break;
        if (!vrt_evict_one(vrt)) {
            /* Nothing left to evict — try compact (may close holes) */
            size_t reclaimed = vrt_compact(vrt);
            if (reclaimed == 0) return -1; /* cannot fit even after full evict+compact */
        }
        evicted++;
    }
    return evicted;
}

/* ── VRAM alloc (64-byte aligned bump, with compact fallback) */
static inline uint8_t *vrt_vram_alloc(VRamTileStore *vrt, size_t sz) {
    size_t aligned = (vrt->vram_used + 63) & ~63;
    if (aligned + sz > vrt->vram_capacity) {
        int r = vrt_make_room(vrt, sz);
        if (r < 0) return NULL;
        aligned = (vrt->vram_used + 63) & ~63;
    }
    if (aligned + sz > vrt->vram_capacity) return NULL; /* still doesn't fit */
    vrt->vram_used = aligned + sz;
    return vrt->vram_base + aligned;
}

/* ── Promote ──────────────────────────────────────────────── */

static inline uint8_t *vrt_promote(VRamTileStore *vrt,
                                    const char *name,
                                    VramUploadFn upload_fn,
                                    void *upload_user)
{
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % VRT_MAX_SLOTS;
    VRamEntry *ve = &vrt->vram_hash[slot];

    DRamTileStore *src_store = vrt->external_src ? vrt->external_src : &vrt->store;
    uint8_t *cpu_ptr = dt_get(src_store, name);
    if (!cpu_ptr) return NULL;
    size_t sz = dt_get_size(src_store, name);
    if (sz == 0) return NULL;

    if (ve->state == VRT_IN_VRAM && ve->dram_addr == addr) {
        ve->access_tick = ++vrt->tick;
        if (upload_fn)
            upload_fn(vrt->vram_base + ve->vram_offset, cpu_ptr, sz, upload_user);
        else
            memcpy(vrt->vram_base + ve->vram_offset, cpu_ptr, sz);
        return vrt->vram_base + ve->vram_offset;
    }

    if (ve->state == VRT_IN_VRAM) {
        /* Hash collision: different name, same slot — free old */
        memset(vrt->vram_base + ve->vram_offset, 0xFE, ve->size);
        vrt->n_promoted--;
    }

    uint8_t *vram_dst = vrt_vram_alloc(vrt, sz);
    if (!vram_dst) return NULL;

    if (upload_fn) {
        if (upload_fn(vram_dst, cpu_ptr, sz, upload_user) != 0) return NULL;
    } else {
        memcpy(vram_dst, cpu_ptr, sz);
    }

    ve->state = VRT_IN_VRAM;
    ve->vram_offset = (uint32_t)(vram_dst - vrt->vram_base);
    ve->size = sz;
    ve->access_tick = ++vrt->tick;
    ve->dram_addr = addr;
    ve->flags = vrt->store.hash[slot].dram_addr & DT_FLAGS_MASK;

    vrt->n_promoted++;
    vrt->n_uploads++;
    return vram_dst;
}

/* ── Batch promote by name list ──────────────────────────── */
static inline int vrt_promote_names(VRamTileStore *vrt,
                                     const char **names, int n_names,
                                     VramUploadFn upload_fn,
                                     void *upload_user)
{
    int promoted = 0;
    for (int i = 0; i < n_names; i++) {
        if (vrt_promote(vrt, names[i], upload_fn, upload_user))
            promoted++;
    }
    return promoted;
}

/* ── Evict all ────────────────────────────────────────────── */

static inline void vrt_evict_all(VRamTileStore *vrt) {
    for (int i = 0; i < VRT_MAX_SLOTS; i++) {
        if (vrt->vram_hash[i].state == VRT_IN_VRAM) {
            memset(vrt->vram_base + vrt->vram_hash[i].vram_offset, 0xFE,
                   vrt->vram_hash[i].size);
            vrt->vram_hash[i].state = VRT_CPU_ONLY;
            vrt->vram_hash[i].dram_addr = 0;
            vrt->vram_hash[i].vram_offset = 0;
            vrt->vram_hash[i].size = 0;
            vrt->n_evicted++;
        }
    }
    vrt->n_promoted = 0;
    vrt->vram_used = 0;
}

/* ── Upload callback (memcpy simulation) ──────────────────── */

static inline int vrt_upload_memcpy(void *gpu_dst, const void *cpu_src,
                                     size_t sz, void *user)
{
    (void)user;
    memcpy(gpu_dst, cpu_src, sz);
    return 0;
}

/* ── Destroy ──────────────────────────────────────────────── */

static inline void vrt_destroy(VRamTileStore *vrt) {
    if (vrt->vram_base) {
#ifdef _WIN32
        VirtualFree(vrt->vram_base, 0, MEM_RELEASE);
#else
        munmap(vrt->vram_base, vrt->vram_capacity);
#endif
    }
    if (!vrt->external_src) {
        if (vrt->store.is_twin)
            dt_store_destroy_twin(&vrt->store);
        else
            dt_store_destroy(&vrt->store);
    }
    memset(vrt, 0, sizeof(*vrt));
}

/* ── Stats ────────────────────────────────────────────────── */

static inline void vrt_stats(const VRamTileStore *vrt, FILE *fp) {
    const DRamTileStore *src = vrt->external_src ? vrt->external_src : &vrt->store;
    fprintf(fp, "=== VRamTile ===\n");
    fprintf(fp, "  DRamTile: %zu/%zu used (%.1f%%), %u tensors\n",
            src->used, src->capacity,
            100.0 * src->used / (src->capacity ? src->capacity : 1),
            src->n_stored);
    fprintf(fp, "  External source: %s\n", vrt->external_src ? "yes" : "no");
    fprintf(fp, "  VRAM:     %zu/%zu used (%.1f%%), %u promoted, %u evictions\n",
            vrt->vram_used, vrt->vram_capacity,
            100.0 * vrt->vram_used / (vrt->vram_capacity ? vrt->vram_capacity : 1),
            vrt->n_promoted, vrt->n_evicted);
    fprintf(fp, "  Uploads:  %u\n", vrt->n_uploads);
    if (vrt->store.is_twin)
        fprintf(fp, "  Twin:     %s\n", vrt->store.filepath);
    fprintf(fp, "  GPU mode: %s\n", vrt->vram_is_gpu ? "real GPU" : "simulation");
}

/* ── Post-SID-swap promote ───────────────────────────────── */

static inline uint8_t *vrt_promote_postswap(VRamTileStore *vrt,
                                              const char *name,
                                              VramUploadFn upload_fn,
                                              void *upload_user)
{
    return vrt_promote(vrt, name, upload_fn, upload_user);
}

/* ── Gear-aware VRAM eviction (wired to GearLock) ──────────
 *   gpu_worlds_source: pointer to gear lock's gpu_worlds counter.
 *   Only evicts entries whose access_tick maps to completed GPU worlds.
 */

/* GearLock watermarks: CPU ops in [0, 127] = world 0, etc.
   We derive a safe-min-age from gpu_worlds:
   entry is safe if:  access_tick  <  gpu_worlds * 128   */
static inline int vrt_evict_gear(VRamTileStore *vrt,
                                  const uint32_t *gpu_worlds_ptr)
{
    if (!gpu_worlds_ptr) return vrt_evict_all(vrt), 0;

    uint32_t gpu_w = *gpu_worlds_ptr;
    uint32_t safe_upto = gpu_w * 128; /* 128 CPU ops per world */

    int evicted = 0;
    for (int pass = 0; pass < 32; pass++) {
        int lru_slot = -1;
        uint32_t oldest = UINT32_MAX;

        for (int i = 0; i < VRT_MAX_SLOTS; i++) {
            if (vrt->vram_hash[i].state != VRT_IN_VRAM) continue;
            uint32_t t = vrt->vram_hash[i].access_tick;
            if (t >= safe_upto) continue; /* GPU hasn't caught up */
            if (t < oldest) { oldest = t; lru_slot = i; }
        }
        if (lru_slot < 0) break;

        VRamEntry *ve = &vrt->vram_hash[lru_slot];
        memset(vrt->vram_base + ve->vram_offset, 0xFE, ve->size);
        if (ve->vram_offset + ve->size >= vrt->vram_used)
            vrt->vram_used = ve->vram_offset;
        ve->state = VRT_CPU_ONLY;
        ve->dram_addr = 0;
        vrt->n_evicted++;
        vrt->n_promoted--;
        evicted++;
    }
    return evicted;
}

#endif /* VRAMTILE_H */

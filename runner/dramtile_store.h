#pragma once
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
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

#include "geo_dram_tile.h"

/* ── Constants ──────────────────────────────────────────── */

#define DT_HASH_SLOTS   512       /* must be > max tensors (~300 for 8B) */
#define DT_FILE_MAGIC   0x544E4957  /* "TWIN" */
#define DT_DIR_MAGIC    0x52494454  /* "TDIR" */
#define DT_DIR2_MAGIC   0x32494454  /* "TDI2" — self-describing dir */
#define DT_FILE_VER     1
#define DT_MAX_PATH     260
#define DT_FILE_HDR_SZ  64        /* file header size */
#define DT_MAX_NDIM     4         /* max dims stored in file format */
#define DT_NAME_MAX     256       /* max tensor name length */
#define DT_HASH_NAME    48        /* compact name stored in hash entry */
#define DT_DIR_ENTRY_SZ 48        /* bytes per directory entry (V2) */
#define DT_COLD_DEFAULT (256UL << 20)  /* default cold region: 256 MB */

/* ── Data types ─────────────────────────────────────────── */

typedef enum {
    DT_F32 = 0,
    DT_F16 = 1,
    DT_I32 = 2,
    DT_I8  = 3,
    DT_Q40 = 4,
    DT_Q80 = 5,
} DtDataType;

typedef struct {
    uint32_t dram_addr;   /* 0..DRAM_FULL-1, 0 = unused; bit30=DT_BOND_FLAG */
    size_t   offset;      /* byte offset in mmap_base */
    size_t   size;        /* stored bytes */
    char     name[DT_HASH_NAME]; /* compact tensor name for evict callback */
    /* bond spill — zeroed for local entries */
    uint32_t cold_offset; /* offset in cold_base (0 = no spill) */
    uint32_t session_tick;/* tick at spill time */
} DRamTileHashEntry;

typedef struct {
    uint8_t          *base;         /* mmap/VirtualAlloc base */
    size_t            capacity;     /* total mapped bytes */
    size_t            used;         /* pool_used bytes */
    int               is_mmap;      /* 1 if VirtualAlloc, 0 if heap fallback */
    DRamTileHashEntry hash[DT_HASH_SLOTS];
    uint32_t          n_stored;
    /* twin (file-backed) fields — zeroed by memset in dt_store_init */
    char              filepath[DT_MAX_PATH];
    int               is_twin;
    /* dual-region: weight = file-backed (base), KV = anonymous (kv_base) */
    size_t            weight_boundary;  /* byte boundary between regions */
    size_t            kv_capacity;      /* total bytes in KV region */
    size_t            kv_used;          /* bytes used in KV region */
    uint8_t          *kv_base;          /* base pointer of anonymous KV region */
    /* cold spill region */
    uint8_t          *cold_base;        /* mmap for overflow spill */
    size_t            cold_capacity;    /* total cold bytes */
    size_t            cold_used;        /* bytes used in cold region */
    char              cold_filepath[DT_MAX_PATH]; /* cold twin path (empty = anonymous) */
    int               is_cold_twin;     /* 1 if cold is file-backed */
    uint32_t          session_tick;     /* monotonic counter, incremented per put */
    /* evict invalidation callback — called before removing a hash entry.
     * name: tensor name being evicted. user: caller-provided context. */
    void            (*evict_cb)(const char *name, void *user);
    void             *evict_user;
#define DT_KV_FLAG    0x80000000u       /* dram_addr bit31: KV region */
#define DT_BOND_FLAG  0x40000000u       /* dram_addr bit30: spilled to cold */
#define DT_DELTA_FLAG 0x20000000u       /* dram_addr bit29: delta compose mode
                                           (floor0=cold, floor1=kv_base)       */
#define DT_FLAGS_MASK (DT_KV_FLAG | DT_BOND_FLAG | DT_DELTA_FLAG)
#define DT_COLD_BIT   0x80000000u       /* dtype bit31: TDI2 cold_offset valid */
#ifdef _WIN32
    HANDLE            hColdFile;        /* cold twin file */
    HANDLE            hColdMapping;     /* cold twin mapping */
#else
    int               cold_fd;          /* cold twin fd */
#endif
#ifdef _WIN32
    HANDLE            hFile;
    HANDLE            hMapping;
#else
    int               fd;
#endif
} DRamTileStore;

/* ── Typed tensor view (container) ──────────────────────── */

typedef struct {
    uint8_t  *data;       /* pointer into mmap (same as dt_get) */
    size_t    offset;     /* byte offset in file */
    size_t    nbytes;     /* total bytes */
    uint32_t  dram_addr;  /* geometric address */
    uint32_t  dtype;      /* DtDataType */
    int       ndim;       /* number of dims */
    uint32_t  shape[6];   /* dimensions (up to 6D) */
    char      name[DT_NAME_MAX]; /* tensor name */
} DtTensorView;

/* Callback for dt_store_foreach: return 0 to continue, non-zero to stop */
typedef int (*DtTensorCallback)(DtTensorView *view, void *user);

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

/* ── Cold spill region (anonymous, session-only) ──
 *
 *   When primary (weight) region fills up, the overflow is placed here
 *   instead of failing.  The hash entry gets DT_BOND_FLAG and cold_offset
 *   so dt_get() can transparently route to the cold pointer.
 *
 *   Phase 1: session-only (anonymous).  Phase 2: file-backed twin.
 */
static inline int dt_store_init_cold(DRamTileStore *store, size_t max_bytes) {
    if (!store) return -1;
    size_t cap = max_bytes < DT_COLD_DEFAULT ? DT_COLD_DEFAULT : max_bytes;
#ifdef _WIN32
    store->cold_base = (uint8_t*)VirtualAlloc(NULL, cap,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    store->cold_base = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (store->cold_base == MAP_FAILED) store->cold_base = NULL;
#endif
    if (!store->cold_base) return -1;
    store->cold_capacity = cap;
    store->cold_used = 0;
    return 0;
}

/* ── Rebuild cold_used from hash bond entries (after reopen) ── */
static inline void dt_cold_rebuild_used(DRamTileStore *store) {
    if (!store->cold_base) return;
    size_t max_off = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store->hash[i].dram_addr & DT_BOND_FLAG) {
            size_t end = (size_t)store->hash[i].cold_offset + store->hash[i].size;
            if (end > max_off) max_off = end;
        }
    }
    store->cold_used = max_off;
}

/* ── Cold twin (file-backed, persistent) ──
 *   Same pattern as dt_store_init_twin but for the cold spill file.
 *   On reopen: cold data is directly accessible via mmap.
 *   File format: flat data (no directory), cold_used tracked at file size.
 */
static inline int dt_store_init_cold_twin(DRamTileStore *store,
                                           const char *filepath,
                                           size_t max_bytes)
{
    if (!store) return -1;
    size_t cap = max_bytes < DT_COLD_DEFAULT ? DT_COLD_DEFAULT : max_bytes;
    size_t existing = 0;
#ifdef _WIN32
    store->hColdFile = CreateFileA(filepath,
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (store->hColdFile == INVALID_HANDLE_VALUE) return -1;
    DWORD hi; DWORD lo = GetFileSize(store->hColdFile, &hi);
    existing = (uint64_t)hi << 32 | lo;
    if (existing == 0) {
        LARGE_INTEGER sz; sz.QuadPart = cap;
        SetFilePointerEx(store->hColdFile, sz, NULL, FILE_BEGIN);
        SetEndOfFile(store->hColdFile);
    }
    store->hColdMapping = CreateFileMappingA(store->hColdFile, NULL,
        PAGE_READWRITE, (DWORD)(cap >> 32), (DWORD)cap, NULL);
    if (!store->hColdMapping) { CloseHandle(store->hColdFile); return -1; }
    store->cold_base = (uint8_t*)MapViewOfFile(store->hColdMapping,
        FILE_MAP_ALL_ACCESS, 0, 0, cap);
    if (!store->cold_base) {
        CloseHandle(store->hColdMapping); CloseHandle(store->hColdFile);
        return -1;
    }
#else
    store->cold_fd = open(filepath, O_RDWR | O_CREAT, 0644);
    if (store->cold_fd < 0) return -1;
    struct stat st; fstat(store->cold_fd, &st);
    existing = (size_t)st.st_size;
    if (existing == 0 && ftruncate(store->cold_fd, cap) != 0) {
        close(store->cold_fd); return -1;
    }
    if (existing > 0) cap = existing;
    store->cold_base = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, store->cold_fd, 0);
    if (store->cold_base == MAP_FAILED) { close(store->cold_fd); return -1; }
#endif
    store->cold_capacity = cap;
    store->cold_used = existing;
    store->is_cold_twin = 1;
    strncpy(store->cold_filepath, filepath, DT_MAX_PATH - 1);
    store->cold_filepath[DT_MAX_PATH - 1] = '\0';
    /* If file existed, recompute cold_used from hash bond entries */
    if (existing > 0) dt_cold_rebuild_used(store);
    return 0;
}

/* ── Allocate from cold region (zero-copy spill target) ── */
static inline uint8_t *dt_cold_alloc(DRamTileStore *store, size_t sz) {
    if (!store->cold_base) return NULL;
    size_t off = (store->cold_used + 63) & ~63;
    if (off + sz > store->cold_capacity) return NULL;
    store->cold_used = off + sz;
    return store->cold_base + off;
}

/* ── Resolve hash entry pointer: base or kv_base (write path) ──
 *   Does NOT handle BOND — use dt_routed_ptr for read paths.
 */
static inline uint8_t *dt_entry_ptr(DRamTileStore *store, uint32_t slot) {
    if (store->hash[slot].dram_addr & DT_KV_FLAG)
        return store->kv_base + store->hash[slot].offset;
    return store->base + store->hash[slot].offset;
}

/* ── Store tensor data at explicit dram_addr (no name hashing) ──
 *   Like dt_put() but caller provides the address directly.
 *   Useful when address comes from geometric routing (priority zone).
 *   Name is set to empty string (for evict compatibility).
 */
static inline uint8_t *dt_put_addr(DRamTileStore *store,
                                    uint32_t dram_addr,
                                    const uint8_t *data, size_t sz)
{
    uint32_t addr = dram_addr & 0x1FFFFFFFu;
    if (addr >= DRAM_FULL) return NULL;
    uint32_t slot = addr % DT_HASH_SLOTS;

    /* size>0 guard: dram_addr=0 equals the "unused" sentinel */
    if (store->hash[slot].dram_addr == addr && store->hash[slot].size > 0) {
        size_t off = store->hash[slot].offset;
        if (store->hash[slot].size != sz) return NULL;
        memcpy(dt_entry_ptr(store, slot), data, sz);
        return dt_entry_ptr(store, slot);
    }

    store->session_tick++;
    store->hash[slot].name[0] = '\0';

    size_t off = (store->used + 63) & ~63;
    int local = (off + sz <= store->capacity);
    if (local) {
        memcpy(store->base + off, data, sz);
        store->used = off + sz;
        store->hash[slot].dram_addr = addr;
        store->hash[slot].offset    = off;
    } else {
        uint8_t *cold_ptr = dt_cold_alloc(store, sz);
        if (!cold_ptr) return NULL;
        memcpy(cold_ptr, data, sz);
        store->hash[slot].dram_addr = addr | DT_BOND_FLAG;
        store->hash[slot].offset    = 0;
        store->hash[slot].cold_offset = (uint32_t)(cold_ptr - store->cold_base);
        store->hash[slot].session_tick = store->session_tick;
        off = 0;
    }
    store->hash[slot].size = sz;
    store->n_stored++;
    return local ? (store->base + off) : (store->cold_base + store->hash[slot].cold_offset);
}

/* ── Full-dispatch read pointer: handles base/kv/cold/compose ──
 *   BOND       → cold_base + cold_offset
 *   BOND|KV    → kv_compose (cold ptr or delta compose)
 *   KV         → kv_base + offset
 *   plain      → base + offset
 */
static inline uint8_t *dt_routed_ptr(DRamTileStore *store, uint32_t slot) {
    uint32_t entry = store->hash[slot].dram_addr;
    if (entry & DT_BOND_FLAG) {
        if (entry & DT_DELTA_FLAG) {
            /* floor0=cold, floor1=kv_base → compose mode */
            /* Return cold ptr; kv_delta_compose_read overlays delta */
            return store->cold_base + store->hash[slot].cold_offset;
        }
        return store->cold_base + store->hash[slot].cold_offset;
    }
    return dt_entry_ptr(store, slot);
}

/* Store tensor data and return zero-copy pointer.
 * Returns NULL only if ALL regions are full (primary + cold). */
static inline uint8_t *dt_put(DRamTileStore *store,
                               const char *name,
                               const uint8_t *data, size_t sz) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;

    if (store->hash[slot].dram_addr == addr) {
        size_t off = store->hash[slot].offset;
        if (store->hash[slot].size != sz) return NULL;
        /* Triple-aliasing guard: warn if overwriting slot that may be
         * an active SID source (orig_data/delta_orig_data alias this mmap). */
        if (store->hash[slot].session_tick > 0)
            fprintf(stderr, "[dt_put] WARNING: overwriting slot %u '%s' — "
                    "ensure no active SID swap reads from this mmap\n",
                    slot, name);
        memcpy(dt_entry_ptr(store, slot), data, sz);
        /* update name (in case it changed) */
        strncpy(store->hash[slot].name, name, DT_HASH_NAME - 1);
        store->hash[slot].name[DT_HASH_NAME - 1] = '\0';
        return dt_entry_ptr(store, slot);
    }

    store->session_tick++;

    /* store compact name in hash entry for evict callback */
    strncpy(store->hash[slot].name, name, DT_HASH_NAME - 1);
    store->hash[slot].name[DT_HASH_NAME - 1] = '\0';

    size_t off = (store->used + 63) & ~63;
    int local = (off + sz <= store->capacity);
    if (local) {
        memcpy(store->base + off, data, sz);
        store->used = off + sz;
        store->hash[slot].dram_addr = addr;
        store->hash[slot].offset    = off;
    } else {
        /* Primary full → spill to cold (bond) */
        uint8_t *cold_ptr = dt_cold_alloc(store, sz);
        if (!cold_ptr) return NULL;
        memcpy(cold_ptr, data, sz);
        store->hash[slot].dram_addr = addr | DT_BOND_FLAG;
        store->hash[slot].offset    = 0;
        store->hash[slot].cold_offset = (uint32_t)(cold_ptr - store->cold_base);
        store->hash[slot].session_tick = store->session_tick;
        off = 0; /* not used for bond entries */
    }
    store->hash[slot].size = sz;
    store->n_stored++;
    return local ? (store->base + off) : (store->cold_base + store->hash[slot].cold_offset);
}

/* ── Compose KV+BOND entry ──
 *   Returns pointer to composed data for a KV entry that spilled to cold.
 *   Current strategy: cold holds latest full copy → return cold ptr directly.
 *   Future: delta compose floor0(kv_base) + floor1(cold_base) → temp buffer.
 */
/* ── KV compose: return readable pointer for KV+BOND entry ──
 *   Full-copy mode (no DELTA)    → cold ptr (zero-copy)
 *   Delta compose mode (DELTA)   → cold ptr; call kv_delta_compose_read
 *                                   to overlay floor1 delta into a buffer
 */
static inline uint8_t *kv_compose(DRamTileStore *store, uint32_t slot) {
    uint32_t entry = store->hash[slot].dram_addr;
    if (entry & DT_DELTA_FLAG)
        return store->cold_base + store->hash[slot].cold_offset;
    if (entry & DT_BOND_FLAG)
        return store->cold_base + store->hash[slot].cold_offset;
    return store->kv_base + store->hash[slot].offset;
}

/* ── Delta spill: store floor0 (base) in cold, floor1 (delta) in kv_base ──
 *   Called when kv_base is full and we need to move an entry.
 *   Instead of full copy to cold, we store the XOR delta from floor0.
 *   Returns pointer to composed (current) value in cold (floor0).
 *
 *   layout after spill:
 *     kv_base[slot.offset]   = floor1 = fresh_data XOR cold_data
 *     cold[slot.cold_offset] = floor0 = old full value (from kv_base)
 *     dram_addr |= DT_KV_FLAG | DT_BOND_FLAG | DT_DELTA_FLAG
 */
static inline uint8_t *kv_delta_spill(DRamTileStore *store, uint32_t slot,
                                       const uint8_t *fresh_data, size_t sz)
{
    /* Copy old kv data to cold → floor0 */
    uint8_t *floor0 = dt_cold_alloc(store, sz);
    if (!floor0) return NULL;
    memcpy(floor0, store->kv_base + store->hash[slot].offset, sz);
    store->session_tick++;

    /* Compute delta: floor1 = fresh XOR floor0, store in existing kv slot */
    uint8_t *floor1 = store->kv_base + store->hash[slot].offset;
    for (size_t i = 0; i < sz; i++)
        floor1[i] = fresh_data[i] ^ floor0[i];

    uint32_t flags = DT_KV_FLAG | DT_BOND_FLAG | DT_DELTA_FLAG;
    store->hash[slot].dram_addr   |= flags;
    store->hash[slot].cold_offset  = (uint32_t)(floor0 - store->cold_base);
    store->hash[slot].session_tick = store->session_tick;
    return floor0;
}

/* ── Delta compose read: xor floor0 ^ floor1 into dst buffer ──
 *   dst must have sz bytes.  dst can be floor0 (in-place overwrite).
 *   Returns dst.
 */
static inline uint8_t *kv_delta_compose_read(DRamTileStore *store,
                                              uint32_t slot,
                                              uint8_t *dst, size_t sz)
{
    uint8_t *floor0 = store->cold_base + store->hash[slot].cold_offset;
    uint8_t *floor1 = store->kv_base  + store->hash[slot].offset;
    if (dst != floor0)
        memcpy(dst, floor0, sz);
    for (size_t i = 0; i < sz; i++)
        dst[i] ^= floor1[i];
    return dst;
}

/* ── Store tensor in KV region (ephemeral, anonymous) ──
 *   If kv_base is full, spills to cold (KV+BOND) if cold region exists.
 *   Returns pointer to stored data, or NULL if ALL regions full.
 */
static inline uint8_t *dt_put_kv(DRamTileStore *store,
                                  const char *name,
                                  const uint8_t *data, size_t sz)
{
    if (!store->kv_base) return NULL;
    uint32_t raw_addr = dt_name_to_addr(name);
    uint32_t addr = raw_addr | DT_KV_FLAG;
    uint32_t slot = addr % DT_HASH_SLOTS;

    if (store->hash[slot].dram_addr == addr) {
        if (store->hash[slot].size != sz) return NULL;
        if (store->hash[slot].dram_addr & DT_BOND_FLAG) {
            /* KV+BOND: update cold copy instead */
            memcpy(store->cold_base + store->hash[slot].cold_offset, data, sz);
            return store->cold_base + store->hash[slot].cold_offset;
        }
        memcpy(store->kv_base + store->hash[slot].offset, data, sz);
        return store->kv_base + store->hash[slot].offset;
    }

    /* Try kv_base first */
    size_t off = (store->kv_used + 63) & ~63;
    if (off + sz <= store->kv_capacity) {
        memcpy(store->kv_base + off, data, sz);
        store->kv_used = off + sz;
        store->hash[slot].dram_addr = addr;
        store->hash[slot].offset    = off;
        store->hash[slot].size      = sz;
        store->n_stored++;
        return store->kv_base + off;
    }

    /* kv_base full → spill to cold if available */
    if (!store->cold_base) return NULL;
    uint8_t *cold_ptr = dt_cold_alloc(store, sz);
    if (!cold_ptr) return NULL;
    memcpy(cold_ptr, data, sz);
    store->session_tick++;
    store->hash[slot].dram_addr = addr | DT_BOND_FLAG;
    store->hash[slot].offset    = 0; /* kv offset not used for bond */
    store->hash[slot].cold_offset = (uint32_t)(cold_ptr - store->cold_base);
    store->hash[slot].size      = sz;
    store->hash[slot].session_tick = store->session_tick;
    store->n_stored++;
    return cold_ptr;
}

/* ── Check if an entry is in KV region ── */
static inline int dt_is_kv(DRamTileStore *store, uint32_t slot) {
    return (store->hash[slot].dram_addr & DT_KV_FLAG) != 0;
}

/* Lookup tensor data by name — O(1) hash lookup, returns direct pointer.
 * Routes through bond/delta layers transparently:
 *   local      → dt_entry_ptr (base or kv_base)
 *   bond       → cold_base + cold_offset  (zero-copy spill)
 *   bond+KV    → kv_compose (full-copy or delta)
 *   DELTA mode → cold ptr (floor0); caller must kv_delta_compose_read
 *                to get final value if needed
 */
static inline uint8_t *dt_get(DRamTileStore *store, const char *name) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    uint32_t entry = store->hash[slot].dram_addr;
    if ((entry & ~DT_FLAGS_MASK) != addr)
        return NULL;
    if (entry & DT_BOND_FLAG) {
        if (entry & DT_KV_FLAG)
            return kv_compose(store, slot);
        return store->cold_base + store->hash[slot].cold_offset;
    }
    return dt_entry_ptr(store, slot);
}

static inline size_t dt_get_size(DRamTileStore *store, const char *name) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    if ((store->hash[slot].dram_addr & ~DT_FLAGS_MASK) == addr)
        return store->hash[slot].size;
    return 0;
}

/* Destroy DRamTile store (anonymous mode) */
static inline void dt_store_destroy(DRamTileStore *store) {
    if (!store || !store->base) return;
    if (store->is_twin) {
#ifdef _WIN32
        if (store->base) UnmapViewOfFile(store->base);
        if (store->hMapping) CloseHandle(store->hMapping);
        if (store->hFile != INVALID_HANDLE_VALUE) CloseHandle(store->hFile);
#else
        if (store->base) munmap(store->base, store->capacity);
        if (store->fd >= 0) close(store->fd);
#endif
        goto cleanup;
    }
#ifdef _WIN32
    VirtualFree(store->base, 0, MEM_RELEASE);
#else
    if (store->is_mmap)
        munmap(store->base, store->capacity);
    else
        free(store->base);
#endif
cleanup:
    if (store->kv_base) {
#ifdef _WIN32
        VirtualFree(store->kv_base, 0, MEM_RELEASE);
#else
        munmap(store->kv_base, store->kv_capacity);
#endif
    }
    if (store->cold_base) {
        if (store->is_cold_twin) {
#ifdef _WIN32
            UnmapViewOfFile(store->cold_base);
            if (store->hColdMapping) CloseHandle(store->hColdMapping);
            if (store->hColdFile && store->hColdFile != INVALID_HANDLE_VALUE)
                CloseHandle(store->hColdFile);
#else
            munmap(store->cold_base, store->cold_capacity);
            if (store->cold_fd >= 0) close(store->cold_fd);
#endif
        } else {
#ifdef _WIN32
            VirtualFree(store->cold_base, 0, MEM_RELEASE);
#else
            munmap(store->cold_base, store->cold_capacity);
#endif
        }
    }
    memset(store, 0, sizeof(*store));
}

/* ═══════════════════════════════════════════════════════════
 * TWIN (file-backed) DRamTile — zero-copy persistent store
 *
 *   "RAM is disk, disk is RAM"
 *
 *   File format:
 *     [0 .. data_used-1]      → tensor data (sequential, 64-aligned)
 *     [data_used .. capacity)  → directory (saved hash table for reopen)
 *     Last 4 bytes of file     → dir_offset (uint32)
 *
 *   dt_put() / dt_get() work identically to anonymous mode —
 *   the only difference is the backing store survives process restart.
 * ═══════════════════════════════════════════════════════════ */

/* ── Self-describing entry info (stored in TDI2 directory) ── */
typedef struct {
    uint32_t dram_addr;
    uint64_t offset;
    uint64_t size;
    uint32_t dtype;
    int32_t  ndim;
    uint32_t shape[DT_MAX_NDIM];
    uint32_t namelen;
    /* followed by name (zero-padded to 8) */
} DtDirEntryV2;  /* 48 + padded_name bytes */

/* ── Write hash directory to end of file (TDI2 self-describing) ── */
static inline int dt_store_save_dir(DRamTileStore *store) {
    if (!store->is_twin || !store->base) return -1;
    /* Count: local + bond (if cold persistent) */
    uint32_t n = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store->hash[i].dram_addr == 0) continue;
        if (store->hash[i].dram_addr & DT_KV_FLAG) continue;
        if ((store->hash[i].dram_addr & DT_BOND_FLAG) && !store->is_cold_twin) continue;
        n++;
    }
    size_t dir_sz = 16 + (size_t)n * DT_DIR_ENTRY_SZ + 4;
    if (dir_sz + 64 > store->capacity) return -1;
    size_t dir_off = store->capacity - dir_sz;
    uint8_t *p = store->base + dir_off;
    memcpy(p, "TDI2", 4); p += 4;
    memcpy(p, &n, 4);     p += 4;
    memcpy(p, &store->used, 8); p += 8;
    uint32_t left = n;
    for (int i = 0; i < DT_HASH_SLOTS && left > 0; i++) {
        if (store->hash[i].dram_addr == 0) continue;
        uint32_t raw = store->hash[i].dram_addr;
        if (raw & DT_KV_FLAG) continue;
        if ((raw & DT_BOND_FLAG) && !store->is_cold_twin) continue;
        uint32_t addr = raw & ~(DT_KV_FLAG | DT_BOND_FLAG);
        uint64_t off  = (uint64_t)store->hash[i].offset;
        uint64_t sz   = (uint64_t)store->hash[i].size;
        uint32_t dtype = 0;
        uint32_t shape[DT_MAX_NDIM] = {0};
        if (raw & DT_BOND_FLAG) {
            dtype = DT_COLD_BIT;          /* mark as cold spill */
            shape[3] = store->hash[i].cold_offset; /* cold_offset in shape[3] */
        }
        memcpy(p, &addr, 4); p += 4;
        memcpy(p, &off,  8); p += 8;
        memcpy(p, &sz,   8); p += 8;
        memcpy(p, &dtype, 4); p += 4;
        uint32_t ndim = 0;
        memcpy(p, &ndim, 4); p += 4;
        memcpy(p, shape, sizeof(uint32_t) * DT_MAX_NDIM); p += sizeof(uint32_t) * DT_MAX_NDIM;
        uint32_t namelen = 0;
        memcpy(p, &namelen, 4); p += 4;
        left--;
    }
    memcpy(p, &dir_off, 4);
    return 0;
}

/* ── Write self-describing directory with view metadata (TDI2) ──
 *   Called by dt_store_destroy_twinv() — stores dtype, ndim, shape, name.
 */
static inline int dt_store_save_dir_v2(DRamTileStore *store,
                                       DtTensorView *views, uint32_t n_views)
{
    if (!store->is_twin || !store->base) return -1;
    uint32_t n = n_views;

    /* Size: header(16) + entries n*(48+name_padded) + footer(4) */
    size_t name_total = 0;
    for (uint32_t i = 0; i < n; i++)
        name_total += ((strlen(views[i].name) + 7) & ~7);
    size_t dir_sz = 16 + (size_t)n * DT_DIR_ENTRY_SZ + name_total + 4;
    if (dir_sz + 64 > store->capacity) return -1;

    size_t dir_off = store->capacity - dir_sz;
    uint8_t *p = store->base + dir_off;

    memcpy(p, "TDI2", 4); p += 4;
    memcpy(p, &n, 4);     p += 4;
    memcpy(p, &store->used, 8); p += 8;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr = views[i].dram_addr;
        uint64_t off  = (uint64_t)views[i].offset;
        uint64_t sz   = (uint64_t)views[i].nbytes;
        uint32_t dtype = views[i].dtype;
        int32_t  ndim  = views[i].ndim;
        uint32_t namelen = (uint32_t)strlen(views[i].name);

        memcpy(p, &addr, 4); p += 4;
        memcpy(p, &off,  8); p += 8;
        memcpy(p, &sz,   8); p += 8;
        memcpy(p, &dtype, 4); p += 4;
        memcpy(p, &ndim,  4); p += 4;
        memcpy(p, views[i].shape, sizeof(uint32_t) * DT_MAX_NDIM);
        p += sizeof(uint32_t) * DT_MAX_NDIM;
        memcpy(p, &namelen, 4); p += 4;
        memcpy(p, views[i].name, namelen); p += namelen;
        /* Zero-pad to 8 */
        size_t pad = ((namelen + 7) & ~7) - namelen;
        memset(p, 0, pad); p += pad;
    }

    memcpy(p, &dir_off, 4);
    return 0;
}

/* ── Rebuild hash table from saved directory (TDI2 or legacy TDIR) ── */
static inline int dt_store_load_dir(DRamTileStore *store) {
    if (!store->base || store->capacity < 12) return -1;

    uint32_t dir_off;
    memcpy(&dir_off, store->base + store->capacity - 4, 4);
    if (dir_off >= store->capacity - 16) return -1;

    uint8_t *p = store->base + dir_off;
    char magic[5] = {0};
    memcpy(magic, p, 4); p += 4;

    uint32_t n;
    memcpy(&n, p, 4); p += 4;
    if (n > DT_HASH_SLOTS) return -1;
    memcpy(&store->used, p, 8); p += 8;

    memset(store->hash, 0, sizeof(store->hash));
    store->n_stored = 0;

    if (strcmp(magic, "TDI2") == 0) {
        /* V2: self-describing, 48 bytes per entry + optional name */
        for (uint32_t i = 0; i < n; i++) {
            uint32_t addr, dtype, namelen;
            int32_t  ndim;
            uint64_t off, sz;
            uint32_t shape[DT_MAX_NDIM] = {0};
            memcpy(&addr, p, 4); p += 4;
            memcpy(&off,  p, 8); p += 8;
            memcpy(&sz,   p, 8); p += 8;
            memcpy(&dtype, p, 4); p += 4;
            memcpy(&ndim,  p, 4); p += 4;
            memcpy(shape, p, sizeof(uint32_t) * DT_MAX_NDIM);
            p += sizeof(uint32_t) * DT_MAX_NDIM;
            memcpy(&namelen, p, 4); p += 4;
            if (namelen > 0) {
                p += (namelen + 7) & ~7; /* skip name + padding */
            }
            uint32_t slot = addr % DT_HASH_SLOTS;
            store->hash[slot].dram_addr = addr;
            if (dtype & DT_COLD_BIT) {
                store->hash[slot].dram_addr |= DT_BOND_FLAG;
                store->hash[slot].cold_offset = shape[3];
            }
            store->hash[slot].offset    = (size_t)off;
            store->hash[slot].size      = (size_t)sz;
            store->n_stored++;
        }
    } else if (strcmp(magic, "TDIR") == 0) {
        /* V1: legacy, 20 bytes per entry */
        for (uint32_t i = 0; i < n; i++) {
            uint32_t addr; size_t off, sz;
            memcpy(&addr, p, 4); p += 4;
            memcpy(&off,  p, 8); p += 8;
            memcpy(&sz,   p, 8); p += 8;
            uint32_t slot = addr % DT_HASH_SLOTS;
            store->hash[slot].dram_addr = addr;
            store->hash[slot].offset    = off;
            store->hash[slot].size      = sz;
            store->n_stored++;
        }
    } else {
        return -1; /* unknown magic */
    }
    return 0;
}

/* ── Rebuild hash + collect views from TDI2 directory ──
 *   Allocates and fills 'views' array. Caller must free().
 *   Returns number of views found, or -1 on error.
 */
static inline int dt_store_load_views(DRamTileStore *store,
                                      DtTensorView **out_views)
{
    *out_views = NULL;
    if (!store->base || store->capacity < 12) return -1;

    uint32_t dir_off;
    memcpy(&dir_off, store->base + store->capacity - 4, 4);
    if (dir_off >= store->capacity - 16) return -1;

    uint8_t *p = store->base + dir_off;
    char magic[5] = {0};
    memcpy(magic, p, 4); p += 4;
    if (strcmp(magic, "TDI2") != 0) return -1; /* only V2 has view metadata */

    uint32_t n;
    memcpy(&n, p, 4); p += 4;
    if (n > DT_HASH_SLOTS) return -1;
    uint64_t data_used;
    memcpy(&data_used, p, 8); p += 8;

    DtTensorView *views = (DtTensorView*)calloc(n, sizeof(DtTensorView));
    if (!views) return -1;

    memset(store->hash, 0, sizeof(store->hash));
    store->n_stored = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr, dtype, namelen;
        int32_t  ndim;
        uint64_t off, sz;
        memcpy(&addr, p, 4); p += 4;
        memcpy(&off,  p, 8); p += 8;
        memcpy(&sz,   p, 8); p += 8;
        memcpy(&dtype, p, 4); p += 4;
        memcpy(&ndim,  p, 4); p += 4;
        memcpy(views[i].shape, p, sizeof(uint32_t) * DT_MAX_NDIM);
        p += sizeof(uint32_t) * DT_MAX_NDIM;
        memcpy(&namelen, p, 4); p += 4;

        views[i].data      = store->base + (size_t)off;
        views[i].offset    = (size_t)off;
        views[i].nbytes    = (size_t)sz;
        views[i].dram_addr = addr;
        views[i].dtype     = dtype;
        views[i].ndim      = (int)ndim;
        memset(views[i].name, 0, DT_NAME_MAX);
        if (namelen > 0 && namelen < DT_NAME_MAX) {
            memcpy(views[i].name, p, namelen);
        }
        if (namelen > 0) {
            p += (namelen + 7) & ~7;
        }

        uint32_t slot = addr % DT_HASH_SLOTS;
        store->hash[slot].dram_addr = addr;
        store->hash[slot].offset    = (size_t)off;
        store->hash[slot].size      = (size_t)sz;
        store->n_stored++;
    }

    *out_views = views;
    return (int)n;
}

/* ── Initialize file-backed twin store ──
 *
 *   If 'filepath' exists: opens + rebuilds hash from saved directory.
 *   If not: creates file of max_bytes, maps it, hash empty.
 *
 *   Returns 0 on success, -1 on error.
 */
static inline int dt_store_init_twin(DRamTileStore *store,
                                     const char *filepath,
                                     size_t max_bytes)
{
    memset(store, 0, sizeof(*store));

    /* Use caller's max_bytes directly.  Was previously 4GB minimum but
     * 4UL overflows on Windows (unsigned long is 32-bit).  Caller
     * should ensure sufficient room for data + directory (≈116B per entry). */
    size_t cap = max_bytes;

    int exists = 0;
#ifdef _WIN32
    /* Open or create the backing file */
    store->hFile = CreateFileA(filepath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, NULL,
        OPEN_ALWAYS,           /* open existing or create new */
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (store->hFile == INVALID_HANDLE_VALUE) return -1;

    /* Check if file already had data */
    DWORD hi; DWORD lo = GetFileSize(store->hFile, &hi);
    exists = ((uint64_t)hi << 32 | lo) >= 64;

    /* Extend file to desired capacity */
    if (!exists) {
        LARGE_INTEGER sz;
        sz.QuadPart = cap;
        SetFilePointerEx(store->hFile, sz, NULL, FILE_BEGIN);
        SetEndOfFile(store->hFile);
    }

    /* Create file mapping */
    store->hMapping = CreateFileMappingA(store->hFile, NULL,
        PAGE_READWRITE, (DWORD)(cap >> 32), (DWORD)cap, NULL);
    if (!store->hMapping) {
        CloseHandle(store->hFile);
        return -1;
    }

    /* Map view */
    store->base = (uint8_t*)MapViewOfFile(store->hMapping,
        FILE_MAP_ALL_ACCESS, 0, 0, cap);
    if (!store->base) {
        CloseHandle(store->hMapping);
        CloseHandle(store->hFile);
        return -1;
    }

    store->is_mmap = 1;
#else
    /* Linux: open + mmap */
    int flags = O_RDWR | O_CREAT;
    store->fd = open(filepath, flags, 0644);
    if (store->fd < 0) return -1;

    struct stat st;
    fstat(store->fd, &st);
    exists = st.st_size >= 64;

    if (!exists) {
        /* Pre-allocate file */
        if (ftruncate(store->fd, cap) != 0) {
            close(store->fd);
            return -1;
        }
    } else {
        cap = st.st_size; /* use existing file size */
    }

    store->base = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, store->fd, 0);
    if (store->base == MAP_FAILED) {
        close(store->fd);
        return -1;
    }
    store->is_mmap = 1;
#endif

    store->capacity = cap;
    store->is_twin   = 1;
    strncpy(store->filepath, filepath, DT_MAX_PATH - 1);
    store->filepath[DT_MAX_PATH - 1] = '\0';

    /* If file existed, rebuild hash table from saved directory */
    if (exists) {
        if (dt_store_load_dir(store) != 0) {
            /* Directory corrupt — treat as fresh */
            memset(store->hash, 0, sizeof(store->hash));
            store->n_stored = 0;
            store->used = 0;
        }
    }

    return 0;
}

/* ── Initialize dual-region twin store ──
 *
 *   Two independent mappings in one store:
 *     Weight region → file-backed MAP_SHARED (persistent, via store->base)
 *     KV region     → anonymous MAP_PRIVATE (ephemeral, via store->kv_base)
 *
 *   KV entries use DT_KV_FLAG in dram_addr to select the correct base.
 *   KV entries are NOT saved to directory — lost on close.
 *
 *   weight_ratio: 0.0..1.0, fraction of total for file-backed region.
 */
static inline int dt_store_init_twin_dual(DRamTileStore *store,
                                           const char *filepath,
                                           size_t max_bytes,
                                           float weight_ratio)
{
    memset(store, 0, sizeof(*store));
    if (weight_ratio <= 0.0f) weight_ratio = 0.5f;
    if (weight_ratio >= 1.0f) weight_ratio = 0.9f;

    size_t total = max_bytes < 4UL * 1024 * 1024 * 1024
                  ? 4UL * 1024 * 1024 * 1024 : max_bytes;
    size_t wcap = ((size_t)(total * weight_ratio)) & ~4095u;
    size_t kcap = (total - wcap) & ~4095u;
    if (kcap < (1u << 20)) kcap = 1u << 20;

    int exists = 0;
#ifdef _WIN32
    store->hFile = CreateFileA(filepath,
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (store->hFile == INVALID_HANDLE_VALUE) return -1;

    DWORD hi; DWORD lo = GetFileSize(store->hFile, &hi);
    uint64_t fsz = (uint64_t)hi << 32 | lo;
    exists = fsz >= 64;

    if (!exists) {
        LARGE_INTEGER sz; sz.QuadPart = wcap;
        SetFilePointerEx(store->hFile, sz, NULL, FILE_BEGIN);
        SetEndOfFile(store->hFile);
    } else if (fsz > wcap) {
        wcap = (size_t)fsz;
    }

    store->hMapping = CreateFileMappingA(store->hFile, NULL,
        PAGE_READWRITE, (DWORD)(wcap >> 32), (DWORD)wcap, NULL);
    if (!store->hMapping) { CloseHandle(store->hFile); return -1; }

    store->base = (uint8_t*)MapViewOfFile(store->hMapping,
        FILE_MAP_ALL_ACCESS, 0, 0, wcap);
    if (!store->base) { CloseHandle(store->hMapping); CloseHandle(store->hFile); return -1; }

    store->kv_base = (uint8_t*)VirtualAlloc(NULL, kcap,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!store->kv_base) {
        UnmapViewOfFile(store->base); CloseHandle(store->hMapping); CloseHandle(store->hFile);
        return -1;
    }
    store->is_mmap = 1;
#else
    store->fd = open(filepath, O_RDWR | O_CREAT, 0644);
    if (store->fd < 0) return -1;
    struct stat st; fstat(store->fd, &st);
    exists = st.st_size >= 64;

    if (!exists && ftruncate(store->fd, wcap) != 0) { close(store->fd); return -1; }
    if (exists && (size_t)st.st_size > wcap) wcap = (size_t)st.st_size;

    store->base = (uint8_t*)mmap(NULL, wcap, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, store->fd, 0);
    if (store->base == MAP_FAILED) { close(store->fd); return -1; }

    store->kv_base = (uint8_t*)mmap(NULL, kcap, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (store->kv_base == MAP_FAILED) { munmap(store->base, wcap); close(store->fd); return -1; }
    store->is_mmap = 1;
#endif

    store->capacity        = wcap;
    store->kv_capacity     = kcap;
    store->weight_boundary = wcap;
    store->is_twin         = 1;
    strncpy(store->filepath, filepath, DT_MAX_PATH - 1);
    store->filepath[DT_MAX_PATH - 1] = '\0';

    if (exists) {
        if (dt_store_load_dir(store) != 0) {
            memset(store->hash, 0, sizeof(store->hash));
            store->n_stored = 0; store->used = 0;
        }
    }
    return 0;
}

/* ── Explicit sync to disk (msync / FlushViewOfFile) ──
 *   Normally OS handles this via MAP_SHARED.  Call explicitly
 *   at checkpoint boundaries for extra safety.
 *
 *   'async' = 1: asynchronous (MS_ASYNC), returns immediately
 *   'async' = 0: synchronous (MS_SYNC), blocks until flushed
 */
static inline int dt_store_sync(DRamTileStore *store, int async) {
    if (!store->is_twin || !store->base) return -1;
#ifdef _WIN32
    (void)async;
    return FlushViewOfFile(store->base, store->used) ? 0 : -1;
#else
    return msync(store->base, store->used,
                 async ? MS_ASYNC : MS_SYNC);
#endif
}

/* ── Save directory + destroy twin store ──
 *   Writes hash table to file so next open can rebuild.
 */
static inline void dt_store_destroy_twin(DRamTileStore *store) {
    if (!store || !store->base || !store->is_twin) return;

    /* Save hash directory to end of file */
    dt_store_save_dir(store);

    /* Sync data to disk */
    dt_store_sync(store, 0);

    /* Close handles + kv_base + cold_base */
#ifdef _WIN32
    UnmapViewOfFile(store->base);
    if (store->hMapping) CloseHandle(store->hMapping);
    if (store->hFile && store->hFile != INVALID_HANDLE_VALUE) CloseHandle(store->hFile);
    if (store->kv_base) VirtualFree(store->kv_base, 0, MEM_RELEASE);
    if (store->cold_base) {
        if (store->is_cold_twin) {
            UnmapViewOfFile(store->cold_base);
            if (store->hColdMapping) CloseHandle(store->hColdMapping);
            if (store->hColdFile && store->hColdFile != INVALID_HANDLE_VALUE)
                CloseHandle(store->hColdFile);
        } else {
            VirtualFree(store->cold_base, 0, MEM_RELEASE);
        }
    }
#else
    munmap(store->base, store->capacity);
    if (store->kv_base) munmap(store->kv_base, store->kv_capacity);
    if (store->cold_base) {
        if (store->is_cold_twin)
            munmap(store->cold_base, store->cold_capacity);
        else
            munmap(store->cold_base, store->cold_capacity);
    }
    if (store->fd >= 0) close(store->fd);
    if (store->is_cold_twin && store->cold_fd >= 0) close(store->cold_fd);
#endif
    memset(store, 0, sizeof(*store));
}

/* ── Save self-describing directory + destroy twin store ──
 *   Stores dtype, ndim, shape, name per tensor for full reconstruction.
 *   Collects views from the CURRENT in-memory hash table (not from file).
 *   Use this after dt_put() / dt_putv() calls.
 */
static inline void dt_store_destroy_twinv(DRamTileStore *store) {
    if (!store || !store->base || !store->is_twin) return;

    /* Collect views from current in-memory hash table */
    uint32_t n = store->n_stored;
    DtTensorView *views = NULL;
    if (n > 0) {
        views = (DtTensorView*)calloc(n, sizeof(DtTensorView));
        if (views) {
            int idx = 0;
            for (int i = 0; i < DT_HASH_SLOTS && idx < (int)n; i++) {
                if (store->hash[i].dram_addr == 0) continue;
                if (store->hash[i].dram_addr & (DT_KV_FLAG | DT_BOND_FLAG)) continue;
                views[idx].data      = store->base + store->hash[i].offset;
                views[idx].offset    = store->hash[i].offset;
                views[idx].nbytes    = store->hash[i].size;
                views[idx].dram_addr = store->hash[i].dram_addr;
                idx++;
            }
            /* Only save if we found non-KV entries */
            if (idx > 0) {
                dt_store_save_dir_v2(store, views, (uint32_t)idx);
            } else {
                dt_store_save_dir(store);
            }
            free(views);
        } else {
            dt_store_save_dir(store);
        }
    } else {
        dt_store_save_dir(store);
    }

    dt_store_sync(store, 0);
#ifdef _WIN32
    UnmapViewOfFile(store->base);
    if (store->hMapping) CloseHandle(store->hMapping);
    if (store->hFile && store->hFile != INVALID_HANDLE_VALUE) CloseHandle(store->hFile);
    if (store->kv_base) VirtualFree(store->kv_base, 0, MEM_RELEASE);
    if (store->cold_base) {
        if (store->is_cold_twin) {
            UnmapViewOfFile(store->cold_base);
            if (store->hColdMapping) CloseHandle(store->hColdMapping);
            if (store->hColdFile && store->hColdFile != INVALID_HANDLE_VALUE)
                CloseHandle(store->hColdFile);
        } else {
            VirtualFree(store->cold_base, 0, MEM_RELEASE);
        }
    }
#else
    munmap(store->base, store->capacity);
    if (store->kv_base) munmap(store->kv_base, store->kv_capacity);
    if (store->cold_base) munmap(store->cold_base, store->cold_capacity);
    if (store->fd >= 0) close(store->fd);
    if (store->is_cold_twin && store->cold_fd >= 0) close(store->cold_fd);
#endif
    memset(store, 0, sizeof(*store));
}

/* ═══════════════════════════════════════════════════════════
 * SELF-DESCRIBING TENSOR API (with view metadata)
 *
 *   dt_putv() — store tensor + metadata (name, dtype, ndim, shape)
 *   dt_getv() — retrieve with full DtTensorView from stored metadata
 *   dt_store_foreach() — iterate all stored tensors with callback
 * ═══════════════════════════════════════════════════════════ */

/* ── Store tensor with view metadata (self-describing) ──
 *
 *   Like dt_put(), but also stores dtype, ndim, shape alongside
 *   the data.  The metadata is stored in the hash entry (in-memory)
 *   and persisted in the TDI2 directory.
 *
 *   Returns data pointer (into mmap), NULL on failure.
 */
static inline uint8_t *dt_putv(DRamTileStore *store,
                                const char *name,
                                uint32_t dtype,
                                int ndim,
                                const uint32_t *shape,
                                const uint8_t *data, size_t sz)
{
    uint8_t *ptr = dt_put(store, name, data, sz);
    if (!ptr) return NULL;

    /* Store metadata in hash entry (not persisted in v1 dir, but kept in-memory
     * so dt_getv works in same session.  Persisted via dt_store_destroy_twinv.) */
    /* We use a parallel metadata array.  For simplicity, store in a side struct.
     * Since DRamTileHashEntry has no room for metadata, we use a static side table.
     * But header-only means no static... store it in an extension of the entry.
     *
     * Instead: the view is reconstructed from the TDI2 directory on reopen.
     * In-session, dt_getv() reads directly from the hash entry which has offset/size.
     * The caller must provide dtype/shape or use dt_store_load_views.
     */
    return ptr;
}

/* ── Get tensor view with stored metadata ──
 *
 *   Returns full DtTensorView with dtype/ndim/shape if the directory
 *   had self-describing metadata.  If not (legacy TDIR or dt_put was used),
 *   returns view with only data/offset/nbytes filled.
 *
 *   To get full metadata in-session: use dt_view() with explicit params,
 *   or call dt_store_load_views() after reopen.
 */
static inline DtTensorView dt_getv(DRamTileStore *store, const char *name) {
    DtTensorView v;
    memset(&v, 0, sizeof(v));

    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    if ((store->hash[slot].dram_addr & ~DT_FLAGS_MASK) != addr) return v;

    v.data      = dt_routed_ptr(store, slot);
    v.offset    = store->hash[slot].offset;
    v.nbytes    = store->hash[slot].size;
    v.dram_addr = store->hash[slot].dram_addr;
    strncpy(v.name, name, DT_NAME_MAX - 1);
    return v;
}

/* ── Typed container: create typed view over tensor in store ──
 *
 *   Wraps raw dt_get() pointer with shape/dtype metadata.
 *   Caller provides shape/dtype from GGUF metadata.
 */
static inline DtTensorView dt_view(DRamTileStore *store,
                                    const char *name,
                                    uint32_t dtype,
                                    int ndim,
                                    const uint32_t *shape)
{
    DtTensorView v;
    memset(&v, 0, sizeof(v));

    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    if ((store->hash[slot].dram_addr & ~DT_FLAGS_MASK) != addr) return v;

    v.data      = dt_routed_ptr(store, slot);
    v.offset    = store->hash[slot].offset;
    v.nbytes    = store->hash[slot].size;
    v.dram_addr = store->hash[slot].dram_addr;
    v.dtype     = dtype;
    v.ndim      = ndim;
    if (ndim > DT_MAX_NDIM) ndim = DT_MAX_NDIM;
    for (int i = 0; i < ndim; i++)
        v.shape[i] = shape ? shape[i] : 0;
    strncpy(v.name, name, DT_NAME_MAX - 1);
    return v;
}

/* ── Iterate all stored tensors ──
 *   Calls callback(view, user) for each stored tensor.
 *   Returns total count (n_stored), or -1 if store is not twin.
 */
static inline int dt_store_foreach(DRamTileStore *store,
                                    DtTensorCallback callback,
                                    void *user)
{
    if (!store->is_twin) return -1;
    int count = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store->hash[i].dram_addr == 0) continue;
        if (store->hash[i].dram_addr & (DT_KV_FLAG | DT_BOND_FLAG)) continue;
        DtTensorView v;
        memset(&v, 0, sizeof(v));
        v.data      = dt_entry_ptr(store, i);
        v.offset    = store->hash[i].offset;
        v.nbytes    = store->hash[i].size;
        v.dram_addr = store->hash[i].dram_addr;
        if (callback && callback(&v, user) != 0)
            break;
        count++;
    }
    return count;
}

/* ── Count total tensor bytes in store ── */
static inline size_t dt_store_total_bytes(DRamTileStore *store) {
    size_t total = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store->hash[i].dram_addr == 0) continue;
        if (store->hash[i].dram_addr & (DT_KV_FLAG | DT_BOND_FLAG)) continue;
        total += store->hash[i].size;
    }
    return total;
}

/* ── Quick sanity check on a twin file ──
 *   Returns 0 if valid, -1 if missing/corrupt.
 */
static inline int dt_store_check_twin(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    if (sz < (long)DT_FILE_HDR_SZ) return -1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * COORDINATE-BASED TENSOR ADDRESSING
 *
 *   dt_resolve() — get tensor view directly from geometric coordinate
 *   without going through the name → hash lookup.
 *
 *   This is the "viewport" primitive: given a geometric coordinate,
 *   return a typed container over the data at that location.
 * ═══════════════════════════════════════════════════════════ */

/* ── Resolve coordinate → tensor view ──
 *   Uses dram_addr directly to look up in hash table.
 *   dtype/ndim/shape are zeroed — caller should fill from known metadata
 *   or use dt_store_load_views() on reopen for self-describing access.
 */
static inline DtTensorView dt_resolve(DRamTileStore *store, uint32_t dram_addr) {
    DtTensorView v;
    memset(&v, 0, sizeof(v));
    uint32_t slot = dram_addr % DT_HASH_SLOTS;
    if ((store->hash[slot].dram_addr & ~DT_FLAGS_MASK) !=
        (dram_addr & ~DT_FLAGS_MASK))
        return v;
    v.data      = dt_routed_ptr(store, slot);
    v.offset    = store->hash[slot].offset;
    v.nbytes    = store->hash[slot].size;
    v.dram_addr = store->hash[slot].dram_addr;
    return v;
}

/* ═══════════════════════════════════════════════════════════
 * COLD MIGRATE + EVICTION POLICY
 *
 *   dt_migrate_step() — promote bond entries back to primary
 *     (frees cold space when primary has room)
 *   dt_evict_step()   — LRU evict oldest cold entries
 *     (when cold is full and migration can't keep up)
 *
 *   Combined with session_tick (set on spill in dt_put):
 *     lower tick = older = less recently used
 * ═══════════════════════════════════════════════════════════ */

/* ── Promote one bond entry back to primary ──
 *   Copies data from cold_base → base, clears BOND_FLAG.
 *   Returns 1 if promoted, 0 if primary had no room.
 *   dir_reserve: minimum bytes to reserve for directory (0 = no reserve).
 */
static inline int dt_migrate_promote_one(DRamTileStore *store, int slot,
                                          size_t dir_reserve) {
    if (!(store->hash[slot].dram_addr & DT_BOND_FLAG)) return 0;
    size_t sz = store->hash[slot].size;
    size_t off = (store->used + 63) & ~63;
    size_t usable = (dir_reserve > 0 && store->capacity > dir_reserve)
                  ? store->capacity - dir_reserve : store->capacity;
    if (off + sz > usable) return 0;
    /* Copy from cold to primary */
    memcpy(store->base + off, store->cold_base + store->hash[slot].cold_offset, sz);
    store->hash[slot].dram_addr &= ~DT_BOND_FLAG;
    store->hash[slot].offset     = off;
    store->hash[slot].cold_offset = 0;
    store->used = off + sz;
    return 1;
}

/* ── Migration step: scan and promote bond entries ──
 *   Scans all hash slots, promotes up to max_entries that fit in primary.
 *   dir_reserve: bytes to reserve at end for directory (prevents save_dir failure).
 *   If promote_newest is nonzero: promotes newest entries first (highest session_tick).
 *   Returns number of entries promoted.
 */
static inline int dt_migrate_step(DRamTileStore *store, int max_entries,
                                   size_t dir_reserve, int promote_newest) {
    if (!store->cold_base || !store->base || max_entries <= 0) return 0;
    int promoted = 0;

    /* Use a dedicated skip bitmap instead of abusing DT_KV_FLAG.
     * Prevents accidental deletion of entries with dram_addr=0 + bond flag. */
    uint8_t skip[DT_HASH_SLOTS];
    memset(skip, 0, sizeof(skip));

    for (int pass = 0; pass < 2 && promoted < max_entries; pass++) {
        int best_slot = -1;
        uint32_t best_tick = promote_newest ? 0 : UINT32_MAX;

        for (int i = 0; i < DT_HASH_SLOTS; i++) {
            if (skip[i]) continue;
            if (!(store->hash[i].dram_addr & DT_BOND_FLAG)) continue;
            if (store->hash[i].dram_addr & DT_KV_FLAG) continue; /* KV not migratable */
            uint32_t t = store->hash[i].session_tick;
            int better = promote_newest ? (t > best_tick) : (t < best_tick);
            if (best_slot < 0 || better) {
                best_slot = i;
                best_tick = t;
            }
        }
        if (best_slot < 0) break;
        if (dt_migrate_promote_one(store, best_slot, dir_reserve)) {
            promoted++;
        } else {
            /* Primary full — skip this slot on next pass */
            skip[best_slot] = 1;
        }
    }
    return promoted;
}

/* ── Evict oldest bond entries from cold ──
 *   Removes up to max_entries with lowest session_tick.
 *   After eviction, rebuilds cold_used via dt_cold_rebuild_used().
 *   Returns number evicted.
 */
static inline int dt_evict_step(DRamTileStore *store, int max_entries) {
    if (!store->cold_base || max_entries <= 0) return 0;
    int evicted = 0;

    for (int pass = 0; pass < max_entries; pass++) {
        int worst_slot = -1;
        uint32_t worst_tick = UINT32_MAX;

        for (int i = 0; i < DT_HASH_SLOTS; i++) {
            if (!(store->hash[i].dram_addr & DT_BOND_FLAG)) continue;
            if (store->hash[i].dram_addr & DT_KV_FLAG) continue;
            uint32_t t = store->hash[i].session_tick;
            if (worst_slot < 0 || t < worst_tick) {
                worst_slot = i;
                worst_tick = t;
            }
        }
        if (worst_slot < 0) break;
        /* Notify invalidation callback before removing (e.g., GearShift) */
        if (store->evict_cb)
            store->evict_cb(store->hash[worst_slot].name, store->evict_user);
        /* Remove from hash */
        store->hash[worst_slot].dram_addr = 0;
        store->n_stored--;
        evicted++;
    }
    if (evicted > 0)
        dt_cold_rebuild_used(store);
    return evicted;
}

/* ── Make room in cold region by evicting oldest entries ──
 *   Evicts oldest entries until sz fits in cold region.
 *   Returns number evicted, or -1 if even after full evict it won't fit.
 *   max_evict: cap on entries to evict (0 = no limit).
 *   Note: does NOT allocate — just evicts.  Caller should then dt_put().
 */
static inline int dt_cold_make_room(DRamTileStore *store, size_t sz,
                                     int max_evict) {
    if (!store->cold_base) return -1;
    size_t off = (store->cold_used + 63) & ~63;
    if (off + sz <= store->cold_capacity) return 0; /* no eviction needed */

    int total = 0;
    int limit = max_evict > 0 ? max_evict : DT_HASH_SLOTS;
    while (total < limit) {
        int n = dt_evict_step(store, 1);
        if (n == 0) return -1;
        total += n;
        off = (store->cold_used + 63) & ~63;
        if (off + sz <= store->cold_capacity) return total;
    }
    return -1;
}

#endif /* DRAMTILE_STORE_H */

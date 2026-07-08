/*
 * pipe_backend.h — Backend Driver Interface
 *
 * Pluggable storage backends that implement the same interface.
 * GearShift calls backend->migrate() without knowing what the backend is.
 * Application swaps backend entirely without touching Pipe ABI.
 *
 * Backends:
 *   DRamTileBackend     — mmap'd DRAM (current, zero-copy)
 *   NVMeBackend         — direct NVMe I/O
 *   CXLBackend          — CXL-attached memory
 *   RemoteBackend       — network-attached (RDMA, etc.)
 *   SharedMemoryBackend — cross-process mmap
 *
 * All backends implement PipeBackend vtable identically.
 */

#ifndef PIPE_BACKEND_H
#define PIPE_BACKEND_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pipe_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
 * BACKEND SLOT — internal storage slot
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t    key;            /* RDH key for this slot */
    uint32_t    offset;         /* byte offset in backend */
    uint32_t    nbytes;         /* stored bytes */
    uint32_t    dtype;          /* PIPE_DTYPE_* */
    uint32_t    flags;          /* PIPE_SLOT_* flags */
    uint64_t    last_access;    /* monotonic tick for LRU */
    uint32_t    refcount;       /* active map references */
    uint32_t    tier;           /* current storage tier (0=hot, 1=warm, 2=cold) */
} BackendSlot;

/* ═══════════════════════════════════════════════════════════════
 * BACKEND STATISTICS
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t capacity_total;
    uint64_t capacity_used;
    uint32_t n_slots;
    uint32_t n_free;
    uint64_t n_reads;
    uint64_t n_writes;
    uint64_t n_migrates;
    uint64_t n_evictions;
    double   read_latency_ns;   /* average read latency */
    double   write_latency_ns;  /* average write latency */
} BackendStats;

/* ═══════════════════════════════════════════════════════════════
 * BACKEND DRIVER VTABLE
 * ═══════════════════════════════════════════════════════════════
 * Every backend implements this exact interface.
 * GearShift calls these without knowing the backend type.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct PipeBackend PipeBackend;

struct PipeBackend {
    /* Lifecycle */
    const char *name;                   /* backend name for logging */
    void       *impl;                   /* backend-specific state (RAMBackend, etc.) */
    void       *storage_base;           /* base pointer of storage region (for offset calc) */

    /* Initialize backend with capacity in bytes.
     * Returns 0 on success, negative on error. */
    int     (*init)(PipeBackend *b, uint64_t capacity, void *config);

    /* Destroy backend, free all resources. */
    void    (*destroy)(PipeBackend *b);

    /* Memory operations */
    void*   (*alloc)(PipeBackend *b, uint32_t nbytes, uint32_t alignment);
    void    (*free)(PipeBackend *b, void *ptr);
    void*   (*map)(PipeBackend *b, uint32_t offset, uint32_t nbytes);
    void    (*unmap)(PipeBackend *b, void *ptr, uint32_t nbytes);

    /* Data operations */
    int     (*read)(PipeBackend *b, uint32_t offset, void *dst, uint32_t nbytes);
    int     (*write)(PipeBackend *b, uint32_t offset, const void *src, uint32_t nbytes);

    /* Tiering operations */
    int     (*migrate)(PipeBackend *b, uint32_t src_offset,
                       uint32_t dst_tier, uint32_t nbytes);
    int     (*compact)(PipeBackend *b, uint32_t *bytes_reclaimed);

    /* Snapshot operations (Void Space) */
    int     (*snapshot)(PipeBackend *b, const char *path,
                        uint32_t slot_count, void *slot_data, uint32_t slot_size);
    int     (*restore)(PipeBackend *b, const char *path,
                       uint32_t *slot_count, void *slot_data, uint32_t slot_size);
    uint64_t (*snapshot_size)(PipeBackend *b);

    /* Status */
    void    (*stats)(PipeBackend *b, BackendStats *stats);
    int     (*healthy)(PipeBackend *b);
};

/* ═══════════════════════════════════════════════════════════════
 * BACKEND CONSTRUCTORS (defined below as static inline)
 * ═══════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════
 * BACKEND HELPER — generic implementation (for simple backends)
 * ═══════════════════════════════════════════════════════════════ */

/* Generic alloc: bump allocator (simple, no fragmentation).
 * Suitable for write-once-read-many workloads. */
static inline void *pipe_backend_bump_alloc(PipeBackend *b,
                                             uint32_t nbytes,
                                             uint32_t alignment,
                                             uint8_t **bump_ptr,
                                             uint64_t *bump_used,
                                             uint64_t capacity)
{
    (void)b;
    uintptr_t addr = (uintptr_t)*bump_ptr;
    uintptr_t aligned = (addr + alignment - 1) & ~(uintptr_t)(alignment - 1);
    uint32_t padding = (uint32_t)(aligned - addr);
    if (*bump_used + nbytes + padding > capacity) return NULL;
    *bump_ptr += padding + nbytes;
    *bump_used += padding + nbytes;
    return (void *)aligned;
}

/* ═══════════════════════════════════════════════════════════════
 * RAM BACKEND — malloc-based, no persistence
 * Simplest backend. For testing and in-memory workloads.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t    *base;           /* malloc'd buffer */
    uint64_t    capacity;
    uint64_t    used;
    uint8_t    *bump_ptr;       /* bump allocator pointer */
    BackendStats stats;
} RAMBackend;

static inline int ram_backend_init(PipeBackend *b, uint64_t capacity, void *config) {
    (void)config;
    RAMBackend *ram = (RAMBackend *)b->impl;
    ram->base = (uint8_t *)malloc((size_t)capacity);
    if (!ram->base) return -1;
    ram->capacity = capacity;
    ram->used = 0;
    ram->bump_ptr = ram->base;
    ram->stats.capacity_total = capacity;
    ram->stats.capacity_used = 0;
    ram->stats.n_slots = 0;
    b->storage_base = ram->base;
    return 0;
}

static inline void ram_backend_destroy(PipeBackend *b) {
    RAMBackend *ram = (RAMBackend *)b->impl;
    if (ram && ram->base) {
        free(ram->base);
        ram->base = NULL;
    }
}

static inline void *ram_backend_alloc(PipeBackend *b, uint32_t nbytes, uint32_t alignment) {
    RAMBackend *ram = (RAMBackend *)b->impl;
    void *ptr = pipe_backend_bump_alloc(b, nbytes, alignment,
                                         &ram->bump_ptr, &ram->used, ram->capacity);
    if (ptr) {
        ram->stats.capacity_used = ram->used;
        ram->stats.n_slots++;
    }
    return ptr;
}

static inline void ram_backend_free(PipeBackend *b, void *ptr) {
    (void)b; (void)ptr;
    /* bump allocator — no individual free */
}

static inline void *ram_backend_map(PipeBackend *b, uint32_t offset, uint32_t nbytes) {
    RAMBackend *ram = (RAMBackend *)b->impl;
    if (offset + nbytes > ram->capacity) return NULL;
    ram->stats.n_reads++;
    return ram->base + offset;
}

static inline void ram_backend_unmap(PipeBackend *b, void *ptr, uint32_t nbytes) {
    (void)b; (void)ptr; (void)nbytes;
    /* no-op for RAM */
}

static inline int ram_backend_read(PipeBackend *b, uint32_t offset, void *dst, uint32_t nbytes) {
    RAMBackend *ram = (RAMBackend *)b->impl;
    if (offset + nbytes > ram->capacity) return -1;
    memcpy(dst, ram->base + offset, nbytes);
    ram->stats.n_reads++;
    return 0;
}

static inline int ram_backend_write(PipeBackend *b, uint32_t offset, const void *src, uint32_t nbytes) {
    RAMBackend *ram = (RAMBackend *)b->impl;
    (void)offset;
    /* For bump allocator, write to current position */
    if (ram->used + nbytes > ram->capacity) return -1;
    memcpy(ram->bump_ptr, src, nbytes);
    ram->bump_ptr += nbytes;
    ram->used += nbytes;
    ram->stats.capacity_used = ram->used;
    ram->stats.n_writes++;
    return 0;
}

static inline int ram_backend_migrate(PipeBackend *b, uint32_t src_offset,
                                       uint32_t dst_tier, uint32_t nbytes) {
    (void)b; (void)src_offset; (void)dst_tier; (void)nbytes;
    /* RAM backend has only one tier — no migration needed */
    return 0;
}

static inline int ram_backend_compact(PipeBackend *b, uint32_t *bytes_reclaimed) {
    (void)b;
    if (bytes_reclaimed) *bytes_reclaimed = 0;
    return 0;
}

static inline void ram_backend_stats(PipeBackend *b, BackendStats *stats) {
    RAMBackend *ram = (RAMBackend *)b->impl;
    *stats = ram->stats;
}

static inline int ram_backend_healthy(PipeBackend *b) {
    RAMBackend *ram = (RAMBackend *)b->impl;
    return ram && ram->base != NULL;
}

/* ── RAM Backend Snapshot (Void Space) ──────────────────────── */

static inline uint64_t ram_backend_snapshot_size(PipeBackend *b) {
    RAMBackend *ram = (RAMBackend *)b->impl;
    return ram->used;
}

static inline int ram_backend_snapshot(PipeBackend *b, const char *path,
                                       uint32_t slot_count, void *slot_data, uint32_t slot_size)
{
    RAMBackend *ram = (RAMBackend *)b->impl;
    if (!path || !ram->base) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    /* Write PipeSnapshotHeader */
    PipeSnapshotHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = PIPE_SNAP_MAGIC;
    hdr.version = PIPE_SNAP_VERSION;
    hdr.n_slots = slot_count;
    hdr.slot_size = slot_size;
    hdr.slot_data_size = (uint64_t)slot_count * slot_size;
    hdr.backend_size = ram->used;
    hdr.tick = 0;  /* filled by pipe_checkpoint */
    hdr.total_size = sizeof(hdr) + (uint64_t)slot_count * slot_size + ram->used;

    fwrite(&hdr, sizeof(hdr), 1, f);

    /* Write slot table */
    if (slot_data && slot_count > 0) {
        fwrite(slot_data, slot_size, slot_count, f);
    }

    /* Write backend data */
    fwrite(ram->base, 1, (size_t)ram->used, f);

    fclose(f);
    return 0;
}

static inline int ram_backend_restore(PipeBackend *b, const char *path,
                                       uint32_t *slot_count, void *slot_data, uint32_t slot_size)
{
    RAMBackend *ram = (RAMBackend *)b->impl;
    if (!path) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    /* Read header */
    PipeSnapshotHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return -1; }
    if (hdr.magic != PIPE_SNAP_MAGIC || hdr.version != PIPE_SNAP_VERSION) {
        fclose(f); return -1;
    }

    /* Read slot table */
    if (slot_data && slot_count && hdr.n_slots > 0) {
        uint32_t to_read = hdr.n_slots;
        uint32_t avail = (uint32_t)(hdr.slot_data_size / slot_size);
        if (to_read > avail) to_read = avail;
        fread(slot_data, slot_size, to_read, f);
        *slot_count = to_read;
    }

    /* Read backend data */
    if (hdr.backend_size > 0 && ram->base) {
        size_t to_read = (size_t)hdr.backend_size;
        if (to_read > ram->capacity) to_read = (size_t)ram->capacity;
        fread(ram->base, 1, to_read, f);
        ram->used = to_read;
        ram->bump_ptr = ram->base + to_read;
        ram->stats.capacity_used = to_read;
    }

    fclose(f);
    return 0;
}

/* Backend instance storage */
static RAMBackend ram_backend_instance;

/* Constructor */
static inline PipeBackend *pipe_backend_ram_create(uint64_t capacity) {
    (void)capacity;  /* capacity passed to init() at pipe_open time */
    static PipeBackend backend = {0};
    memset(&backend, 0, sizeof(PipeBackend));
    memset(&ram_backend_instance, 0, sizeof(RAMBackend));

    backend.name      = "RAM";
    backend.impl      = &ram_backend_instance;
    backend.init      = ram_backend_init;
    backend.destroy   = ram_backend_destroy;
    backend.alloc     = ram_backend_alloc;
    backend.free      = ram_backend_free;
    backend.map       = ram_backend_map;
    backend.unmap     = ram_backend_unmap;
    backend.read      = ram_backend_read;
    backend.write     = ram_backend_write;
    backend.migrate   = ram_backend_migrate;
    backend.compact   = ram_backend_compact;
    backend.stats     = ram_backend_stats;
    backend.healthy   = ram_backend_healthy;
    backend.snapshot  = ram_backend_snapshot;
    backend.restore   = ram_backend_restore;
    backend.snapshot_size = ram_backend_snapshot_size;

    return &backend;
}

/* ═══════════════════════════════════════════════════════════════
 * DRamTile BACKEND — mmap'd file-backed persistent storage
 *
 * File layout (single mmap on F:\ or any path):
 *   [PipeSnapshotHeader]   ← at offset 0
 *   [PipeSlot × N]         ← slot table (persistent)
 *   [data region]          ← bump-allocated data
 *
 * Data survives process restarts — just re-mmap the file.
 * No explicit checkpoint needed for data persistence.
 * ═══════════════════════════════════════════════════════════════ */

/* Windows mmap headers */
#include <windows.h>

/* Max tracked allocations for compact defrag */
#define DT_MAX_TRACKED     4096

/* Growth increment (64 MB chunks) */
#define DT_GROW_CHUNK      (64UL * 1024 * 1024)

/* Minimal file size: slot offset gap + full slot table + 64KB data region.
   For PIPE_MAX_SLOTS=4096, slot_size~112: 64KB + 448KB + 64KB = 576KB */
#define DT_MIN_FILE_SIZE   (DT_SLOT_OFFSET + PIPE_MAX_SLOTS * sizeof(PipeSlot) + 64UL * 1024)

/* Minimal file size for the "file too small to restore" check (just the header+slots) */
#define DT_MIN_FILE_SIZE_FULL  (DT_SLOT_OFFSET + PIPE_MAX_SLOTS * sizeof(PipeSlot))

typedef struct {
    HANDLE       hFile;        /* file handle */
    HANDLE       hMapping;     /* file mapping handle */
    uint8_t     *base;         /* mmap base (entire file) */
    uint64_t     capacity;     /* max data bytes (unused — grow as needed) */
    uint64_t     used;         /* used data bytes */
    uint64_t     map_size;     /* current file/mapping size (grows as needed) */
    uint32_t     slot_offset;  /* offset from base to slot table */
    uint32_t     data_offset;  /* offset from base to data region */
    uint8_t     *data_base;    /* shortcut: base + data_offset */
    uint8_t     *bump_ptr;     /* current bump allocator position */
    uint32_t     max_slots;    /* max slot count */

    /*     Tracked allocations (for compact defrag) */
    uint32_t     n_tracked;
    struct {
        uint32_t offset;
        uint32_t size;
        uint32_t slot_idx;    /* which PipeSlot owns this (UINT32_MAX = orphan) */
    } tracked[DT_MAX_TRACKED];

    BackendStats stats;
    char         file_path[260];  /* backend file path (for snapshot/restore routing) */
} DRamTileBackend;

/* ── Slot table offset (right after header, 64KB aligned) ───── */

#define DT_SLOT_OFFSET 65536

/* ── Data offset = start of slot table + max slots * sizeof(PipeSlot) ── */

static inline uint32_t dt_data_offset(uint32_t max_slots) {
    return DT_SLOT_OFFSET + max_slots * sizeof(PipeSlot);
}

/* ── Internal: regrow mapping (extend file + remap) ──────────── */

static inline int dramtile_backend_regrow(PipeBackend *b, uint64_t needed) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    if (needed <= dt->map_size) return 0;  /* already big enough */

    uint64_t new_size = dt->map_size;
    while (new_size < needed) new_size += DT_GROW_CHUNK;

    /* Extend file on disk */
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)new_size;
    SetFilePointerEx(dt->hFile, li, NULL, FILE_BEGIN);
    SetEndOfFile(dt->hFile);

    /* Remap — close old, create new */
    if (dt->base) UnmapViewOfFile(dt->base);
    if (dt->hMapping) CloseHandle(dt->hMapping);

    dt->hMapping = CreateFileMappingA(dt->hFile, NULL, PAGE_READWRITE,
                                       (DWORD)(new_size >> 32), (DWORD)new_size, NULL);
    if (!dt->hMapping) return -1;

    dt->base = (uint8_t *)MapViewOfFile(dt->hMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!dt->base) { CloseHandle(dt->hMapping); dt->hMapping = NULL; return -1; }

    dt->map_size = new_size;
    dt->data_base = dt->base + dt->data_offset;
    dt->bump_ptr = dt->data_base + dt->used;
    b->storage_base = dt->data_base;
    return 0;
}

/* ── Init ──────────────────────────────────────────────────────── */

static inline int dramtile_backend_init(PipeBackend *b, uint64_t capacity, void *config) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    const char *path = (const char *)config;  /* file path */
    if (!path) return -1;
    (void)capacity;  /* grow on demand — no fixed capacity needed */

    dt->slot_offset = DT_SLOT_OFFSET;
    dt->data_offset = dt_data_offset(PIPE_MAX_SLOTS);
    dt->max_slots   = PIPE_MAX_SLOTS;

    /* Open or create file */
    dt->hFile = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dt->hFile == INVALID_HANDLE_VALUE) return -1;

    /* Get current file size */
    LARGE_INTEGER li;
    GetFileSizeEx(dt->hFile, &li);
    uint64_t cur_size = (uint64_t)li.QuadPart;

    /* If file is too small (new or truncated), extend to full size */
    if (cur_size < DT_MIN_FILE_SIZE_FULL) {
        cur_size = DT_MIN_FILE_SIZE;
        li.QuadPart = (LONGLONG)cur_size;
        SetFilePointerEx(dt->hFile, li, NULL, FILE_BEGIN);
        SetEndOfFile(dt->hFile);
    }

    /* Create file mapping */
    dt->map_size = cur_size;
    dt->hMapping = CreateFileMappingA(dt->hFile, NULL, PAGE_READWRITE,
                                       (DWORD)(cur_size >> 32), (DWORD)cur_size, NULL);
    if (!dt->hMapping) { CloseHandle(dt->hFile); dt->hFile = INVALID_HANDLE_VALUE; return -1; }

    /* Map */
    dt->base = (uint8_t *)MapViewOfFile(dt->hMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!dt->base) {
        CloseHandle(dt->hMapping); dt->hMapping = NULL;
        CloseHandle(dt->hFile);    dt->hFile = INVALID_HANDLE_VALUE;
        return -1;
    }

    dt->data_base = dt->base + dt->data_offset;
    dt->n_tracked = 0;

    /* Check if file has valid header */
    PipeSnapshotHeader *hdr = (PipeSnapshotHeader *)dt->base;
    if (hdr->magic == PIPE_SNAP_MAGIC && hdr->version == PIPE_SNAP_VERSION) {
        /* Existing file — restore used */
        dt->used = (uint64_t)hdr->backend_size;
    } else {
        /* Initialize new file */
        memset(dt->base, 0, (size_t)cur_size);
        hdr->magic   = PIPE_SNAP_MAGIC;
        hdr->version = PIPE_SNAP_VERSION;
        hdr->slot_size = sizeof(PipeSlot);
        dt->used = 0;
    }

    dt->bump_ptr = dt->data_base + dt->used;
    dt->stats.capacity_total = cur_size;
    dt->stats.capacity_used  = dt->used;
    /* Store file path for snapshot/restore routing */
    strncpy(dt->file_path, path, sizeof(dt->file_path) - 1);
    dt->file_path[sizeof(dt->file_path) - 1] = '\0';

    b->storage_base = dt->data_base;  /* offsets relative to data region */
    return 0;
}

/* ── Destroy ──────────────────────────────────────────────────────── */

static inline void dramtile_backend_destroy(PipeBackend *b) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    if (!dt) return;

    /* Update header with current state */
    PipeSnapshotHeader *hdr = (PipeSnapshotHeader *)dt->base;
    if (hdr) {
        hdr->backend_size = dt->used;
    }

    if (dt->base)     { UnmapViewOfFile(dt->base); dt->base = NULL; }
    if (dt->hMapping) { CloseHandle(dt->hMapping); dt->hMapping = NULL; }
    if (dt->hFile && dt->hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(dt->hFile); dt->hFile = INVALID_HANDLE_VALUE;
    }
    memset(dt, 0, sizeof(DRamTileBackend));
    dt->hFile = INVALID_HANDLE_VALUE;
}

/* ── Alloc (grow on demand) ──────────────────────────────────────── */

static inline void *dramtile_backend_alloc(PipeBackend *b, uint32_t nbytes, uint32_t alignment) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    if (!dt->data_base) return NULL;

    /* Align */
    uint32_t align = (alignment < 8) ? 8 : alignment;
    uintptr_t addr = (uintptr_t)dt->bump_ptr;
    uint32_t pad = (uint32_t)((align - (addr & (align - 1))) & (align - 1));

    uint32_t needed = dt->data_offset + dt->used + pad + nbytes;
    if (needed > dt->map_size) {
        /* Grow the file */
        if (dramtile_backend_regrow(b, needed + DT_GROW_CHUNK) != 0)
            return NULL;
    }

    uint8_t *ptr = dt->bump_ptr + pad;
    uint32_t offset = dt->used + pad;  /* relative to data_base */
    dt->bump_ptr = ptr + nbytes;
    dt->used = dt->used + pad + nbytes;
    dt->stats.capacity_used = dt->used;
    dt->stats.capacity_total = dt->map_size;
    dt->stats.n_writes++;

    /* Track this allocation */
    if (dt->n_tracked < DT_MAX_TRACKED) {
        dt->tracked[dt->n_tracked].offset   = offset;
        dt->tracked[dt->n_tracked].size     = nbytes;
        dt->tracked[dt->n_tracked].slot_idx = UINT32_MAX;
        dt->n_tracked++;
    }

    return ptr;
}

/* ── Free (mark as freed, space reclaimed on compact) ──────────── */

static inline void dramtile_backend_free(PipeBackend *b, void *ptr) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    if (!dt || !ptr) return;

    /* Find tracked allocation and mark as orphan (will be compacted away) */
    uint32_t off = (uint32_t)((uint8_t *)ptr - dt->data_base);  /* relative to data_base */
    for (uint32_t i = 0; i < dt->n_tracked; i++) {
        if (dt->tracked[i].offset == off) {
            dt->tracked[i].slot_idx = UINT32_MAX;  /* orphan — remove on compact */
            break;
        }
    }
    dt->stats.n_evictions++;
}

/* ── Map / Unmap ─────────────────────────────────────────────────── */

static inline void *dramtile_backend_map(PipeBackend *b, uint32_t offset, uint32_t nbytes) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    if (offset + nbytes > dt->used) return NULL;
    dt->stats.n_reads++;
    return dt->data_base + offset;  /* offset is relative to data_base */
}

static inline void dramtile_backend_unmap(PipeBackend *b, void *ptr, uint32_t nbytes) {
    (void)b; (void)ptr; (void)nbytes;
}

/* ── Read / Write ──────────────────────────────────────────────────── */

static inline int dramtile_backend_read(PipeBackend *b, uint32_t offset, void *dst, uint32_t nbytes) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    if (offset + nbytes > dt->used) return -1;
    memcpy(dst, dt->data_base + offset, nbytes);
    dt->stats.n_reads++;
    return 0;
}

static inline int dramtile_backend_write(PipeBackend *b, uint32_t offset, const void *src, uint32_t nbytes) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    if (offset + nbytes > dt->used) {
        /* Need to grow for this write */
        uint32_t needed = offset + nbytes;
        if (dramtile_backend_regrow(b, dt->data_offset + needed + DT_GROW_CHUNK) != 0) return -1;
        dt->used = offset + nbytes;
    }
    memcpy(dt->data_base + offset, src, nbytes);
    dt->stats.n_writes++;
    return 0;
}

/* ── Tiering / Compact ──────────────────────────────────────────────── */

static inline int dramtile_backend_migrate(PipeBackend *b, uint32_t src_offset,
                                            uint32_t dst_tier, uint32_t nbytes) {
    (void)b; (void)src_offset; (void)dst_tier; (void)nbytes;
    return 0;
}

/* ── Compact: defragment live data + truncate file to reclaim space ── */

static inline int dramtile_backend_compact(PipeBackend *b, uint32_t *bytes_reclaimed) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    if (!dt->data_base) return -1;

    uint32_t n_live = 0;
    uint32_t live_size = 0;

    /* Count live items (slot_idx != UINT32_MAX) */
    for (uint32_t i = 0; i < dt->n_tracked; i++) {
        if (dt->tracked[i].slot_idx != UINT32_MAX) {
            /* Move data to front (tracked.offset is relative to data_base) */
            if (n_live < i) {
                memmove(dt->data_base + live_size,
                        dt->data_base + dt->tracked[i].offset,
                        dt->tracked[i].size);
            }
            dt->tracked[i].offset = live_size;
            live_size += dt->tracked[i].size;
            if (n_live != i) {
                dt->tracked[n_live] = dt->tracked[i];
            }
            n_live++;
        }
    }

    uint64_t old_used = dt->used;
    dt->used = live_size;
    dt->bump_ptr = dt->data_base + live_size;
    dt->stats.capacity_used = dt->used;

    /* Truncate file if we saved more than one chunk */
    uint64_t saved = old_used - live_size;
    uint64_t new_map = dt->data_offset + live_size + DT_GROW_CHUNK;
    if (new_map + DT_GROW_CHUNK * 2 < dt->map_size) {
        /* Shrink to fit + one growth chunk of slack */
        uint64_t target = (new_map > DT_MIN_FILE_SIZE) ? new_map : DT_MIN_FILE_SIZE;

        if (dt->base) { UnmapViewOfFile(dt->base); dt->base = NULL; }
        if (dt->hMapping) { CloseHandle(dt->hMapping); dt->hMapping = NULL; }

        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)target;
        SetFilePointerEx(dt->hFile, li, NULL, FILE_BEGIN);
        SetEndOfFile(dt->hFile);

        dt->hMapping = CreateFileMappingA(dt->hFile, NULL, PAGE_READWRITE,
                                           (DWORD)(target >> 32), (DWORD)target, NULL);
        if (dt->hMapping) {
            dt->base = (uint8_t *)MapViewOfFile(dt->hMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        }
        if (!dt->base) {
            /* Remap failed — remap at old size */
            li.QuadPart = (LONGLONG)dt->map_size;
            SetFilePointerEx(dt->hFile, li, NULL, FILE_BEGIN);
            SetEndOfFile(dt->hFile);
            dt->hMapping = CreateFileMappingA(dt->hFile, NULL, PAGE_READWRITE,
                                               (DWORD)(dt->map_size >> 32), (DWORD)dt->map_size, NULL);
            if (dt->hMapping) dt->base = (uint8_t *)MapViewOfFile(dt->hMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        }
        dt->map_size = target;
        b->storage_base = dt->base;
    }

    dt->data_base = dt->base ? dt->base + dt->data_offset : NULL;
    dt->n_tracked = n_live;

    /* Update header with new used size */
    PipeSnapshotHeader *hdr = (PipeSnapshotHeader *)dt->base;
    if (hdr) hdr->backend_size = dt->used;

    if (bytes_reclaimed) *bytes_reclaimed = (uint32_t)saved;
    return 0;
}

static inline void dramtile_backend_stats(PipeBackend *b, BackendStats *stats) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    *stats = dt->stats;
}

static inline int dramtile_backend_healthy(PipeBackend *b) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    return dt && dt->base != NULL && dt->hMapping != NULL;
}

/* ── Snapshot / Restore (Void Space) ──────────────────────────────── */

static inline uint64_t dramtile_backend_snapshot_size(PipeBackend *b) {
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    return dt->used;
}

static inline int dramtile_backend_snapshot(PipeBackend *b, const char *path,
                                            uint32_t slot_count, void *slot_data, uint32_t slot_size)
{
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    if (!dt->base) return -1;

    /* Update header with current state */
    PipeSnapshotHeader *hdr = (PipeSnapshotHeader *)dt->base;
    hdr->n_slots       = slot_count;
    hdr->slot_size     = slot_size;
    hdr->slot_data_size  = (uint64_t)slot_count * slot_size;
    hdr->backend_size  = dt->used;
    hdr->tick++;
    hdr->total_size    = sizeof(*hdr) + hdr->slot_data_size + dt->used;

    /* Copy slot table into mmap */
    if (slot_data && slot_count > 0) {
        uint64_t copy_sz = (uint64_t)slot_count * slot_size;
        memcpy(dt->base + dt->slot_offset, slot_data, (size_t)copy_sz);
    }

    /* If path is different from our file, write a compact snapshot file */
    if (path && path[0] && strcmp(path, dt->file_path) != 0) {
        FILE *f = fopen(path, "wb");
        if (f) {
            PipeSnapshotHeader out = *hdr;
            fwrite(&out, sizeof(out), 1, f);
            if (slot_data && slot_count > 0)
                fwrite(slot_data, slot_size, slot_count, f);
            fclose(f);
        }
    }
    return 0;
}

static inline int dramtile_backend_restore(PipeBackend *b, const char *path,
                                           uint32_t *slot_count, void *slot_data, uint32_t slot_size)
{
    DRamTileBackend *dt = (DRamTileBackend *)b->impl;
    if (!dt->base || !slot_data || !slot_count) return -1;

    /* Different file path → read compact snapshot file */
    if (path && path[0] && strcmp(path, dt->file_path) != 0) {
        FILE *f = fopen(path, "rb");
        if (!f) return -1;
        PipeSnapshotHeader hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
            hdr.magic != PIPE_SNAP_MAGIC ||
            hdr.version != PIPE_SNAP_VERSION) {
            fclose(f); return -1;
        }
        uint32_t to_read = hdr.n_slots;
        uint32_t max_read = (uint32_t)(hdr.slot_data_size / slot_size);
        if (to_read > max_read) to_read = max_read;
        if (to_read > 0) {
            size_t sz = (size_t)to_read * slot_size;
            if (fread(slot_data, 1, sz, f) == sz)
                *slot_count = to_read;
        }
        fclose(f);
        return 0;
    }

    /* Same file → read from mmap (gap layout: header at 0, slots at slot_offset) */
    PipeSnapshotHeader *hdr = (PipeSnapshotHeader *)dt->base;
    if (hdr->magic != PIPE_SNAP_MAGIC || hdr->version != PIPE_SNAP_VERSION)
        return -1;
    if (hdr->n_slots == 0) return 0;

    uint32_t to_read = hdr->n_slots;
    uint32_t max_read = (uint32_t)(hdr->slot_data_size / slot_size);
    if (to_read > max_read) to_read = max_read;
    if (to_read > 0) {
        memcpy(slot_data, dt->base + dt->slot_offset, (size_t)to_read * slot_size);
        *slot_count = to_read;
    }

    dt->used = (uint64_t)hdr->backend_size;
    dt->bump_ptr = dt->data_base + (size_t)dt->used;
    dt->stats.capacity_used = dt->used;

    dt->n_tracked = 0;
    PipeSlot *slots = (PipeSlot *)slot_data;
    for (uint32_t i = 0; i < *slot_count && dt->n_tracked < DT_MAX_TRACKED; i++) {
        if (slots[i].key != 0) {
            dt->tracked[dt->n_tracked].offset   = slots[i].backend_offset;
            dt->tracked[dt->n_tracked].size     = slots[i].nbytes;
            dt->tracked[dt->n_tracked].slot_idx = i;
            dt->n_tracked++;
        }
    }
    return 0;
}

/* ── Constructor ──────────────────────────────────────────────────── */

static DRamTileBackend dramtile_backend_instance;

static inline PipeBackend *pipe_backend_dramtile_create(const char *path, uint64_t capacity) {
    (void)path; (void)capacity;
    static PipeBackend backend = {0};
    memset(&backend, 0, sizeof(PipeBackend));
    memset(&dramtile_backend_instance, 0, sizeof(DRamTileBackend));
    dramtile_backend_instance.hFile = INVALID_HANDLE_VALUE;

    backend.name      = "DRamTile";
    backend.impl      = &dramtile_backend_instance;
    backend.init      = dramtile_backend_init;
    backend.destroy   = dramtile_backend_destroy;
    backend.alloc     = dramtile_backend_alloc;
    backend.free      = dramtile_backend_free;
    backend.map       = dramtile_backend_map;
    backend.unmap     = dramtile_backend_unmap;
    backend.read      = dramtile_backend_read;
    backend.write     = dramtile_backend_write;
    backend.migrate   = dramtile_backend_migrate;
    backend.compact   = dramtile_backend_compact;
    backend.stats     = dramtile_backend_stats;
    backend.healthy   = dramtile_backend_healthy;
    backend.snapshot  = dramtile_backend_snapshot;
    backend.restore   = dramtile_backend_restore;
    backend.snapshot_size = dramtile_backend_snapshot_size;

    return &backend;
}

#ifdef __cplusplus
}
#endif

#endif /* PIPE_BACKEND_H */

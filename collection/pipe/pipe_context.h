/*
 * pipe_context.h — Pipe Runtime Context
 *
 * The main runtime object. Contains all subsystems:
 *   Radial → detect
 *   RDH    → coordinate + key generation
 *   DRamTile → memory mapping + zero-copy
 *   GearShift → placement + migration + priority
 *
 * Application calls pipe_open() → gets PipeContext* → calls pipe_write/read/map.
 * PipeContext holds the entire state. Multiple pipes can coexist.
 *
 * Usage:
 *   PipeContext *ctx = pipe_open(&config);
 *   pipe_write(ctx, "weight_0", data, size, PIPE_DTYPE_F32, PIPE_PRIO_NORMAL);
 *   const void *ptr = pipe_read(ctx, "weight_0", &size);
 *   pipe_compact(ctx);
 *   pipe_close(ctx);
 */

#ifndef PIPE_CONTEXT_H
#define PIPE_CONTEXT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "pipe_abi.h"
#include "pipe_backend.h"

/* Radial Detect — 24-vertex × 7-node sensor mesh */
#include "../../runner/geo_radial_capture.h"

/* RDH Coordinate — infinite-scale mixed-radix addressing */
#include "../../collection/rdh/rdh_addr.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
 * RADIAL CONTEXT (detect subsystem)
 * ═══════════════════════════════════════════════════════════════
 * Wraps geo_radial_capture.h — fixed 24-vertex × 7-node sensor.
 * Maps any name/data → (vertex, node) radial address.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t    n_captures;     /* total captures performed */
    uint32_t    n_names;        /* names captured this context */
} RadialContext;

/* ═══════════════════════════════════════════════════════════════
 * RDH CONTEXT (coordinate subsystem)
 * ═══════════════════════════════════════════════════════════════
 * Wraps rdh_addr.h — infinite-scale mixed-radix coordinate system.
 * Translates radial addresses → flat RDH keys.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    RDHConfig    cfg;            /* RDH dimensions (n_rings, n_wedges, etc.) */
    int64_t      capacity;       /* total address space */
    uint64_t     n_translations; /* total translations */
} RDHContext;

/* ═══════════════════════════════════════════════════════════════
 * TILE CONTEXT (memory mapping subsystem)
 * ═══════════════════════════════════════════════════════════════
 * Wraps DRamTile — receives translated key, provides mmap pointer.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t    *base;           /* mmap base pointer */
    uint64_t    capacity;       /* total bytes */
    uint64_t    used;           /* bytes in use */
    uint32_t    n_mappings;     /* active map references */
} TileContext;

/* ═══════════════════════════════════════════════════════════════
 * GEAR CONTEXT (placement + priority subsystem)
 * ═══════════════════════════════════════════════════════════════
 * Wraps GearShift + GearLock — tier routing, migration, prefetch.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t    n_lanes;        /* priority lanes (4 by default) */
    uint32_t    active_lane;    /* currently streaming lane */
    uint64_t    n_migrations;   /* total tier migrations */
    uint64_t    n_prefetches;   /* total prefetch operations */
    uint32_t    speed_limit;    /* operations per tick (0 = unlimited) */
} GearContext;

/* ═══════════════════════════════════════════════════════════════
 * PIPE CONTEXT — Main Runtime Object
 * ═══════════════════════════════════════════════════════════════
 * Contains all subsystems. All pipe_*() functions take this as
 * first argument. Passed by pointer → no copy, no lookup.
 * ═══════════════════════════════════════════════════════════════ */

struct PipeContext {
    /* Identity */
    uint32_t    magic;          /* PIPE_MAGIC */
    uint32_t    version;        /* PIPE_VERSION */
    char        name[64];       /* pipe name */

    /* Subsystems */
    RadialContext  radial;      /* detect */
    RDHContext     rdh;         /* coordinate */
    TileContext    tile;        /* memory mapping */
    GearContext    gear;        /* placement + priority */

    /* Backend */
    PipeBackend   *backend;     /* pluggable storage driver */

    /* Slot table */
    PipeSlot      *slots;       /* slot array */
    uint32_t       n_slots;     /* active slots */
    uint32_t       max_slots;   /* slot array capacity */

    /* Statistics */
    PipeStats     stats;

    /* State */
    int            is_open;     /* 1 if active, 0 if closed */
    uint64_t       tick;        /* monotonic counter */
};

/* ═══════════════════════════════════════════════════════════════
 * PIPE ABI IMPLEMENTATION
 * ═══════════════════════════════════════════════════════════════ */

/* ── Forward declarations ──────────────────────────────────── */

static inline int pipe_flush(PipeContext *ctx);
static inline uint64_t pipe_compact(PipeContext *ctx);
static inline int pipe_restore(PipeContext *ctx, const char *path);
static inline int pipe_snapshot_info(const char *path, PipeSnapshotHeader *hdr);

/* ── pipe_open ──────────────────────────────────────────────── */

static inline PipeContext *pipe_open(const PipeConfig *config) {
    if (!config) return NULL;

    PipeContext *ctx = (PipeContext *)calloc(1, sizeof(PipeContext));
    if (!ctx) return NULL;

    /* Identity */
    ctx->magic = PIPE_MAGIC;
    ctx->version = PIPE_VERSION;
    if (config->name) {
        strncpy(ctx->name, config->name, sizeof(ctx->name) - 1);
    } else {
        strncpy(ctx->name, "pipe", sizeof(ctx->name) - 1);
    }
    ctx->is_open = 1;
    ctx->tick = 0;

    /* Radial context (always initialized, no allocation needed) */
    memset(&ctx->radial, 0, sizeof(RadialContext));

    /* RDH context — default to tier0: {128, 162, 1, 1, 1} */
    ctx->rdh.cfg.n_rings   = 128;
    ctx->rdh.cfg.n_wedges  = 162;
    ctx->rdh.cfg.n_mirror  = 1;
    ctx->rdh.cfg.max_u     = 1;
    ctx->rdh.cfg.n_v       = 1;
    ctx->rdh.capacity  = ctx->rdh.cfg.n_rings * ctx->rdh.cfg.n_wedges
                        * ctx->rdh.cfg.n_mirror * ctx->rdh.cfg.max_u * ctx->rdh.cfg.n_v;

    /* Tile context — backend will fill this */
    memset(&ctx->tile, 0, sizeof(TileContext));

    /* Gear context — 4 priority lanes */
    ctx->gear.n_lanes = config->n_priority > 0 ? config->n_priority : PIPE_PRIO_COUNT;
    ctx->gear.speed_limit = 0;

    /* Backend — choose based on config */
    uint64_t capacity = config->capacity > 0 ? config->capacity : PIPE_DEFAULT_CAPACITY;
    if (config->backend_path && config->backend_path[0]) {
        /* DRamTile mmap-backed persistent storage */
        ctx->backend = pipe_backend_dramtile_create(config->backend_path, capacity);
        if (!ctx->backend) {
            free(ctx);
            return NULL;
        }
        if (ctx->backend->init(ctx->backend, capacity, (void *)config->backend_path) != 0) {
            ctx->backend->destroy(ctx->backend);
            free(ctx);
            return NULL;
        }
    } else {
        /* RAM backend (default — volatile, malloc'd) */
        ctx->backend = pipe_backend_ram_create(capacity);
        if (!ctx->backend) {
            free(ctx);
            return NULL;
        }
        if (ctx->backend->init(ctx->backend, capacity, config->backend_config) != 0) {
            ctx->backend->destroy(ctx->backend);
            free(ctx);
            return NULL;
        }
    }

    /* Slot table */
    ctx->max_slots = config->max_slots > 0 ? config->max_slots : PIPE_MAX_SLOTS;
    ctx->slots = (PipeSlot *)calloc(ctx->max_slots, sizeof(PipeSlot));
    if (!ctx->slots) {
        ctx->backend->destroy(ctx->backend);
        free(ctx);
        return NULL;
    }
    ctx->n_slots = 0;

    /* Auto-restore for DRamTile backend — if file has existing data, restore slots */
    if (config->backend_path && config->backend_path[0]) {
        PipeSnapshotHeader hdr;
        if (pipe_snapshot_info(config->backend_path, &hdr) == PIPE_OK && hdr.n_slots > 0) {
            pipe_restore(ctx, config->backend_path);
        }
    }

    /* Stats */
    memset(&ctx->stats, 0, sizeof(PipeStats));
    ctx->stats.capacity_total = capacity;

    return ctx;
}

/* ── pipe_flush (forward decl — defined after pipe_write) ───── */

static inline int pipe_flush(PipeContext *ctx);

/* ── pipe_restore / pipe_snapshot_info (forward decl — defined in Void Space section) ── */

static inline int pipe_restore(PipeContext *ctx, const char *path);
static inline int pipe_snapshot_info(const char *path, PipeSnapshotHeader *hdr);

/* ── pipe_compact (forward decl — defined after pipe_flush) ─── */

static inline uint64_t pipe_compact(PipeContext *ctx);

/* ── pipe_close (forward decl — defined below) ──────────────── */

static inline void pipe_close(PipeContext *ctx);

/* ═══════════════════════════════════════════════════════════════
 * VOID SPACE — Checkpoint / Restore / Recreate
 * ═══════════════════════════════════════════════════════════════
 * When runtime gets messy:
 *   1. pipe_checkpoint() — save persistent state to file
 *   2. pipe_destroy()    — dispose runtime
 *   3. pipe_recreate()   — fresh runtime + restore from file
 *
 * Application doesn't know runtime was recreated.
 * ═══════════════════════════════════════════════════════════════ */

/* pipe_checkpoint — Save persistent state to file.
 * Captures: slot table + backend data.
 * Returns PIPE_OK or error code. */
static inline int pipe_checkpoint(PipeContext *ctx, const char *path) {
    if (!ctx || !ctx->is_open || !path) return PIPE_ERR_BADARG;
    if (!ctx->backend || !ctx->backend->snapshot) return PIPE_ERR_BACKEND;

    /* Flush dirty data first */
    pipe_flush(ctx);

    /* Set tick in snapshot header (done inside backend snapshot) */
    ctx->tick++;

    /* Delegate to backend snapshot */
    int rc = ctx->backend->snapshot(ctx->backend, path,
                                     ctx->n_slots, ctx->slots, sizeof(PipeSlot));
    return rc == 0 ? PIPE_OK : PIPE_ERR_BACKEND;
}

/* pipe_restore — Rebuild runtime from snapshot file.
 * Returns PIPE_OK or error code. */
static inline int pipe_restore(PipeContext *ctx, const char *path) {
    if (!ctx || !ctx->is_open || !path) return PIPE_ERR_BADARG;
    if (!ctx->backend || !ctx->backend->restore) return PIPE_ERR_BACKEND;

    /* Clear current state */
    for (uint32_t i = 0; i < ctx->max_slots; i++) {
        ctx->slots[i].key = 0;
    }
    ctx->n_slots = 0;

    /* Delegate to backend restore */
    int rc = ctx->backend->restore(ctx->backend, path,
                                    &ctx->n_slots, ctx->slots, sizeof(PipeSlot));
    if (rc != 0) return PIPE_ERR_BACKEND;

    /* Rebuild stats from restored slots */
    ctx->stats.n_slots = ctx->n_slots;
    ctx->stats.n_dirty = 0;
    ctx->stats.n_pinned = 0;
    for (uint32_t i = 0; i < ctx->n_slots; i++) {
        if (ctx->slots[i].flags & PIPE_SLOT_DIRTY) ctx->stats.n_dirty++;
        if (ctx->slots[i].flags & PIPE_SLOT_PINNED) ctx->stats.n_pinned++;
    }

    return PIPE_OK;
}

/* pipe_recreate — Atomic destroy + create + restore.
 * Disposes current runtime, creates fresh one, restores from snapshot.
 * Application's pipe pointer becomes invalid — must use returned pointer.
 * Returns new PipeContext* or NULL on failure. */
static inline PipeContext *pipe_recreate(PipeContext *old_ctx, const char *snapshot_path) {
    if (!old_ctx || !snapshot_path) return NULL;

    /* Capture config before destroying */
    PipeConfig config = PIPE_CONFIG_DEFAULT;
    config.name = old_ctx->name;
    config.capacity = old_ctx->stats.capacity_total;
    config.max_slots = old_ctx->max_slots;

    /* Save backend reference and name */
    char saved_name[64];
    strncpy(saved_name, old_ctx->name, sizeof(saved_name));

    /* Destroy old runtime (but don't free the snapshot — it's on disk) */
    pipe_close(old_ctx);

    /* Create fresh runtime */
    PipeContext *new_ctx = pipe_open(&config);
    if (!new_ctx) return NULL;

    /* Restore from snapshot */
    int rc = pipe_restore(new_ctx, snapshot_path);
    if (rc != PIPE_OK) {
        /* Restore failed — return fresh empty context */
        fprintf(stderr, "[pipe] recreate: restore failed (%d), returning fresh context\n", rc);
    }

    return new_ctx;
}

/* pipe_snapshot_exists — Check if a snapshot file exists and is valid. */
static inline int pipe_snapshot_exists(const char *path) {
    if (!path) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    PipeSnapshotHeader hdr;
    int ok = (fread(&hdr, sizeof(hdr), 1, f) == 1)
          && (hdr.magic == PIPE_SNAP_MAGIC)
          && (hdr.version == PIPE_SNAP_VERSION);
    fclose(f);
    return ok;
}

/* pipe_snapshot_info — Read snapshot header without restoring. */
static inline int pipe_snapshot_info(const char *path, PipeSnapshotHeader *hdr) {
    if (!path || !hdr) return PIPE_ERR_BADARG;

    FILE *f = fopen(path, "rb");
    if (!f) return PIPE_ERR_NOTFOUND;

    int ok = (fread(hdr, sizeof(*hdr), 1, f) == 1);
    fclose(f);

    if (!ok || hdr->magic != PIPE_SNAP_MAGIC || hdr->version != PIPE_SNAP_VERSION) {
        return PIPE_ERR_BACKEND;
    }
    return PIPE_OK;
}

/* ── pipe_close ─────────────────────────────────────────────── */

static inline void pipe_close(PipeContext *ctx) {
    if (!ctx || !ctx->is_open) return;

    /* Flush before close */
    pipe_flush(ctx);

    /* Release slot names */
    for (uint32_t i = 0; i < ctx->max_slots; i++) {
        ctx->slots[i].key = 0;
    }

    /* Destroy backend */
    if (ctx->backend) {
        ctx->backend->destroy(ctx->backend);
        ctx->backend = NULL;
    }

    /* Free slot table */
    free(ctx->slots);
    ctx->slots = NULL;

    ctx->is_open = 0;
    free(ctx);
}

/* ── pipe_write ─────────────────────────────────────────────── */

static inline int pipe_write(PipeContext *ctx, const char *name,
                             const void *data, uint32_t nbytes,
                             uint32_t dtype, uint32_t priority)
{
    if (!ctx || !ctx->is_open) return PIPE_ERR_CLOSED;
    if (!name || !data || nbytes == 0) return PIPE_ERR_BADARG;
    if (priority >= PIPE_PRIO_COUNT) return PIPE_ERR_BADARG;

    /* Check for duplicate */
    for (uint32_t i = 0; i < ctx->n_slots; i++) {
        if (ctx->slots[i].key != 0 && strcmp(ctx->slots[i].name, name) == 0) {
            return PIPE_ERR_DUPLICATE;
        }
    }

    /* Find free slot */
    if (ctx->n_slots >= ctx->max_slots) return PIPE_ERR_FULL;
    uint32_t slot_idx = ctx->n_slots;

    /* ═══ Radial Detect: name → (vertex, node) address ═══ */
    uint64_t radial_addr = rc_capture_name(name);

    /* ═══ RDH Coordinate: radial → flat key ═══ */
    /* Decompose radial address into RDH params */
    int vertex = (int)(radial_addr / RC_N_NODES);  /* 0..23 */
    int node   = (int)(radial_addr % RC_N_NODES);  /* 0..6 */

    /* Map to RDH dimensions: ring≈layer, wedge≈vertex, mirror≈node parity */
    int64_t ring   = (int64_t)(vertex % ctx->rdh.cfg.n_rings);
    int64_t wedge  = (int64_t)(vertex / ctx->rdh.cfg.n_rings) % ctx->rdh.cfg.n_wedges;
    int64_t mirror = (int64_t)(node % ctx->rdh.cfg.n_mirror);
    int64_t u      = (int64_t)((vertex * 7 + node) % ctx->rdh.cfg.max_u);
    int64_t key = rdh_key(&ctx->rdh.cfg, ring, wedge, mirror, u, 0);
    ctx->rdh.n_translations++;

    /* DRamTile: allocate in backend */
    void *backend_ptr = ctx->backend->alloc(ctx->backend, nbytes, 32);
    if (!backend_ptr) return PIPE_ERR_NOMEM;

    /* Copy data directly to allocated pointer (zero-copy path coming soon) */
    memcpy(backend_ptr, data, nbytes);

    /* Compute backend offset from pointer (backend-agnostic via storage_base) */
    uint32_t backend_offset = (uint32_t)((uint8_t *)backend_ptr - (uint8_t *)ctx->backend->storage_base);

    /* Fill slot */
    PipeSlot *slot = &ctx->slots[slot_idx];
    memset(slot, 0, sizeof(PipeSlot));
    slot->key       = (uint64_t)key;
    slot->priority  = priority;
    slot->dtype     = dtype;
    slot->nbytes    = nbytes;
    slot->backend_offset = backend_offset;
    slot->radial_addr = radial_addr;
    slot->flags     = PIPE_SLOT_DIRTY;
    strncpy(slot->name, name, sizeof(slot->name) - 1);

    ctx->n_slots++;
    ctx->tick++;

    /* Update stats */
    ctx->stats.n_writes++;
    ctx->stats.n_slots = ctx->n_slots;

    return PIPE_OK;
}

/* ── pipe_read ──────────────────────────────────────────────── */

static inline const void *pipe_read(PipeContext *ctx, const char *name,
                                    uint32_t *nbytes)
{
    if (!ctx || !ctx->is_open) return NULL;
    if (!name) return NULL;

    /* Linear search by name */
    for (uint32_t i = 0; i < ctx->n_slots; i++) {
        if (ctx->slots[i].key != 0 && strcmp(ctx->slots[i].name, name) == 0) {
            if (nbytes) *nbytes = ctx->slots[i].nbytes;
            ctx->slots[i].flags |= PIPE_SLOT_DIRTY;  /* mark accessed */
            ctx->stats.n_reads++;

            /* Map from backend (zero-copy) */
            return ctx->backend->map(ctx->backend, ctx->slots[i].backend_offset, ctx->slots[i].nbytes);
        }
    }
    return NULL;
}

/* ── pipe_map ───────────────────────────────────────────────── */

static inline const void *pipe_map(PipeContext *ctx, uint64_t key) {
    if (!ctx || !ctx->is_open) return NULL;

    for (uint32_t i = 0; i < ctx->n_slots; i++) {
        if (ctx->slots[i].key == key) {
            ctx->slots[i].refcount++;
            ctx->stats.n_reads++;
            return ctx->backend->map(ctx->backend, ctx->slots[i].backend_offset, ctx->slots[i].nbytes);
        }
    }
    return NULL;
}

/* ── pipe_unmap ─────────────────────────────────────────────── */

static inline void pipe_unmap(PipeContext *ctx, const void *ptr) {
    if (!ctx || !ptr) return;
    /* For RAM backend, unmap is a no-op (bump allocator) */
    (void)ctx;
}

/* ── pipe_stream ────────────────────────────────────────────── */

static inline int pipe_stream(PipeContext *ctx, const char *name,
                              PipeStreamCallback callback, void *user)
{
    if (!ctx || !ctx->is_open || !callback) return PIPE_ERR_BADARG;

    /* Find the slot */
    for (uint32_t i = 0; i < ctx->n_slots; i++) {
        if (ctx->slots[i].key != 0 && strcmp(ctx->slots[i].name, name) == 0) {
            PipeSlot *slot = &ctx->slots[i];
            slot->flags |= PIPE_SLOT_STREAMING;

            /* Stream in priority order (single item for now) */
            PipeStreamChunk chunk;
            chunk.data     = ctx->backend->map(ctx->backend, slot->backend_offset, slot->nbytes);
            chunk.nbytes   = slot->nbytes;
            chunk.priority = slot->priority;
            chunk.index    = 0;
            chunk.user     = user;

            int result = callback(&chunk, user);
            slot->flags &= ~PIPE_SLOT_STREAMING;
            return result;
        }
    }
    return PIPE_ERR_NOTFOUND;
}

/* ── pipe_flush ─────────────────────────────────────────────── */

static inline int pipe_flush(PipeContext *ctx) {
    if (!ctx || !ctx->is_open) return 0;

    int flushed = 0;
    for (uint32_t i = 0; i < ctx->n_slots; i++) {
        if (ctx->slots[i].key != 0 && (ctx->slots[i].flags & PIPE_SLOT_DIRTY)) {
            ctx->slots[i].flags &= ~PIPE_SLOT_DIRTY;
            flushed++;
        }
    }
    ctx->stats.n_dirty = 0;
    return flushed;
}

/* ── pipe_compact ───────────────────────────────────────────── */

static inline uint64_t pipe_compact(PipeContext *ctx) {
    if (!ctx || !ctx->is_open) return 0;

    uint64_t reclaimed = 0;

    /* Phase 1: Remove empty slots */
    uint32_t write_idx = 0;
    for (uint32_t i = 0; i < ctx->n_slots; i++) {
        if (ctx->slots[i].key != 0) {
            if (write_idx != i) {
                ctx->slots[write_idx] = ctx->slots[i];
            }
            write_idx++;
        } else {
            reclaimed += sizeof(PipeSlot);
        }
    }
    ctx->n_slots = write_idx;

    /* Phase 2: Backend compact */
    if (ctx->backend && ctx->backend->compact) {
        uint32_t backend_reclaimed = 0;
        ctx->backend->compact(ctx->backend, &backend_reclaimed);
        reclaimed += backend_reclaimed;
    }

    ctx->stats.n_compacts++;
    return reclaimed;
}

/* ── pipe_stats ─────────────────────────────────────────────── */

static inline void pipe_stats(PipeContext *ctx, PipeStats *stats) {
    if (!ctx || !stats) return;
    *stats = ctx->stats;
    stats->n_slots = ctx->n_slots;
    stats->n_dirty = 0;
    stats->n_pinned = 0;
    for (uint32_t i = 0; i < ctx->n_slots; i++) {
        if (ctx->slots[i].flags & PIPE_SLOT_DIRTY) stats->n_dirty++;
        if (ctx->slots[i].flags & PIPE_SLOT_PINNED) stats->n_pinned++;
    }
}

/* ── pipe_find ──────────────────────────────────────────────── */

static inline int pipe_find(PipeContext *ctx, const char *name) {
    if (!ctx || !name) return 0;
    for (uint32_t i = 0; i < ctx->n_slots; i++) {
        if (ctx->slots[i].key != 0 && strcmp(ctx->slots[i].name, name) == 0)
            return 1;
    }
    return 0;
}

/* ── pipe_remove ────────────────────────────────────────────── */

static inline int pipe_remove(PipeContext *ctx, const char *name) {
    if (!ctx || !name) return PIPE_ERR_BADARG;
    for (uint32_t i = 0; i < ctx->max_slots; i++) {
        if (ctx->slots[i].key != 0 && strcmp(ctx->slots[i].name, name) == 0) {
            /* Free backend data — pass actual data pointer for tracked cleanup */
            if (ctx->backend && ctx->backend->free) {
                void *ptr = ctx->backend->map(ctx->backend,
                                               ctx->slots[i].backend_offset,
                                               ctx->slots[i].nbytes);
                ctx->backend->free(ctx->backend, ptr);
            }
            /* Move last active slot into this hole, then clear last */
            if ((int)(ctx->n_slots - 1) >= 0) {
                ctx->slots[i] = ctx->slots[ctx->n_slots - 1];
                memset(&ctx->slots[ctx->n_slots - 1], 0, sizeof(PipeSlot));
            } else {
                memset(&ctx->slots[i], 0, sizeof(PipeSlot));
            }
            ctx->n_slots--;
            return PIPE_OK;
        }
    }
    return PIPE_ERR_NOTFOUND;
}

/* ── pipe_count ─────────────────────────────────────────────── */

static inline uint32_t pipe_count(PipeContext *ctx) {
    return ctx ? ctx->n_slots : 0;
}

/* ── pipe_name ──────────────────────────────────────────────── */

static inline const char *pipe_name(PipeContext *ctx) {
    return ctx ? ctx->name : "(null)";
}

#ifdef __cplusplus
}
#endif

#endif /* PIPE_CONTEXT_H */

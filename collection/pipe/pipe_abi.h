/*
 * pipe_abi.h — Pipe Application Binary Interface
 *
 * The unified ABI for all applications (Kokoro TTS, moondream2 VLM, LLM,
 * ComfyUI, Whisper, etc.) to interact with the Memory Runtime.
 *
 * Applications know ONLY this interface. They do not know whether data
 * lives in RAM, VRAM, SSD, CXL, or Remote Node.
 *
 * Philosophy:
 *   pipe_open()   — acquire resource
 *   pipe_write()  — store data (Radial detect → RDH coordinate → DRamTile map)
 *   pipe_read()   — retrieve data (RDH lookup → zero-copy pointer)
 *   pipe_map()    — zero-copy pointer access
 *   pipe_stream() — priority-routed streaming
 *   pipe_flush()  — force pending writes to backend
 *   pipe_compact()— reduce footprint (NOT collapse — preserve data)
 *   pipe_close()  — release resource
 *
 * All operations are O(1) or O(n) where n = items in current context.
 * No hidden allocation in hot path. No implicit I/O.
 */

#ifndef PIPE_ABI_H
#define PIPE_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════ */

#define PIPE_MAGIC          0x50495045  /* "PIPE" */
#define PIPE_VERSION        1
#define PIPE_MAX_NAME       256
#define PIPE_MAX_SLOTS      4096
#define PIPE_DEFAULT_CAPACITY (1UL << 24)  /* 16 MB default */

/* Return codes */
#define PIPE_OK             0
#define PIPE_ERR_NOMEM     -1
#define PIPE_ERR_NOTFOUND  -2
#define PIPE_ERR_BADARG   -3
#define PIPE_ERR_FULL      -4
#define PIPE_ERR_CLOSED    -5
#define PIPE_ERR_BACKEND   -6
#define PIPE_ERR_DUPLICATE -7

/* Priority levels (for pipe_stream / pipe_write) */
#define PIPE_PRIO_LOW       0
#define PIPE_PRIO_NORMAL    1
#define PIPE_PRIO_HIGH      2
#define PIPE_PRIO_CRITICAL  3
#define PIPE_PRIO_COUNT     4

/* Data type hints (for pipe_write) */
#define PIPE_DTYPE_RAW      0
#define PIPE_DTYPE_F32      1
#define PIPE_DTYPE_F16      2
#define PIPE_DTYPE_I32      3
#define PIPE_DTYPE_I8       4
#define PIPE_DTYPE_Q4       5
#define PIPE_DTYPE_Q8       6

/* ═══════════════════════════════════════════════════════════════
 * SLOT DESCRIPTOR
 * ═══════════════════════════════════════════════════════════════
 * Each written item occupies one slot in the PipeContext.
 * The slot holds metadata; actual data is in the backend.
 * ═══════════════════════════════════════════════════════════════ */

/* PipeSlot — metadata for each stored item */
typedef struct {
    uint64_t    key;            /* RDH flat key (0 = unused) */
    uint32_t    priority;       /* PIPE_PRIO_* */
    uint32_t    dtype;          /* PIPE_DTYPE_* */
    uint32_t    nbytes;         /* data size in bytes */
    uint32_t    backend_offset; /* byte offset in backend storage */
    uint32_t    backend_slot;   /* index in backend storage (0 = none) */
    uint32_t    flags;          /* bit flags (see below) */
    uint32_t    refcount;       /* active map references */
    uint64_t    radial_addr;    /* Radial capture address (vertex×node + twin bit) */
    char        name[64];
} PipeSlot;

/* Slot flags */
#define PIPE_SLOT_DIRTY     0x01    /* written but not flushed */
#define PIPE_SLOT_PINNED    0x02    /* cannot be migrated/evicted */
#define PIPE_SLOT_SHARED    0x04    /* multiple readers allowed */
#define PIPE_SLOT_STREAMING 0x08    /* actively being streamed */

/* ═══════════════════════════════════════════════════════════════
 * SNAPSHOT — Persistent State for Void Space
 * ═══════════════════════════════════════════════════════════════
 * When runtime gets messy: Checkpoint → Destroy → Recreate → Restore
 * Application doesn't know runtime was recreated.
 *
 * Snapshot file layout:
 *   [PipeSnapshotHeader]
 *   [PipeConfig]
 *   [PipeSlot × n_slots]
 *   [backend_size bytes of backend data]
 * ═══════════════════════════════════════════════════════════════ */

#define PIPE_SNAP_MAGIC     0x50534E41  /* "PSNA" */
#define PIPE_SNAP_VERSION   1

typedef struct {
    uint32_t    magic;          /* PIPE_SNAP_MAGIC */
    uint32_t    version;        /* PIPE_SNAP_VERSION */
    uint32_t    n_slots;        /* number of slots saved */
    uint32_t    slot_size;      /* sizeof(PipeSlot) — size of one slot */
    uint64_t    slot_data_size; /* total bytes of slot data (n_slots × slot_size) */
    uint64_t    backend_size;   /* bytes of backend data */
    uint64_t    tick;           /* snapshot tick (monotonic) */
    uint64_t    total_size;     /* total file size (for validation) */
    char        pipe_name[64];  /* pipe name for verification */
} PipeSnapshotHeader;

/* ═══════════════════════════════════════════════════════════════
 * PIPE CONFIGURATION
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;           /* pipe name (for logging/debug) */
    uint64_t    capacity;       /* total backend bytes (0 = default) */
    uint32_t    max_slots;      /* max concurrent slots (0 = default) */
    uint32_t    n_priority;     /* priority lanes (0 = all 4) */
    uint32_t    auto_compact_threshold;  /* auto-compact when usage > this % */
    const char *backend_path;   /* DRamTile file path (NULL = RAM backend) */
    void       *backend_config; /* backend-specific config (NULL = default) */
} PipeConfig;

/* Default config initializer */
#define PIPE_CONFIG_DEFAULT { \
    .name = "pipe",           \
    .capacity = 0,            \
    .max_slots = 0,           \
    .n_priority = 0,          \
    .auto_compact_threshold = 80, \
    .backend_config = NULL     \
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE STATS (for monitoring / compact decisions)
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t capacity_total;    /* total backend capacity */
    uint64_t capacity_used;     /* bytes currently in use */
    uint32_t n_slots;           /* active slots */
    uint32_t n_dirty;           /* unflushed slots */
    uint32_t n_pinned;          /* pinned slots */
    uint64_t n_reads;           /* total read operations */
    uint64_t n_writes;          /* total write operations */
    uint64_t n_compacts;        /* total compact operations */
    uint64_t n_evictions;       /* total evictions (by GearShift) */
} PipeStats;

/* ═══════════════════════════════════════════════════════════════
 * STREAM CALLBACK
 * ═══════════════════════════════════════════════════════════════
 * Called per chunk during pipe_stream().
 * Return 0 to continue, non-zero to stop.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    const void *data;           /* chunk data pointer */
    uint32_t    nbytes;         /* chunk size */
    uint32_t    priority;       /* current chunk priority */
    uint32_t    index;          /* chunk index (0, 1, 2, ...) */
    void       *user;           /* user context */
} PipeStreamChunk;

typedef int (*PipeStreamCallback)(const PipeStreamChunk *chunk, void *user);

/* ═══════════════════════════════════════════════════════════════
 * FORWARD DECLARATIONS
 * ═══════════════════════════════════════════════════════════════ */

typedef struct PipeContext PipeContext;

/* ═══════════════════════════════════════════════════════════════
 * PIPE ABI — System Call Interface
 * ═══════════════════════════════════════════════════════════════
 * Types-only header. Implementation in pipe_context.h (static inline).
 * Applications include pipe_context.h which pulls in this header.
 *
 * API surface:
 *   pipe_open()     — acquire resource
 *   pipe_close()    — release resource
 *   pipe_write()    — store data
 *   pipe_read()     — retrieve data (zero-copy)
 *   pipe_map()      — zero-copy pointer access by key
 *   pipe_stream()   — priority-routed streaming
 *   pipe_flush()    — force pending writes to backend
 *   pipe_compact()  — reduce footprint (NOT collapse)
 *   pipe_find()     — existence check
 *   pipe_remove()   — delete item
 *   pipe_count()    — active slot count
 *   pipe_stats()    — statistics
 *   pipe_name()     — pipe name accessor
 * ═══════════════════════════════════════════════════════════════ */

#ifdef __cplusplus
}
#endif

#endif /* PIPE_ABI_H */

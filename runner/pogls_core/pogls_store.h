/*
 * pogls_store.h — Store API Contract (Pure C)
 *
 * Defines the interface that dramtile_store.h implements.
 * CLI tools include this to work with stores without coupling
 * to the full implementation.
 *
 * No platform dependency — pure C structs and typedefs.
 */

#ifndef POGLS_STORE_H
#define POGLS_STORE_H

#include <stdint.h>
#include <stddef.h>

/* ── Constants ──────────────────────────────────────────── */

#define POGLS_STORE_HASH_SLOTS   512
#define POGLS_STORE_MAX_PATH     260
#define POGLS_STORE_NAME_MAX     256
#define POGLS_STORE_HASH_NAME    48
#define POGLS_STORE_DIR_ENTRY_SZ 48
#define POGLS_STORE_MAX_NDIM     4
#define POGLS_STORE_MAX_FREE     512

/* ── Flags ──────────────────────────────────────────────── */

#define POGLS_DT_KV_FLAG    0x80000000u
#define POGLS_DT_BOND_FLAG  0x40000000u
#define POGLS_DT_DELTA_FLAG 0x20000000u
#define POGLS_DT_FLAGS_MASK (POGLS_DT_KV_FLAG | POGLS_DT_BOND_FLAG | POGLS_DT_DELTA_FLAG)

/* ── Data Types ─────────────────────────────────────────── */

typedef enum {
    POGLS_DT_F32 = 0,
    POGLS_DT_F16 = 1,
    POGLS_DT_I32 = 2,
    POGLS_DT_I8  = 3,
    POGLS_DT_Q40 = 4,
    POGLS_DT_Q80 = 5,
} PoglsDtDataType;

/* ── Tensor View (read-only perspective) ────────────────── */

typedef struct {
    uint8_t  *data;
    size_t    offset;
    size_t    nbytes;
    uint32_t  dram_addr;
    uint32_t  dtype;
    int       ndim;
    uint32_t  shape[6];
    char      name[POGLS_STORE_NAME_MAX];
} PoglsStoreView;

/* ── Callback for iteration ─────────────────────────────── */

typedef int (*PoglsStoreCallback)(PoglsStoreView *view, void *user);

/* ── Store Stats ────────────────────────────────────────── */

typedef struct {
    size_t capacity;
    size_t used;
    size_t kv_capacity;
    size_t kv_used;
    size_t cold_capacity;
    size_t cold_used;
    uint32_t n_stored;
    uint32_t free_count;
} PoglsStoreStats;

#endif /* POGLS_STORE_H */

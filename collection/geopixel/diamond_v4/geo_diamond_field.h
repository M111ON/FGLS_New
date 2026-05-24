/*
 * geo_diamond_field.h — Diamond Field v3 (Shell + Slot + Tring)
 *
 * Core concept:
 *   block  = geometry slot (bit in shell)
 *   data   = Tring timeline (external, tick-indexed)
 *   delete = clear flag (data survives until GC)
 *   reshape= remap slots to new shell level
 *
 * Shell scale: level n → size = 2n+1 → slots = (2n+1)^3
 *   n=0: 1    n=1: 27   n=2: 125  n=3: 343
 *   n=4: 729  n=5: 1331 n=6: 2197 n=7: 3375  n=8: 4913
 *
 * Global slot index (no collision across levels):
 *   global_idx = (n << 13) | local_idx
 *   n: 4 bits (0-8), local_idx: 13 bits (0-4912)
 *
 * fit() = popcnt(fold_fibo_intersect(chunk_as_block)) < THRESH[n]
 *   → O(1), uses existing pipeline, no spatial comparison needed
 *
 * depends: pogls_fold.h (DiamondBlock, fold_fibo_intersect, fold_xor_audit)
 */

#ifndef GEO_DIAMOND_FIELD_H
#define GEO_DIAMOND_FIELD_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pogls_fold.h"

/* ── constants ────────────────────────────────────────────────────── */
#define SHELL_MAX_LEVEL   8u
#define SHELL_MAX_SLOTS   4913u          /* 17^3, level 8 */
#define SHELL_IDX_BITS    13u
#define SHELL_IDX_MASK    ((1u << SHELL_IDX_BITS) - 1u)  /* 0x1FFF */
#define TRING_MAX_TICKS   (1u << 20)     /* 1M ticks, caller can extend */
#define SLOT_NULL         0xFFFFFFFFu

/* fit threshold per level: higher n = bigger shell = accepts denser data
 * level 0-2: tight (popcnt < 16), level 3-5: medium, level 6-8: loose
 * tuned so ~50% of random chunks fit at n=3 (practical sweet spot) */
static const uint8_t SHELL_FIT_THRESH[9] = {
    8, 12, 16, 24, 32, 40, 48, 56, 64
};

/* ── Shell ─────────────────────────────────────────────────────────── */
/* flags[] = bitfield, 1 bit per slot (1=visible/occupied) */
typedef struct {
    uint8_t  level;          /* n = 0..8                          */
    uint8_t  size;           /* = 2n+1                            */
    uint16_t slot_count;     /* = size^3 (max 4913)               */
    uint8_t  _pad[4];
    uint64_t flags[77];      /* ceil(4913/64) = 77 words = 616B   */
} Shell;

static inline uint16_t shell_size(uint8_t n)   { return (uint16_t)(2u*n + 1u); }
static inline uint16_t shell_slots(uint8_t n)  { uint16_t s = shell_size(n); return (uint16_t)(s*s*s); }

static inline void shell_init(Shell *sh, uint8_t n) {
    memset(sh, 0, sizeof(*sh));
    sh->level      = n;
    sh->size       = (uint8_t)shell_size(n);
    sh->slot_count = shell_slots(n);
}

/* flag ops */
static inline void  shell_set(Shell *sh, uint16_t idx)
    { sh->flags[idx >> 6] |=  (1ULL << (idx & 63u)); }
static inline void  shell_clr(Shell *sh, uint16_t idx)
    { sh->flags[idx >> 6] &= ~(1ULL << (idx & 63u)); }
static inline int   shell_get(const Shell *sh, uint16_t idx)
    { return (int)((sh->flags[idx >> 6] >> (idx & 63u)) & 1u); }
static inline int   shell_any(const Shell *sh)
    { for (int i=0;i<77;i++) if (sh->flags[i]) return 1; return 0; }

/* ── Slot ──────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t x, y, z;   /* coordinates within (2n+1)^3 */
    uint16_t idx;        /* local flattened: x + y*size + z*size*size */
} Slot;

static inline Slot slot_from_idx(uint16_t idx, uint8_t size) {
    Slot s;
    s.idx = idx;
    s.z   = (uint16_t)(idx / ((uint16_t)size * size));
    s.y   = (uint16_t)((idx / size) % size);
    s.x   = (uint16_t)(idx % size);
    return s;
}

static inline uint16_t slot_to_idx(uint16_t x, uint16_t y, uint16_t z, uint8_t size) {
    return (uint16_t)(x + y*(uint16_t)size + z*(uint16_t)size*(uint16_t)size);
}

/* global index — encodes both level and local idx, no collision */
static inline uint32_t slot_global(uint8_t n, uint16_t local_idx) {
    return ((uint32_t)n << SHELL_IDX_BITS) | (local_idx & SHELL_IDX_MASK);
}
static inline uint8_t  global_level(uint32_t gidx) { return (uint8_t)(gidx >> SHELL_IDX_BITS); }
static inline uint16_t global_local(uint32_t gidx) { return (uint16_t)(gidx & SHELL_IDX_MASK); }

/* ── fit() — O(1) collapse proxy ──────────────────────────────────── */
/*
 * Treat the 64B chunk as a DiamondBlock by pointer cast (zero-copy).
 * fold_fibo_intersect = AND of 4 quad_mirror words → popcount = "stability".
 * Low popcount = data collapses well to geometry at this level.
 * High popcount = data too dense/unique → try larger shell.
 *
 * Note: chunk must be 64B aligned for correct DiamondBlock cast.
 * fold_xor_audit may fail on raw data — we skip it intentionally here
 * (fit is a heuristic, not an integrity check).
 */
static inline int shell_fit(const uint8_t chunk[64], uint8_t n) {
    const DiamondBlock *b = (const DiamondBlock *)chunk;
    uint64_t isect = fold_fibo_intersect(b);
    int pc = __builtin_popcountll(isect);
    return pc <= (int)SHELL_FIT_THRESH[n];
}

/* find minimum shell level that fits chunk; returns 9 if none fit */
static inline uint8_t shell_fit_level(const uint8_t chunk[64]) {
    for (uint8_t n = 0; n <= SHELL_MAX_LEVEL; n++)
        if (shell_fit(chunk, n)) return n;
    return 9u;  /* caller: use n=8 (max) as fallback */
}

/* ── map chunk → slot index (deterministic, reversible) ─────────── */
/*
 * map: fold the 64B chunk into a local slot index within shell n.
 * method: FNV64 of chunk mod slot_count → uniform distribution.
 * inverse_map: given slot + tick, recover original mapping metadata.
 * (full chunk recovery is via tring[tick], map just picks placement)
 */
#define FNV64_OFFSET 14695981039346656037ULL
#define FNV64_PRIME  1099511628211ULL

static inline uint64_t _fnv64(const uint8_t *data, uint32_t len) {
    uint64_t h = FNV64_OFFSET;
    for (uint32_t i = 0; i < len; i++)
        h = (h ^ data[i]) * FNV64_PRIME;
    return h;
}

static inline uint16_t chunk_to_slot_idx(const uint8_t chunk[64], uint8_t n) {
    uint16_t cap = shell_slots(n);
    if (cap == 0) return 0;
    return (uint16_t)(_fnv64(chunk, 64) % cap);
}

/* ── Tring — data timeline ────────────────────────────────────────── */
typedef struct {
    uint64_t tick;
    uint8_t  data[64];   /* raw 64B chunk */
} TringNode;

typedef struct {
    TringNode **nodes;       /* [MAX_TICKS], sparse (NULL = free) */
    uint32_t   capacity;     /* allocated slots                   */
    uint32_t   next_tick;    /* monotonic counter                 */
    uint32_t   live_count;   /* non-NULL entries (for GC trigger) */
} Tring;

static inline int tring_init(Tring *t, uint32_t cap) {
    t->nodes      = (TringNode **)calloc(cap, sizeof(TringNode *));
    if (!t->nodes) return -1;
    t->capacity   = cap;
    t->next_tick  = 0;
    t->live_count = 0;
    return 0;
}

static inline void tring_free(Tring *t) {
    if (!t->nodes) return;
    for (uint32_t i = 0; i < t->capacity; i++)
        if (t->nodes[i]) { free(t->nodes[i]); t->nodes[i] = NULL; }
    free(t->nodes);
    t->nodes = NULL;
}

/* push chunk → returns tick, UINT32_MAX on error */
static inline uint32_t tring_push(Tring *t, const uint8_t chunk[64]) {
    if (t->next_tick >= t->capacity) return UINT32_MAX;
    TringNode *node = (TringNode *)malloc(sizeof(TringNode));
    if (!node) return UINT32_MAX;
    node->tick = t->next_tick;
    memcpy(node->data, chunk, 64);
    t->nodes[t->next_tick] = node;
    t->live_count++;
    return t->next_tick++;
}

/* read chunk at tick (NULL if freed/invalid) */
static inline const uint8_t *tring_read(const Tring *t, uint32_t tick) {
    if (tick >= t->capacity || !t->nodes[tick]) return NULL;
    return t->nodes[tick]->data;
}

/* free a tick slot (GC helper) */
static inline void tring_release(Tring *t, uint32_t tick) {
    if (tick >= t->capacity || !t->nodes[tick]) return;
    free(t->nodes[tick]);
    t->nodes[tick] = NULL;
    if (t->live_count) t->live_count--;
}

/* ── Index — slot → tick mapping ──────────────────────────────────── */
/*
 * index keyed by global_idx (17 bits max: 4 level + 13 local).
 * max global_idx = (8<<13)|4912 = 69552 → flat array fits in 272KB.
 * SLOT_NULL = unoccupied.
 */
#define INDEX_SIZE ((9u << SHELL_IDX_BITS))   /* 9*8192 = 73728 entries */

typedef struct {
    uint32_t tick[INDEX_SIZE];  /* global_idx → tick, SLOT_NULL if empty */
} SlotIndex;

static inline void sidx_init(SlotIndex *si) {
    memset(si->tick, 0xFF, sizeof(si->tick));  /* 0xFFFFFFFF = SLOT_NULL */
}
static inline void    sidx_set(SlotIndex *si, uint32_t gidx, uint32_t tick)
    { if (gidx < INDEX_SIZE) si->tick[gidx] = tick; }
static inline uint32_t sidx_get(const SlotIndex *si, uint32_t gidx)
    { return (gidx < INDEX_SIZE) ? si->tick[gidx] : SLOT_NULL; }
static inline void    sidx_clear(SlotIndex *si, uint32_t gidx)
    { if (gidx < INDEX_SIZE) si->tick[gidx] = SLOT_NULL; }

/* ── DiamondField — top-level container ────────────────────────────── */
typedef struct {
    Shell     shell[9];   /* one shell per level, all pre-allocated */
    Tring     tring;
    SlotIndex sidx;
} DiamondField;

static inline int dfield_init(DiamondField *df, uint32_t tring_cap) {
    for (uint8_t n = 0; n <= SHELL_MAX_LEVEL; n++)
        shell_init(&df->shell[n], n);
    sidx_init(&df->sidx);
    return tring_init(&df->tring, tring_cap ? tring_cap : TRING_MAX_TICKS);
}

static inline void dfield_free(DiamondField *df) {
    tring_free(&df->tring);
}

/* ── ENCODE ────────────────────────────────────────────────────────── */
/*
 * Returns global_idx of assigned slot, SLOT_NULL on error.
 * Inline mode: n<=2 and tring is not used (caller reads from inline_buf).
 * For n<=2 chunks: stored inline inside SlotIndex upper bits (not implemented
 * here — caller handles based on returned level via dfield_encode_level()).
 */
static inline uint32_t dfield_encode(DiamondField *df,
                                      const uint8_t  chunk[64],
                                      uint8_t       *out_level)
{
    /* 1. find fit level */
    uint8_t n = shell_fit_level(chunk);
    if (n > SHELL_MAX_LEVEL) n = SHELL_MAX_LEVEL;  /* fallback: max shell */
    if (out_level) *out_level = n;

    /* 2. map chunk → slot */
    uint16_t local_idx = chunk_to_slot_idx(chunk, n);
    uint32_t gidx      = slot_global(n, local_idx);

    /* 3. probe + level upgrade: find slot that is unflagged AND sidx-empty.
     * If current shell n is full, escalate to n+1 until we find space.
     * This preserves correctness: no tick is ever overwritten. */
    while (n <= SHELL_MAX_LEVEL) {
        uint16_t cap = df->shell[n].slot_count;
        uint16_t probe = chunk_to_slot_idx(chunk, n);
        int found = 0;
        for (uint16_t i = 0; i < cap; i++) {
            uint32_t g_probe = slot_global(n, probe);
            if (!shell_get(&df->shell[n], probe) &&
                sidx_get(&df->sidx, g_probe) == SLOT_NULL) {
                local_idx = probe;
                gidx      = g_probe;
                found = 1;
                break;
            }
            probe = (uint16_t)((probe + 1u) % cap);
        }
        if (found) { if (out_level) *out_level = n; break; }
        n++;  /* shell full — try next level */
    }
    if (n > SHELL_MAX_LEVEL) return SLOT_NULL;  /* all shells full */

    /* 4. push to tring */
    uint32_t tick = tring_push(&df->tring, chunk);
    if (tick == UINT32_MAX) return SLOT_NULL;

    /* 5. mark shell + index */
    shell_set(&df->shell[n], local_idx);
    sidx_set(&df->sidx, gidx, tick);

    return gidx;
}

/* ── DECODE ─────────────────────────────────────────────────────────── */
/* Returns pointer to 64B chunk in tring (valid until GC), NULL on error. */
static inline const uint8_t *dfield_decode(const DiamondField *df,
                                            uint32_t gidx)
{
    uint8_t  n   = global_level(gidx);
    uint16_t idx = global_local(gidx);
    if (n > SHELL_MAX_LEVEL) return NULL;
    if (!shell_get(&df->shell[n], idx)) return NULL;  /* not visible */
    uint32_t tick = sidx_get(&df->sidx, gidx);
    if (tick == SLOT_NULL) return NULL;
    return tring_read(&df->tring, tick);
}

/* ── DELETE O(1) ────────────────────────────────────────────────────── */
/* Data survives in tring until GC sweep. */
static inline void dfield_delete(DiamondField *df, uint32_t gidx) {
    uint8_t  n   = global_level(gidx);
    uint16_t idx = global_local(gidx);
    if (n > SHELL_MAX_LEVEL) return;
    shell_clr(&df->shell[n], idx);
    /* keep sidx entry for GC reference counting */
}

/* ── GC ─────────────────────────────────────────────────────────────── */
/*
 * Sweep: for each tick in tring, check if any shell slot still references it.
 * O(live_ticks × INDEX_SIZE) — call infrequently (e.g. every 1K deletes).
 * Returns number of ticks freed.
 */
static inline uint32_t dfield_gc(DiamondField *df) {
    uint32_t freed = 0;
    for (uint32_t tick = 0; tick < df->tring.next_tick; tick++) {
        if (!df->tring.nodes[tick]) continue;  /* already freed */
        /* check: is any visible slot pointing to this tick? */
        int referenced = 0;
        for (uint32_t gidx = 0; gidx < INDEX_SIZE && !referenced; gidx++) {
            if (sidx_get(&df->sidx, gidx) != tick) continue;
            uint8_t  n   = global_level(gidx);
            uint16_t idx = global_local(gidx);
            if (n <= SHELL_MAX_LEVEL && shell_get(&df->shell[n], idx))
                referenced = 1;
        }
        if (!referenced) {
            tring_release(&df->tring, tick);
            freed++;
        }
    }
    return freed;
}

/* ── RESHAPE ─────────────────────────────────────────────────────────── */
/*
 * Remap all visible slots from current shell level to new level n_new.
 * Old shell flags are cleared; new slots assigned deterministically.
 * Returns number of slots remapped.
 */
static inline uint32_t dfield_reshape(DiamondField *df, uint8_t n_old, uint8_t n_new) {
    if (n_old > SHELL_MAX_LEVEL || n_new > SHELL_MAX_LEVEL) return 0;
    uint32_t remapped = 0;
    uint16_t cap = df->shell[n_old].slot_count;

    for (uint16_t idx = 0; idx < cap; idx++) {
        if (!shell_get(&df->shell[n_old], idx)) continue;
        uint32_t gidx_old = slot_global(n_old, idx);
        uint32_t tick     = sidx_get(&df->sidx, gidx_old);
        if (tick == SLOT_NULL) continue;

        /* remap: re-derive slot from chunk data */
        const uint8_t *chunk = tring_read(&df->tring, tick);
        if (!chunk) continue;

        uint16_t new_local = chunk_to_slot_idx(chunk, n_new);
        /* probe for free slot in new shell */
        uint16_t new_cap = df->shell[n_new].slot_count;
        for (uint16_t p = 0; p < new_cap; p++) {
            if (!shell_get(&df->shell[n_new], new_local)) break;
            new_local = (uint16_t)((new_local + 1u) % new_cap);
        }
        uint32_t gidx_new = slot_global(n_new, new_local);

        /* clear old, set new */
        shell_clr(&df->shell[n_old], idx);
        sidx_clear(&df->sidx, gidx_old);
        shell_set(&df->shell[n_new], new_local);
        sidx_set(&df->sidx, gidx_new, tick);
        remapped++;
    }
    return remapped;
}

#endif /* GEO_DIAMOND_FIELD_H */

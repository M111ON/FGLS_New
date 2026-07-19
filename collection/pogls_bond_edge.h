/*
 * pogls_bond_edge.h
 * ─────────────────────────────────────────────────────────────
 * Variable-arity bond edge layer — separate from PoglsPlug.
 *
 * PoglsPlug (in pogls_bond.h) stays as-is: fixed 4-face (N/S/E/W)
 * geometric connector, unrelated to this file.
 *
 * This file adds an independent, dynamically-sized list of bond
 * edges. Each edge is the 3-field record discussed with Po:
 *
 *     origin  — geo_key of the chunk the edge starts from
 *     target  — geo_key of the chunk the edge points to
 *     weight  — arbitrary payload/strength value for the edge
 *
 * Cardinality (n_edges) is per-chunk and data-dependent — it is
 * NOT fixed at 4 like PoglsPlug. Geometrically this corresponds
 * to the variable number of red connector-line endpoints seen
 * around a chunk's boundary (joints), rather than a fixed
 * N/S/E/W layout.
 *
 * Fan-out is always 1-to-1 per edge (linked-list style): one
 * edge = one origin pointing at exactly one target. A chunk can
 * still have many edges, it just means many separate 1-to-1
 * links, not branching from a single edge.
 *
 * Validity of an edge reuses the SAME bond-key machinery as
 * pogls_bond.h (pogls_fibo_addr + POGLS_BOND_VERIFY_MASK), so a
 * "joint" (edge landing correctly on its target) is verifiable
 * with the same coordinate-tamper-evidence property intrinsic
 * bonds already have: move either endpoint → its geo_key changes
 * → the edge's derived key changes → edge invalidated automatically.
 * ─────────────────────────────────────────────────────────────
 */

#ifndef POGLS_BOND_EDGE_H
#define POGLS_BOND_EDGE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_bond.h"   /* reuses pogls_fibo_addr, salts, verify mask/target */

/* ── EDGE RECORD (origin, target, weight) ─────────────────────
 * 8B origin + 8B target + 4B weight = 20B per edge (packed)
 * weight is intentionally a plain uint32_t: caller decides its
 * meaning (strength, priority, byte offset, refcount, ...).
 * ─────────────────────────────────────────────────────────────*/
typedef struct __attribute__((packed)) {
    uint64_t origin;   /* geo_key of source chunk         */
    uint64_t target;   /* geo_key of destination chunk    */
    uint32_t weight;   /* caller-defined edge payload     */
} PoglsBondEdge;        /* 20B */

/* ── VARIABLE-ARITY EDGE LIST ──────────────────────────────────
 * Owned, heap-backed, per-chunk. count is the live number of
 * edges; capacity tracks the allocation for amortized growth.
 * This is intentionally separate storage from PoglsSlot/PoglsPlug
 * — a chunk that wants bond edges attaches one of these
 * alongside its existing PoglsSlot, it does not replace plugs[4].
 * ─────────────────────────────────────────────────────────────*/
typedef struct {
    PoglsBondEdge *edges;
    uint32_t       count;
    uint32_t       capacity;
} PoglsBondEdgeList;

static inline void pogls_edge_list_init(PoglsBondEdgeList *list) {
    list->edges    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

static inline void pogls_edge_list_free(PoglsBondEdgeList *list) {
    free(list->edges);
    list->edges    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

/*
 * pogls_edge_list_add(list, origin, target, weight) → index or -1
 * Appends a new 1-to-1 edge. Grows capacity ×2 (amortized O(1)).
 * Returns the index of the new edge, or -1 on allocation failure.
 */
static inline int32_t pogls_edge_list_add(PoglsBondEdgeList *list,
                                           uint64_t origin,
                                           uint64_t target,
                                           uint32_t weight) {
    if (list->count == list->capacity) {
        uint32_t new_cap = list->capacity ? list->capacity * 2u : 4u;
        PoglsBondEdge *grown = (PoglsBondEdge *)realloc(
            list->edges, new_cap * sizeof(PoglsBondEdge));
        if (!grown) return -1;
        list->edges    = grown;
        list->capacity = new_cap;
    }
    list->edges[list->count].origin = origin;
    list->edges[list->count].target = target;
    list->edges[list->count].weight = weight;
    return (int32_t)(list->count++);
}

/*
 * pogls_edge_list_remove(list, index) → 1 on success, 0 if out of range
 * Swap-with-last removal (O(1), order not preserved).
 */
static inline int pogls_edge_list_remove(PoglsBondEdgeList *list, uint32_t index) {
    if (index >= list->count) return 0;
    list->edges[index] = list->edges[list->count - 1];
    list->count--;
    return 1;
}

/* ── EDGE VALIDITY (reuses pogls_bond.h's bond-key machinery) ──
 * An edge is a "joint" (geometrically valid landing point) when
 * its derived key passes the same avalanche+mask check intrinsic
 * bonds use. This keeps edge validation and piece-bond validation
 * on one shared tamper-evidence mechanism instead of two.
 *
 * derived key = fibo_addr(fibo_addr(origin ^ SALT_L) ^
 *                          fibo_addr(target ^ SALT_R))
 * i.e. structurally mirrors pogls_bond_key()/pogls_bond_verify(),
 * just fed (origin, target) directly instead of (bond_L, bond_R)
 * from a single piece.
 */
static inline uint64_t pogls_edge_key(const PoglsBondEdge *e) {
    uint64_t oL = pogls_fibo_addr(e->origin ^ POGLS_BOND_SALT_L);
    uint64_t tR = pogls_fibo_addr(e->target ^ POGLS_BOND_SALT_R);
    return pogls_fibo_addr(oL ^ tR);
}

/*
 * pogls_edge_verify(e) → PoglsBond
 * Same nonce + double-avalanche pass as pogls_bond_verify(), just
 * driven by the edge's own key instead of two pieces' bond_keys.
 * valid==1 means this edge is a legitimate joint at its current
 * (origin, target) coordinates; move either chunk → key changes
 * → valid becomes 0 automatically, same tamper-evidence property
 * as the rest of the bond layer.
 */
static inline PoglsBond pogls_edge_verify(const PoglsBondEdge *e) {
    PoglsBond bond;
    uint64_t  ek = pogls_edge_key(e);

    uint64_t nonce     = pogls_config_get_nonce();
    uint64_t nonce_mix = pogls_fibo_addr(ek ^ nonce);
    uint64_t combined  = pogls_fibo_addr(nonce_mix ^ ek);

    bond.bond_key = ek;
    bond.valid    = ((combined & POGLS_BOND_VERIFY_MASK) == POGLS_BOND_VERIFY_TARGET)
                     ? 1 : 0;
    return bond;
}

/*
 * pogls_edge_list_verify_all(list, out_valid_count) → 0 if all valid,
 * else count of invalid edges. Walks the whole list; useful as a
 * chunk-level "are all my joints still intact" check, e.g. after
 * a batch of chunks moved/rehashed.
 */
static inline uint32_t pogls_edge_list_verify_all(const PoglsBondEdgeList *list,
                                                    uint32_t *out_valid_count) {
    uint32_t valid = 0, invalid = 0;
    for (uint32_t i = 0; i < list->count; i++) {
        PoglsBond b = pogls_edge_verify(&list->edges[i]);
        if (b.valid) valid++; else invalid++;
    }
    if (out_valid_count) *out_valid_count = valid;
    return invalid;
}

#endif /* POGLS_BOND_EDGE_H */

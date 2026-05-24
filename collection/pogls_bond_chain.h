/*
 * pogls_bond_chain.h
 * ─────────────────────────────────────────────────────────────
 * Bond Chain — relationship-based reconstruction layer
 *
 * Architecture position:
 *   geo_field_core_3.h → [THIS FILE] → pogls_bond.h
 *
 * Concept:
 *   Each 64B chunk (GeoField chunk_idx) gets a BondNode.
 *   BondNode carries:
 *     - intrinsic identity  (geo_key, bond_L, bond_R from pogls_bond.h)
 *     - doubly-linked chain (prev_key ↔ next_key)
 *     - route hint          (where this chunk currently lives)
 *     - origin_key          (bond_key at birth — survives any reroute)
 *
 *   Reconstruction rule:
 *     Given ANY node, walk prev/next by matching bond_key.
 *     No sequence numbers. No central index. Order emerges from bonds.
 *
 * Hash table overlay (optional):
 *   bond_chain_build_ht() — O(1) find by bond_key
 *   bond_chain_find_ht()  — O(1) lookup, falls back to O(n) scan if no HT
 *   bond_chain_free_ht()  — free hash table
 *
 * Usage:
 *   BondChain chain;
 *   bond_chain_init(&chain, n_chunks);
 *   bond_chain_build(&chain, face_max);           // encode side
 *   bond_chain_build_ht(&chain);                  // optional O(1) overlay
 *   bond_chain_reconstruct(&chain, order);        // decode side
 *   bond_chain_free(&chain);
 *
 * Rules (matches geofield SKILL.md):
 *   - No malloc in hot path (pre-alloc in init)
 *   - No float, O(1) per node
 *   - Header-only, static inline
 *   - Packed structs, _Static_assert size checks
 * ─────────────────────────────────────────────────────────────
 */

#ifndef POGLS_BOND_CHAIN_H
#define POGLS_BOND_CHAIN_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pogls_bond.h"

/* ── CONSTANTS ─────────────────────────────────────────────── */

/* Route location hints — where a chunk currently lives */
#define BOND_ROUTE_UNKNOWN   0x00
#define BOND_ROUTE_RAM       0x01
#define BOND_ROUTE_DISK      0x02
#define BOND_ROUTE_VRAM      0x03   /* GPU */
#define BOND_ROUTE_NET       0x04   /* remote node */
#define BOND_ROUTE_CACHE     0x05   /* network cache */

/* Sentinel: no neighbor */
#define BOND_KEY_NONE        UINT64_C(0)

/* ── BOND NODE (56B) ────────────────────────────────────────── */
/*
 * One node per chunk_idx.
 *
 * identity (25B):  PoglsPiece — geo_key + shape + bond_L + bond_R
 * chain    (16B):  prev_key + next_key (bond_key of neighbors)
 * origin   ( 8B):  bond_key at birth — stable across reroute
 * chunk_idx( 4B):  back-reference to GeoField chunk index
 * route    ( 1B):  current location hint
 * flags    ( 1B):  bit0=head, bit1=tail, bit2=rerouted
 * _pad     ( 1B):  alignment
 */
typedef struct __attribute__((packed)) {
    PoglsPiece piece;       /* 25B — intrinsic identity       */
    uint64_t   prev_key;    /*  8B — bond_key of prev chunk   */
    uint64_t   next_key;    /*  8B — bond_key of next chunk   */
    uint64_t   origin_key;  /*  8B — stable birth bond_key    */
    uint32_t   chunk_idx;   /*  4B — GeoField chunk index     */
    uint8_t    route;       /*  1B — BOND_ROUTE_*             */
    uint8_t    flags;       /*  1B — head/tail/rerouted bits  */
    uint8_t    _pad;        /*  1B                            */
} BondNode;                 /* total = 56B                    */

_Static_assert(sizeof(BondNode) == 56, "BondNode must be 56B");
_Static_assert(sizeof(PoglsPiece) == 25, "PoglsPiece must be 25B");

/* Flag bits */
#define BNODE_FLAG_HEAD      0x01
#define BNODE_FLAG_TAIL      0x02
#define BNODE_FLAG_REROUTED  0x04

/* ── HASH TABLE OVERLAY (optional, O(1) find) ──────────────── */
/*
 * Open-addressing, power-of-2 capacity.
 * capacity = next_pow2(n_chunks * 4/3)
 * key=0  → empty slot (bond_key is never 0 by construction)
 */
typedef struct {
    uint64_t *keys;       /* bond_key  → slot          */
    uint32_t *values;     /* chunk_idx → slot          */
    uint32_t  capacity;   /* power of 2                */
    uint32_t  mask;       /* capacity - 1              */
} BondChainHT;

/* ── BOND CHAIN ─────────────────────────────────────────────── */
typedef struct {
    BondNode    *nodes;       /* pre-allocated array [n_chunks] */
    BondChainHT  ht;          /* optional hash table overlay    */
    uint32_t     n_chunks;
    uint32_t     n_valid;     /* nodes with valid bond          */
    uint64_t     session_nonce;
} BondChain;

/* ── INTERNAL: seed from chunk_idx + tile_id + dim ─────────── */
static inline uint64_t _bond_chunk_seed(uint64_t chunk_idx,
                                         uint32_t tile_id,
                                         uint8_t  dim) {
    /* Mix chunk position with geometric coordinate */
    uint64_t s = chunk_idx;
    s ^= (uint64_t)tile_id << 32;
    s ^= (uint64_t)dim     << 24;
    s ^= UINT64_C(0x9E3779B185EBCA87);   /* golden constant */
    return pogls_fibo_addr(s);
}

/* ── INTERNAL: next power of 2 ─────────────────────────────── */
static inline uint32_t _bond_next_pow2(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

/* Forward declarations */
static inline void bond_chain_free_ht(BondChain *bc);

/* ── INIT / FREE ────────────────────────────────────────────── */
static inline int bond_chain_init(BondChain *bc, uint32_t n_chunks) {
    if (!bc || n_chunks == 0) return -1;
    bc->nodes = (BondNode *)calloc(n_chunks, sizeof(BondNode));
    if (!bc->nodes) return -2;
    bc->n_chunks      = n_chunks;
    bc->n_valid       = 0;
    bc->session_nonce = pogls_config_get_nonce();
    bc->ht.keys       = NULL;
    bc->ht.values     = NULL;
    bc->ht.capacity   = 0;
    bc->ht.mask       = 0;
    return 0;
}

static inline void bond_chain_free(BondChain *bc) {
    if (!bc) return;
    bond_chain_free_ht(bc);
    free(bc->nodes);
    bc->nodes   = NULL;
    bc->n_chunks = 0;
}

/* ── BUILD: assign bond to each chunk ──────────────────────── */
/*
 * bond_chain_build_chunk()
 *
 * Called once per chunk during encode (mirrors geo_field_encode_chunk).
 * Derives seed from {chunk_idx, tile_id, dim} → makes piece.
 * Wires prev↔next links.
 *
 * fold_axis: use (dim & 0x7) → maps naturally to 1..7 axis range
 */
static inline void bond_chain_build_chunk(BondChain  *bc,
                                           uint64_t    chunk_idx,
                                           uint32_t    tile_id,
                                           uint8_t     dim) {
    if (!bc || chunk_idx >= bc->n_chunks) return;

    BondNode *node = &bc->nodes[chunk_idx];
    uint8_t   axis = (uint8_t)((dim & 0x6) + 1);   /* 1..7, never 0 */

    uint64_t seed    = _bond_chunk_seed(chunk_idx, tile_id, dim);
    node->piece      = pogls_make_piece(seed, axis);
    node->chunk_idx  = (uint32_t)chunk_idx;
    node->route      = BOND_ROUTE_UNKNOWN;
    node->flags      = 0;
    node->origin_key = pogls_bond_key(&node->piece);

    /* Wire doubly-linked chain */
    node->prev_key = (chunk_idx == 0)
                     ? BOND_KEY_NONE
                     : pogls_bond_key(&bc->nodes[chunk_idx - 1].piece);

    node->next_key = BOND_KEY_NONE;   /* filled by next chunk */

    /* Back-fill previous node's next_key */
    if (chunk_idx > 0) {
        bc->nodes[chunk_idx - 1].next_key = node->origin_key;
    }

    /* Head / tail flags */
    if (chunk_idx == 0)
        node->flags |= BNODE_FLAG_HEAD;
    if (chunk_idx == bc->n_chunks - 1)
        node->flags |= BNODE_FLAG_TAIL;

    bc->n_valid++;
}

/*
 * bond_chain_build()
 *
 * Bulk build for linear chunk_idx 0..n-1.
 * tile_id = chunk_idx % face_max  (mirrors geo_field_core mapping)
 * dim     = chunk_idx / face_max  (capped to 0x7F)
 * face_max: pass gp_face_count(gf->gp_level)
 */
static inline void bond_chain_build(BondChain *bc,
                                     uint32_t   face_max) {
    if (!bc || face_max == 0) return;
    for (uint32_t i = 0; i < bc->n_chunks; i++) {
        uint32_t tile_id = i % face_max;
        uint8_t  dim     = (uint8_t)((i / face_max) & 0x7F);
        bond_chain_build_chunk(bc, i, tile_id, dim);
    }
}

/* ── VERIFY: check bond between two adjacent nodes ─────────── */
static inline uint8_t bond_chain_verify_pair(const BondChain *bc,
                                              uint32_t idx_a,
                                              uint32_t idx_b) {
    if (!bc || idx_a >= bc->n_chunks || idx_b >= bc->n_chunks) return 0;
    PoglsBond bond = pogls_bond_verify(&bc->nodes[idx_a].piece,
                                        &bc->nodes[idx_b].piece);
    return bond.valid;
}

/* ── REROUTE: reshape chunk, origin_key survives ───────────── */
/*
 * After reroute:
 *   piece.shape   → new shape (fault-driven)
 *   piece.geo_key → remixed
 *   piece.bond_L  → UNCHANGED (origin identity preserved)
 *   piece.bond_R  → UNCHANGED (origin identity preserved)
 *   origin_key    → UNCHANGED (bond_L^bond_R at birth)
 *   prev/next_key → UNCHANGED (chain topology preserved)
 */
static inline void bond_chain_reroute(BondChain *bc,
                                       uint32_t   chunk_idx,
                                       uint8_t    fault) {
    if (!bc || chunk_idx >= bc->n_chunks) return;
    BondNode *node = &bc->nodes[chunk_idx];

    /* Wrap in a minimal PoglsSlot for the reroute call */
    PoglsSlot slot;
    memset(&slot, 0, sizeof(slot));
    slot.piece = node->piece;

    pogls_reroute(&slot, (PoglsFault)fault);

    /* Copy only geo_key + shape back — bond_L/bond_R untouched */
    node->piece.geo_key = slot.piece.geo_key;
    node->piece.shape   = slot.piece.shape;
    node->flags        |= BNODE_FLAG_REROUTED;
    /* origin_key, prev_key, next_key: NOT modified */
}

/* ── ROUTE HINT: tag chunk location ────────────────────────── */
static inline void bond_chain_set_route(BondChain *bc,
                                         uint32_t   chunk_idx,
                                         uint8_t    route) {
    if (!bc || chunk_idx >= bc->n_chunks) return;
    bc->nodes[chunk_idx].route = route;
}

/* ── HASH TABLE: build / find / free ───────────────────────── */

/*
 * _bond_ht_slot()
 * Open-addressing with linear probing.
 * Uses (key ^ (key>>32)) mod capacity as starting index.
 */
static inline uint32_t _bond_ht_slot(const BondChainHT *ht, uint64_t key) {
    uint32_t idx = (uint32_t)((key ^ (key >> 32)) & ht->mask);
    while (ht->keys[idx] != 0 && ht->keys[idx] != key) {
        idx = (idx + 1) & ht->mask;
    }
    return idx;
}

/*
 * bond_chain_build_ht()
 *
 * Build open-addressing hash table from existing chain.
 * capacity = next_pow2(n_chunks * 4/3)  — minimum 8.
 * Each entry maps origin_key → chunk_idx for O(1) find.
 *
 * Call after bond_chain_build() / bond_chain_reroute().
 * Safe to call multiple times — frees previous HT if any.
 */
static inline int bond_chain_build_ht(BondChain *bc) {
    if (!bc || bc->n_chunks == 0) return -1;

    /* Free existing HT if any */
    bond_chain_free_ht(bc);

    uint32_t cap = _bond_next_pow2((uint32_t)(bc->n_chunks * 4 / 3 + 1));
    if (cap < 8) cap = 8;

    bc->ht.keys   = (uint64_t *)calloc(cap, sizeof(uint64_t));
    bc->ht.values = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!bc->ht.keys || !bc->ht.values) {
        free(bc->ht.keys);   bc->ht.keys   = NULL;
        free(bc->ht.values); bc->ht.values = NULL;
        return -2;
    }
    bc->ht.capacity = cap;
    bc->ht.mask     = cap - 1;

    /* Insert each node */
    for (uint32_t i = 0; i < bc->n_chunks; i++) {
        uint64_t key = bc->nodes[i].origin_key;
        if (key == BOND_KEY_NONE) continue;
        uint32_t slot = _bond_ht_slot(&bc->ht, key);
        bc->ht.keys[slot]   = key;
        bc->ht.values[slot] = i;
    }
    return 0;
}

/*
 * bond_chain_find_ht()
 *
 * O(1) average find by bond_key via hash table.
 * Returns chunk_idx or UINT32_MAX if not found.
 * Falls back to linear scan if hash table not built.
 */
static inline uint32_t bond_chain_find_ht(const BondChain *bc,
                                           uint64_t         target_key) {
    if (!bc || target_key == BOND_KEY_NONE) return UINT32_MAX;

    /* Hash table path — O(1) average */
    if (bc->ht.keys) {
        uint32_t slot = (uint32_t)((target_key ^ (target_key >> 32)) & bc->ht.mask);
        while (bc->ht.keys[slot] != 0) {
            if (bc->ht.keys[slot] == target_key)
                return bc->ht.values[slot];
            slot = (slot + 1) & bc->ht.mask;
        }
        return UINT32_MAX;
    }

    /* Fallback: linear scan (O(n)) */
    for (uint32_t i = 0; i < bc->n_chunks; i++) {
        if (bc->nodes[i].origin_key == target_key) return i;
    }
    return UINT32_MAX;
}

/*
 * bond_chain_free_ht()
 *
 * Free optional hash table overlay. Safe to call even if not built.
 */
static inline void bond_chain_free_ht(BondChain *bc) {
    if (!bc) return;
    free(bc->ht.keys);
    free(bc->ht.values);
    bc->ht.keys     = NULL;
    bc->ht.values   = NULL;
    bc->ht.capacity = 0;
    bc->ht.mask     = 0;
}

/* ── RECONSTRUCT: find chunk_idx by bond_key ───────────────── */
/*
 * bond_chain_find()
 *
 * Deprecated in favor of bond_chain_find_ht().
 * O(n) linear scan. Kept for backward compat.
 * Returns chunk_idx or UINT32_MAX if not found.
 */
static inline uint32_t bond_chain_find(const BondChain *bc,
                                        uint64_t         target_key) {
    return bond_chain_find_ht(bc, target_key);
}

/*
 * bond_chain_walk()
 *
 * Starting from any node, walk the chain in order via next_key.
 * Fills out_order[0..n_chunks-1] with chunk_idx in sequence.
 * Returns number of nodes reached (should equal n_chunks if intact).
 *
 * Works regardless of physical order — nodes can be on any storage.
 */
static inline uint32_t bond_chain_walk(const BondChain *bc,
                                        uint32_t        *out_order,
                                        uint32_t         out_max) {
    if (!bc || !out_order) return 0;

    /* Find head node */
    uint32_t head = UINT32_MAX;
    for (uint32_t i = 0; i < bc->n_chunks; i++) {
        if (bc->nodes[i].flags & BNODE_FLAG_HEAD) { head = i; break; }
    }
    if (head == UINT32_MAX) return 0;

    uint32_t count  = 0;
    uint32_t cur    = head;

    while (cur != UINT32_MAX && count < out_max) {
        out_order[count++] = cur;
        uint64_t nk = bc->nodes[cur].next_key;
        if (nk == BOND_KEY_NONE) break;
        cur = bond_chain_find_ht(bc, nk);
    }

    return count;
}

/* ── STATS ──────────────────────────────────────────────────── */
typedef struct {
    uint32_t n_total;
    uint32_t n_rerouted;
    uint32_t n_head;
    uint32_t n_tail;
    uint32_t n_route[6];    /* count per BOND_ROUTE_* */
    uint32_t chain_broken;  /* next_key not found */
} BondChainStats;

static inline BondChainStats bond_chain_stats(const BondChain *bc) {
    BondChainStats s;
    memset(&s, 0, sizeof(s));
    if (!bc) return s;

    s.n_total = bc->n_valid;
    for (uint32_t i = 0; i < bc->n_chunks; i++) {
        const BondNode *n = &bc->nodes[i];
        if (n->flags & BNODE_FLAG_REROUTED) s.n_rerouted++;
        if (n->flags & BNODE_FLAG_HEAD)     s.n_head++;
        if (n->flags & BNODE_FLAG_TAIL)     s.n_tail++;
        if (n->route < 6) s.n_route[n->route]++;
        if (n->next_key != BOND_KEY_NONE) {
            if (bond_chain_find_ht(bc, n->next_key) == UINT32_MAX)
                s.chain_broken++;
        }
    }
    return s;
}

#endif /* POGLS_BOND_CHAIN_H */

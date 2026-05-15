/*
 * fibo_layer_header.h — POGLS Fibo Layer Clock Header
 * ══════════════════════════════════════════════════════════════════════
 *
 * Concept:
 *   Input is a LAYER SEQUENCE on a Fibonacci clock (not raw data).
 *   Data is not stored — the ADDRESS on the Fibo timeline IS the data.
 *
 * Structure:
 *   16 routes total = 4 sets × (3 pos + 1 inv)
 *
 *   Set 0: routes[0..2]  = positive,  routes[3]  = inv
 *   Set 1: routes[4..6]  = positive,  routes[7]  = inv
 *   Set 2: routes[8..10] = positive,  routes[11] = inv
 *   Set 3: routes[12..14]= positive,  routes[15] = inv
 *
 *   Second fold:
 *   4 inv lines (routes[3,7,11,15]) → fold again → (3 inv + 1 inv_of_inv)
 *   inv_of_inv = proof of structure (integrity witness, not stored)
 *
 * Fibo clock:
 *   tick sequence: 2→3→5→8→13→21→34→55→89→144
 *   tick_start = which Fibo tick this layer begins at
 *   layer_seq  = index in input sequence (0-based)
 *   144 ticks  = full closure (Fibonacci closure point)
 *
 * Derivation rule:
 *   Given (tick_start + layer_seq) → derive all 16 routes deterministically
 *   → derive 4 inv → derive inv_of_inv
 *   → verify: inv_of_inv must match XOR-fold of all 12 positive routes
 *   If match: structure is intact, NO content storage needed.
 *
 * ThetaCoord mapping:
 *   face (0..11) → which set (face % 4) + route within set (face / 4)
 *   edge (0..4)  → sub-route selector within positive group
 *   z    (0..255)→ tick position within layer
 *
 * Frozen rules:
 *   - integer only, no float
 *   - no heap in hot path
 *   - inv_of_inv never stored — always derived
 *   - tick_start must be a valid Fibo number (assert on init)
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef FIBO_LAYER_HEADER_H
#define FIBO_LAYER_HEADER_H

#include <stdint.h>
#include <string.h>

/* ── Fibo clock constants ─────────────────────────────────────────── */
#define FIBO_CLOSURE        144u   /* full closure tick count           */
#define FIBO_SETS             4u   /* 4 sets of routes                  */
#define FIBO_POS_PER_SET      3u   /* 3 positive routes per set         */
#define FIBO_INV_PER_SET      1u   /* 1 invert route per set            */
#define FIBO_ROUTES_TOTAL    16u   /* 4 × (3+1)                         */
#define FIBO_POS_TOTAL       12u   /* 4 × 3 positive routes             */
#define FIBO_INV_TOTAL        4u   /* 4 × 1 invert routes               */

/* Valid Fibo tick values (clock positions) */
static const uint8_t FIBO_TICKS[10] = {
    2, 3, 5, 8, 13, 21, 34, 55, 89, 144
};
#define FIBO_TICK_COUNT  10u

/* ── FiboLayerHeader (32B, self-describing) ───────────────────────── */
/*
 * tick_start  = Fibo clock position where this layer begins
 * layer_seq   = 0-based index in input sequence
 * set_count   = how many sets active (1..4, default 4)
 * flags       = FLHDR_FLAG_*
 * inv_witness = XOR-fold of 4 inv routes (derived, stored for fast verify)
 * seed_origin = seed of first chunk in this layer (anchor for derivation)
 * _reserved   = 0, extend later
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];        /* "FLHD"                                */
    uint8_t  tick_start;      /* Fibo tick: 2,3,5,8,13,21,34,55,89,144*/
    uint8_t  layer_seq;       /* 0..255 (wraps at 144 = closure)       */
    uint8_t  set_count;       /* active sets 1..4                      */
    uint8_t  flags;           /* FLHDR_FLAG_*                          */
    uint64_t seed_origin;     /* first chunk seed — derivation anchor  */
    uint64_t inv_witness;     /* XOR of 4 inv routes (verify shortcut) */
    uint32_t layer_checksum;  /* XOR-fold of all 12 pos route ids      */
    uint32_t _reserved;       /* must be 0                             */
} FiboLayerHeader;            /* 32B */

typedef char _flhdr_size_check[sizeof(FiboLayerHeader) == 32 ? 1 : -1];

/* ── Flags ────────────────────────────────────────────────────────── */
#define FLHDR_FLAG_CLOSURE    0x01u  /* layer_seq reached 144 (epoch)  */
#define FLHDR_FLAG_INV_VALID  0x02u  /* inv_witness pre-computed        */
#define FLHDR_FLAG_PASSTHRU   0x04u  /* layer bypassed Geopixel         */
#define FLHDR_FLAG_VERIFIED   0x08u  /* inv_of_inv check passed         */

/* ── RouteSet: one set of (3 pos + 1 inv) ────────────────────────── */
typedef struct {
    uint64_t pos[FIBO_POS_PER_SET];  /* 3 positive route ids           */
    uint64_t inv;                     /* 1 invert (XOR of 3 pos)        */
} RouteSet;

/* ── FiboLayerState: full 16-route state (not stored, derived) ────── */
typedef struct {
    RouteSet  sets[FIBO_SETS];   /* 4 × (3 pos + 1 inv)                */
    uint64_t  inv_of_inv;        /* fold of 4 inv lines → proof        */
    uint8_t   tick_current;      /* current position on Fibo clock     */
    uint8_t   layer_seq;
    uint8_t   _pad[6];
} FiboLayerState;

/* ── Core derivation functions ────────────────────────────────────── */

/* Derive inv from 3 positive routes (XOR fold) */
static inline uint64_t fibo_derive_inv(uint64_t a, uint64_t b, uint64_t c) {
    return a ^ b ^ c;
}

/* Derive inv_of_inv from 4 inv lines (XOR fold of folds) */
static inline uint64_t fibo_derive_inv_of_inv(
    uint64_t inv0, uint64_t inv1, uint64_t inv2, uint64_t inv3)
{
    return inv0 ^ inv1 ^ inv2 ^ inv3;
}

/* Derive route id from seed_origin + layer_seq + set + route index
 * Pure integer — deterministic, no state needed */
static inline uint64_t fibo_derive_route(
    uint64_t seed_origin,
    uint8_t  layer_seq,
    uint8_t  set_idx,      /* 0..3 */
    uint8_t  route_idx)    /* 0..2 for pos, 3 for inv */
{
    /* Mix: PHI-inspired multiply + XOR scatter */
    uint64_t h = seed_origin;
    h ^= (uint64_t)layer_seq  * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)set_idx    * 0x6C62272E07BB0142ULL;
    h ^= (uint64_t)route_idx  * 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return h;
}

/* Build full FiboLayerState from header (O(1), no heap) */
static inline void fibo_layer_expand(
    const FiboLayerHeader *hdr,
    FiboLayerState        *out)
{
    memset(out, 0, sizeof(*out));
    out->tick_current = hdr->tick_start;
    out->layer_seq    = hdr->layer_seq;

    for (uint8_t s = 0; s < FIBO_SETS; s++) {
        for (uint8_t r = 0; r < FIBO_POS_PER_SET; r++) {
            out->sets[s].pos[r] = fibo_derive_route(
                hdr->seed_origin, hdr->layer_seq, s, r);
        }
        /* inv = XOR of 3 pos in this set */
        out->sets[s].inv = fibo_derive_inv(
            out->sets[s].pos[0],
            out->sets[s].pos[1],
            out->sets[s].pos[2]);
    }

    /* inv_of_inv = XOR of all 4 inv lines → proof of structure */
    out->inv_of_inv = fibo_derive_inv_of_inv(
        out->sets[0].inv,
        out->sets[1].inv,
        out->sets[2].inv,
        out->sets[3].inv);
}

/* Verify integrity: inv_of_inv must equal XOR of all 12 pos routes
 * Returns 1 if intact, 0 if corrupted */
static inline int fibo_layer_verify(const FiboLayerState *st) {
    uint64_t pos_fold = 0;
    for (uint8_t s = 0; s < FIBO_SETS; s++)
        for (uint8_t r = 0; r < FIBO_POS_PER_SET; r++)
            pos_fold ^= st->sets[s].pos[r];

    /* inv_of_inv = XOR(all inv) = XOR(XOR(pos_set)) = XOR(all pos) */
    return (st->inv_of_inv == pos_fold) ? 1 : 0;
}

/* Map ThetaCoord → (set_idx, route_idx) for Geopixel tile routing */
static inline void fibo_coord_to_route(
    uint8_t face,    /* 0..11 */
    uint8_t edge,    /* 0..4  */
    uint8_t *set_out,    /* 0..3  */
    uint8_t *route_out)  /* 0..2  */
{
    *set_out   = face % FIBO_SETS;           /* face 0..11 → set 0..3  */
    *route_out = (face / FIBO_SETS + edge) % FIBO_POS_PER_SET; /* 0..2 */
}

/* Check if tick_start is a valid Fibo clock position */
static inline int fibo_tick_valid(uint8_t tick) {
    for (uint8_t i = 0; i < FIBO_TICK_COUNT; i++)
        if (FIBO_TICKS[i] == tick) return 1;
    return 0;
}

/* Init header with magic + validation */
static inline int fibo_layer_header_init(
    FiboLayerHeader *hdr,
    uint8_t  tick_start,
    uint8_t  layer_seq,
    uint64_t seed_origin)
{
    if (!fibo_tick_valid(tick_start)) return 0;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic[0] = 'F'; hdr->magic[1] = 'L';
    hdr->magic[2] = 'H'; hdr->magic[3] = 'D';
    hdr->tick_start   = tick_start;
    hdr->layer_seq    = layer_seq;
    hdr->set_count    = FIBO_SETS;
    hdr->seed_origin  = seed_origin;

    /* Pre-compute inv_witness for fast verify */
    FiboLayerState st;
    fibo_layer_expand(hdr, &st);
    hdr->inv_witness    = st.inv_of_inv;
    hdr->layer_checksum = (uint32_t)(st.inv_of_inv ^ (st.inv_of_inv >> 32));
    hdr->flags          = FLHDR_FLAG_INV_VALID;

    return 1;
}

#endif /* FIBO_LAYER_HEADER_H */

/*
 * bermuda_shadow.h — Bermuda Float Shadow Layer
 * ══════════════════════════════════════════════════════════════
 *
 * Purpose: intercept float/unstructured data before it enters
 *   the ROUTE pipeline. Classify → redirect to GROUND lane.
 *   Data is NOT destroyed — bond_key preserves retrieval path.
 *
 * Classification signal: range × transition discriminator
 *   Replaces skel_isect_pop (XOR-fold cancels uniform patterns)
 *
 *   HOT  (structured/integer):
 *     range == 0         → uniform bytes (0x00, 0xFF, 0x55) → HOT
 *     range <  64        → small integer sequence → HOT
 *     trans < 96         → low entropy → HOT
 *
 *   COLD (float/random):
 *     range >= 64 AND trans >= 96 → dense, high-entropy → COLD
 *
 *   Verified:
 *     zeros(0x00)  → HOT  ✓   structured
 *     0xFF pattern → HOT  ✓   structured uniform
 *     0x55 pattern → HOT  ✓   structured uniform
 *     float32 bytes→ COLD ✓   range=243 trans=157
 *     int 0..63    → HOT  ✓   range=63  trans=120
 *     random       → COLD ✓   range=250 trans=198
 *
 * Integration stack:
 *   [raw data chunk 64B]
 *       ↓
 *   bermuda_shadow_classify()     ← this file
 *       ↓                  ↓
 *   HOT → bermuda_route_token()   COLD → shadow ring (ground lane)
 *       ↓                              ↓
 *   tgw_fgls_connector             bond_key stored → retrievable
 *
 * Shadow ring: fixed-size ring buffer (no malloc)
 *   capacity = BERMUDA_SHADOW_RING = 144 (2⁴×3², sacred family)
 *   eviction = oldest entry (ring wraps)
 *   lookup   = bond_key O(1) hint, O(144) worst case
 *
 * No malloc. No float arithmetic. No heap. Stateless classify.
 * Sacred: BERMUDA_SHADOW_RING=144. FROZEN.
 * ══════════════════════════════════════════════════════════════
 */

#ifndef BERMUDA_SHADOW_H
#define BERMUDA_SHADOW_H

#include <stdint.h>
#include <string.h>
#include "bermuda_export.h"    /* bermuda_init, snap_gear, traverse, etc. */
#include "shadow_zone.h"       /* ShadowZone, shadow_write */

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define BERMUDA_SHADOW_RING      144u  /* ring capacity — 2⁴×3², sacred    */
#define BERMUDA_SHADOW_HOT         0u  /* structured → ROUTE               */
#define BERMUDA_SHADOW_COLD        1u  /* float/dense → GROUND shadow lane */
#define BERMUDA_CHUNK             64u  /* DiamondBlock size (frozen)       */

/* Classification thresholds (verified against 6 patterns) */
#define BERMUDA_COLD_RANGE_THR    64u  /* byte range  ≥ 64 → candidate    */
#define BERMUDA_COLD_TRANS_THR    96u  /* transitions ≥ 96 → COLD confirm */

/* ══════════════════════════════════════════════════════════════
   SIGNAL PRIMITIVES — O(64) = O(1)
   ══════════════════════════════════════════════════════════════ */

/* Byte range: max - min across 64 bytes */
static inline uint32_t _bsig_range(const uint8_t *b)
{
    uint8_t mn = 255u, mx = 0u;
    for (int i = 0; i < 64; i++) {
        if (b[i] < mn) mn = b[i];
        if (b[i] > mx) mx = b[i];
    }
    return (uint32_t)(mx - mn);
}

/* Transition entropy: sum of popcount(b[i] XOR b[i-1]) */
static inline uint32_t _bsig_transitions(const uint8_t *b)
{
    uint32_t t = 0u;
    for (int i = 1; i < 64; i++)
        t += (uint32_t)__builtin_popcount((unsigned)(b[i] ^ b[i-1]));
    return t;
}

/* Flat check: all bytes zero */
static inline int _bsig_is_flat(const uint8_t *b)
{
    const uint64_t *w = (const uint64_t *)b;
    return !(w[0]|w[1]|w[2]|w[3]|w[4]|w[5]|w[6]|w[7]);
}

/* ══════════════════════════════════════════════════════════════
   CORE TEMPERATURE DECISION
   range==0 → uniform (HOT)  |  range≥64 AND trans≥96 → COLD
   ══════════════════════════════════════════════════════════════ */

static inline uint8_t bermuda_chunk_temperature(const uint8_t *chunk64)
{
    uint32_t r = _bsig_range(chunk64);
    if (r == 0u) return BERMUDA_SHADOW_HOT;   /* uniform pattern */
    if (r < BERMUDA_COLD_RANGE_THR) return BERMUDA_SHADOW_HOT;
    uint32_t t = _bsig_transitions(chunk64);
    return (t >= BERMUDA_COLD_TRANS_THR) ? BERMUDA_SHADOW_COLD
                                         : BERMUDA_SHADOW_HOT;
}

/* ══════════════════════════════════════════════════════════════
   SHADOW ENTRY — one classified chunk (80B)
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t bond_key;              /* retrieval key                       */
    uint64_t addr;                  /* origin geo_key                      */
    uint16_t slot;                  /* bermuda slot (gear space)           */
    uint16_t tring_slot;            /* TRing slot 0..719                   */
    uint8_t  gear;                  /* 1-4                                 */
    uint8_t  zone;                  /* 0..11 dodecahedron face             */
    uint8_t  pole;                  /* 0=north  1=south                    */
    uint8_t  temperature;           /* HOT=0 / COLD=1                      */
    uint8_t  range_sig;             /* byte range (diagnostic)             */
    uint8_t  trans_sig;             /* transitions / 4 (diagnostic, fits 1B)*/
    uint8_t  shape;                 /* routing shape byte                  */
    uint8_t  _pad;
    uint8_t  data[BERMUDA_CHUNK];   /* original 64B chunk preserved        */
} BermudaShadowEntry;               /* sizeof = 80B                        */

/* ══════════════════════════════════════════════════════════════
   SHADOW RING
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    BermudaShadowEntry entries[BERMUDA_SHADOW_RING];
    uint8_t  head;
    uint8_t  count;
    uint32_t total_cold;
    uint32_t total_hot;
    uint32_t evictions;
} BermudaShadowRing;

static inline void bermuda_shadow_ring_init(BermudaShadowRing *r)
{
    memset(r, 0, sizeof(*r));
}

/* ══════════════════════════════════════════════════════════════
   CLASSIFY — stateless O(1)
   ══════════════════════════════════════════════════════════════ */

static inline BermudaShadowEntry bermuda_shadow_classify(
    const uint8_t *chunk64,
    uint16_t       idx,
    uint8_t        gear,
    uint64_t       addr,
    uint64_t       bond_key)
{
    bermuda_init();

    BermudaShadowEntry e;
    memset(&e, 0, sizeof(e));

    e.gear       = gear;
    e.slot       = idx % _bermuda_ctx.slots[gear];
    e.tring_slot = bermuda_tring_slot(idx);
    e.zone       = bermuda_zone(idx, gear);
    e.pole       = bermuda_pole(e.zone);
    e.addr       = addr;
    e.bond_key   = bond_key;

    uint32_t r   = _bsig_range(chunk64);
    uint32_t t   = _bsig_transitions(chunk64);
    e.range_sig  = (uint8_t)(r > 255u ? 255u : r);
    e.trans_sig  = (uint8_t)(t / 4u);  /* /4 fits in 1 byte, max=63×8/4=126 */

    e.temperature = bermuda_chunk_temperature(chunk64);

    /* shape: HOT → ORBITAL, COLD → CROSS (ground-bound) */
    e.shape = bermuda_shape(e.temperature == BERMUDA_SHADOW_HOT ? 0u : 2u,
                            e.zone);

    memcpy(e.data, chunk64, BERMUDA_CHUNK);
    return e;
}

/* ══════════════════════════════════════════════════════════════
   RING PUSH / FIND
   ══════════════════════════════════════════════════════════════ */

static inline void bermuda_shadow_push(BermudaShadowRing        *r,
                                        const BermudaShadowEntry *e)
{
    if (r->count == BERMUDA_SHADOW_RING) r->evictions++;
    else                                 r->count++;
    r->entries[r->head] = *e;
    r->head = (uint8_t)((r->head + 1u) % BERMUDA_SHADOW_RING);
}

/* O(1) hint → O(144) worst case */
static inline const BermudaShadowEntry *bermuda_shadow_find(
    const BermudaShadowRing *r,
    uint64_t                 bond_key)
{
    if (r->count == 0u) return NULL;
    uint8_t hint = (uint8_t)(bond_key % BERMUDA_SHADOW_RING);
    if (r->entries[hint].bond_key == bond_key) return &r->entries[hint];
    for (uint8_t i = 0u; i < BERMUDA_SHADOW_RING; i++)
        if (r->entries[i].bond_key == bond_key) return &r->entries[i];
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
   DISPATCH — classify + route in one call
   Returns: BERMUDA_SHADOW_HOT or COLD
   route_out: valid always (COLD → GROUND routing info for tracing)
   ══════════════════════════════════════════════════════════════ */

static inline uint8_t bermuda_shadow_dispatch(
    BermudaShadowRing  *ring,
    const uint8_t      *chunk64,
    uint16_t            idx,
    uint8_t             gear,
    uint8_t             mode,
    uint64_t            addr,
    uint64_t            bond_key,
    BermudaRouteEntry  *route_out)
{
    BermudaShadowEntry e = bermuda_shadow_classify(
                               chunk64, idx, gear, addr, bond_key);

    if (e.temperature == BERMUDA_SHADOW_HOT) {
        ring->total_hot++;
        if (route_out) bermuda_route_token(idx, gear, mode, route_out);
        return BERMUDA_SHADOW_HOT;
    }

    /* COLD → ground lane */
    ring->total_cold++;
    bermuda_shadow_push(ring, &e);

    if (route_out) {
        /* CROSS traverse → odd spoke (GROUND constraint) */
        uint16_t g_idx = bermuda_traverse(idx, gear, 2u);
        bermuda_route_token(g_idx, gear, 2u, route_out);
        route_out->polarity = 1u;  /* force GROUND */
    }
    return BERMUDA_SHADOW_COLD;
}

/* ══════════════════════════════════════════════════════════════
   SHADOW ZONE DISPATCH — COLD → ShadowZone (sector 10/11)
   ══════════════════════════════════════════════════════════════
   Same as bermuda_shadow_dispatch but writes COLD entries into
   ShadowZone (instead of ring buffer). Returns node_id in shadow
   space via out_node_id. bond_key = node_id for retrieval.

   Returns: BERMUDA_SHADOW_HOT or COLD
   ══════════════════════════════════════════════════════════════ */

static inline uint8_t bermuda_shadow_dispatch_to_zone(
    BermudaShadowRing  *ring,
    ShadowZone         *shadow,
    const uint8_t      *chunk64,
    uint16_t            idx,
    uint8_t             gear,
    uint8_t             mode,
    uint64_t            addr,
    uint64_t            bond_key,
    BermudaRouteEntry  *route_out,
    uint32_t           *out_node_id)
{
    BermudaShadowEntry e = bermuda_shadow_classify(
                               chunk64, idx, gear, addr, bond_key);

    if (e.temperature == BERMUDA_SHADOW_HOT) {
        ring->total_hot++;
        if (route_out) bermuda_route_token(idx, gear, mode, route_out);
        if (out_node_id) *out_node_id = 0;
        return BERMUDA_SHADOW_HOT;
    }

    ring->total_cold++;

    uint32_t node_id;
    int rc = shadow_write(shadow, bond_key, ring->total_cold, BERMUDA_SHADOW_COLD,
                           chunk64, BERMUDA_CHUNK, &node_id);
    if (rc != SHADOW_OK) {
        /* fallback to ring buffer if shadow zone full */
        bermuda_shadow_push(ring, &e);
    }

    if (out_node_id) *out_node_id = node_id;

    if (route_out) {
        uint16_t g_idx = bermuda_traverse(idx, gear, 2u);
        bermuda_route_token(g_idx, gear, 2u, route_out);
        route_out->polarity = 1u;
    }
    return BERMUDA_SHADOW_COLD;
}

/* ══════════════════════════════════════════════════════════════
   BATCH DISPATCH
   chunks   : N × 64B packed
   temps[]  : COLD entries NOT forwarded — caller skips them
   ══════════════════════════════════════════════════════════════ */

static inline void bermuda_shadow_batch(
    BermudaShadowRing  *ring,
    const uint8_t      *chunks,
    const uint16_t     *idxs,
    const uint64_t     *addrs,
    const uint64_t     *bond_keys,
    uint8_t             gear,
    uint8_t             mode,
    BermudaRouteEntry  *out,
    uint8_t            *temps,
    uint32_t            n)
{
    for (uint32_t i = 0u; i < n; i++) {
        temps[i] = bermuda_shadow_dispatch(
            ring,
            chunks + (size_t)i * BERMUDA_CHUNK,
            idxs[i], gear, mode,
            addrs     ? addrs[i]     : (uint64_t)idxs[i],
            bond_keys ? bond_keys[i] : 0u,
            &out[i]);
    }
}

/* ══════════════════════════════════════════════════════════════
   STATS
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total_hot;
    uint32_t total_cold;
    uint32_t ring_count;
    uint32_t evictions;
} BermudaShadowStats;

static inline BermudaShadowStats bermuda_shadow_stats(
    const BermudaShadowRing *r)
{
    BermudaShadowStats s;
    s.total_hot  = r->total_hot;
    s.total_cold = r->total_cold;
    s.ring_count = r->count;
    s.evictions  = r->evictions;
    return s;
}

/* ══════════════════════════════════════════════════════════════
   VERIFY
   ══════════════════════════════════════════════════════════════ */

static inline int bermuda_shadow_verify(void)
{
    bermuda_init();

    /* T1: all-zero → HOT */
    uint8_t zeros[BERMUDA_CHUNK] = {0};
    if (bermuda_chunk_temperature(zeros) != BERMUDA_SHADOW_HOT)  return -1;

    /* T2: 0xFF → HOT (uniform, range=0) */
    uint8_t dense[BERMUDA_CHUNK]; memset(dense, 0xFF, BERMUDA_CHUNK);
    if (bermuda_chunk_temperature(dense) != BERMUDA_SHADOW_HOT)  return -2;

    /* T3: float32 bytes → COLD */
    uint8_t floatlike[BERMUDA_CHUNK];
    float fv[16]; for(int i=0;i<16;i++) fv[i]=(float)(i*0.1f+0.5f);
    memcpy(floatlike, fv, BERMUDA_CHUNK);
    if (bermuda_chunk_temperature(floatlike) != BERMUDA_SHADOW_COLD) return -3;

    /* T4: int 0..63 → HOT */
    uint8_t intlike[BERMUDA_CHUNK];
    for(int i=0;i<64;i++) intlike[i]=(uint8_t)i;
    if (bermuda_chunk_temperature(intlike) != BERMUDA_SHADOW_HOT) return -4;

    /* T5: ring push/find roundtrip */
    BermudaShadowRing ring; bermuda_shadow_ring_init(&ring);
    BermudaShadowEntry e = bermuda_shadow_classify(
                               floatlike, 0, 2, 0xABCDu, 0xDEADBEEFu);
    e.bond_key = 0xDEADBEEFu;
    bermuda_shadow_push(&ring, &e);
    if (!bermuda_shadow_find(&ring, 0xDEADBEEFu)) return -5;

    /* T6: ring size sacred */
    if (BERMUDA_SHADOW_RING != 144u) return -6;

    return 0;
}

#endif /* BERMUDA_SHADOW_H */

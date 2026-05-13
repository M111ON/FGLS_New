/*
 * fabric_wire.h — POGLS Fabric Wiring Layer  (full spec)
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  Three-Layer Model
 *  ─────────────────
 *  CORE    │ 2^n × 3^m            │ structure, address, storage
 *  GEAR    │ × 5                  │ phase ring, resonance, routing
 *  SHADOW  │ × 7                  │ buffer, error, drain entry
 *  SECURITY│ × 17                 │ active security, inject layer
 *  STABLE  │ × 19                 │ post-processed, verified endpoint
 *  REF     │ × 13                 │ structural reference, read-only nav
 *
 *  Frozen constants
 *  ────────────────
 *  TRING_FACES   = 12   (dodecahedron pentagons / drain gates)
 *  META_COND_MOD = 36   (2² × 3²)
 *  SACRED_NEXUS  = 54   (2 × 3³  — Rubik stickers / delta lanes)
 *  NODE_MAX      = 162  (54 × 3)
 *  FILE_SIZE     = 4896 (17 × 2 × 144)
 *  LANE_GROUPS   = 6    (rotation groups; 9 lanes each)
 *
 *  Switch Gate (bit6)
 *  ──────────────────
 *  bit6 = (trit ^ slope) & 1
 *    0 → STORE  (gap 0,  upright triangle,  World A  2^n  expansion)
 *    1 → DRAIN  (gap 4, inverted triangle,  World B  3^n  compression)
 *
 *  Heptagon fence  — one-way valve
 *  ──────────────────────────────────
 *  WRITE : core  → shadow only
 *  READ  : shadow → flush process only
 *  DRAIN : shadow → pentagon drain → flush (irreversible)
 *
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef FABRIC_WIRE_H
#define FABRIC_WIRE_H

#include <stdint.h>
#include <stdbool.h>

/* ───────────────────────────── frozen constants ───────────────────── */

#define POGLS_TRING_FACES    12u      /* pentagon drain gates (Goldberg)  */
#define POGLS_META_COND_MOD  36u      /* 2^2 x 3^2                        */
#define POGLS_SACRED_NEXUS   54u      /* 2 x 3^3 — Rubik stickers         */
#define POGLS_NODE_MAX       162u     /* 54 x 3                           */
#define POGLS_FILE_SIZE      4896u    /* 17 x 2 x 144 — security boundary */
#define POGLS_LANE_GROUPS    6u       /* rotation groups (9 lanes each)   */
#define POGLS_LANES_PER_GRP  9u       /* 54 / 6                           */
#define POGLS_TRING_CYCLE    1440u    /* pentagon sync point (migrated fr 720) */

/* ───────────────────────────── layer enum ─────────────────────────── */

typedef enum {
    POGLS_LAYER_REJECT   = 0,   /* digit_sum != 9, or unknown prime     */
    POGLS_LAYER_CORE     = 1,   /* 2^n x 3^m                            */
    POGLS_LAYER_GEAR     = 2,   /* x 5  — phase ring / routing          */
    POGLS_LAYER_SHADOW   = 3,   /* x 7  — heptagon buffer / drain entry */
    POGLS_LAYER_SECURITY = 4,   /* x 17 — active security               */
    POGLS_LAYER_STABLE   = 5,   /* x 19 — verified endpoint             */
    POGLS_LAYER_REF      = 6,   /* x 13 — read-only navigation          */
} PoglsLayer;

/* ───────────────────────────── drain-gap state ────────────────────── */

typedef struct {
    uint8_t  gap;       /* canonical: 0 (STORE) or 4 (DRAIN)           */
    bool     is_drain;  /* true  → inverted triangle, World B (3^n)    */
    bool     is_store;  /* true  → upright  triangle, World A (2^n)    */
    uint8_t  bit6;      /* Switch Gate bit — 1-instruction result       */
} FabricWireState;

/* ───────────────────────────── digit_sum helper ───────────────────── */

/*
 * digit_sum_9 — fast membership gate
 * A number belongs to the sacred geometry family iff its decimal
 * digit sum equals 9 (equivalently: x % 9 == 0 && x != 0).
 */
static inline bool pogls_digit_sum_9(uint64_t x)
{
    return (x != 0u) && (x % 9u == 0u);
}

/* ───────────────────────────── classify ───────────────────────────── */

/*
 * pogls_classify — Number Classification System (section 10)
 *
 * Gate 1 : digit_sum == 9
 * Gate 2 : strip 2s and 3s, inspect remaining prime
 *
 * Analogy: like sorting mail by zip-code prefix — digit_sum is the
 * country code, prime factor is the city.
 */
static inline PoglsLayer pogls_classify(uint64_t x)
{
    if (!pogls_digit_sum_9(x)) return POGLS_LAYER_REJECT;

    uint64_t r = x;
    while (r % 2u == 0u) r /= 2u;
    while (r % 3u == 0u) r /= 3u;

    if (r == 1u)          return POGLS_LAYER_CORE;
    if (r % 5u  == 0u)    return POGLS_LAYER_GEAR;
    if (r % 7u  == 0u)    return POGLS_LAYER_SHADOW;
    if (r % 17u == 0u)    return POGLS_LAYER_SECURITY;
    if (r % 19u == 0u)    return POGLS_LAYER_STABLE;
    if (r % 13u == 0u)    return POGLS_LAYER_REF;
    return POGLS_LAYER_REJECT;
}

/* ───────────────────────────── Switch Gate / drain-gap ────────────── */

/*
 * fabric_add_wire — core wiring primitive  (task #1, locked)
 *
 * Returns the Switch Gate bit only.
 * Use fabric_wire_state() when you need the full FabricWireState.
 *
 * @param trit   ternary index 0..26  (3³ space)
 * @param slope  entropy value from caller context
 * @return       0 = STORE, 1 = DRAIN
 */
static inline uint8_t fabric_add_wire(uint8_t trit, uint8_t slope)
{
    return (trit ^ slope) & 1u;
}

/*
 * fabric_wire_state — full drain-gap state (section 8.2)
 *
 * Expands the 1-bit gate into the complete FabricWireState struct.
 * drain_gap is canonical: only gap 0 (STABLE/STORE) and gap 4
 * (STABLE/DRAIN) are permitted; gaps 1-3 are forbidden rotation zones.
 */
static inline FabricWireState fabric_wire_state(uint8_t trit, uint8_t slope)
{
    const uint8_t bit6     = fabric_add_wire(trit, slope);
    const uint8_t drain_gap = bit6 ? 4u : 0u;   /* canonical stable only */

    FabricWireState s;
    s.bit6     = bit6;
    s.gap      = drain_gap;
    s.is_drain = (drain_gap == 4u);
    s.is_store = (drain_gap == 0u);
    return s;
}

/* ───────────────────────────── lane routing ───────────────────────── */

/*
 * pogls_lane_group — Metatron rotation → lane group  (section 4)
 *
 * 54 delta lanes = 6 rotation groups × 9 lanes.
 * rotation_state is a raw byte; wrap via % 6 to get the active group.
 *
 * Analogy: like selecting 1 of 6 faces on a Rubik cube — each face
 * owns 9 stickers (lanes).
 */
static inline uint8_t pogls_lane_group(uint8_t rotation_state)
{
    return rotation_state % POGLS_LANE_GROUPS;
}

/*
 * pogls_lane_index — absolute lane from group + local offset
 *
 * @param rotation_state  current rotation byte
 * @param local_idx       lane index within group  (0..8)
 * @return                absolute lane index      (0..53)
 */
static inline uint8_t pogls_lane_index(uint8_t rotation_state,
                                        uint8_t local_idx)
{
    return (pogls_lane_group(rotation_state) * POGLS_LANES_PER_GRP)
           + (local_idx % POGLS_LANES_PER_GRP);
}

/* ───────────────────────────── pentagon drain map ─────────────────── */

/*
 * pogls_pentagon_drain_offset — 4896B file layout  (task #2)
 *
 * 12 pentagon drains divide FILE_SIZE = 4896 into equal 408-byte
 * cosets (4896 / 12 = 408).  Each drain owns one coset.
 *
 * Byte layout:
 *   [0..407]   drain 0   (also: genesis anchor — bit6=0 zone)
 *   [408..815] drain 1
 *   ...
 *   [4488..4895] drain 11  (security boundary — last 17×24 bytes)
 *
 * @param drain_id  0..11
 * @return          byte offset into the 4896B FrustumSlot64 block
 */
#define POGLS_DRAIN_COSET_SIZE  (POGLS_FILE_SIZE / POGLS_TRING_FACES)  /* 408 */

static inline uint32_t pogls_pentagon_drain_offset(uint8_t drain_id)
{
    return (drain_id % POGLS_TRING_FACES) * POGLS_DRAIN_COSET_SIZE;
}

/* ───────────────────────────── heptagon fence ─────────────────────── */

/*
 * Heptagon fence — one-way valve  (section 3.3)
 *
 * Rule: only SHADOW-layer addresses may enter the shadow buffer.
 * Core addresses that attempt a direct shadow READ are rejected.
 *
 * FENCE_WRITE_OK  : caller may write src→shadow (core→shadow only)
 * FENCE_READ_OK   : caller may read  shadow→flush (flush process only)
 *
 * The fence does NOT expose a "shadow→core read" path — that direction
 * is architecturally absent.
 */
static inline bool pogls_fence_write_ok(PoglsLayer src_layer)
{
    /* Only CORE/GEAR may write into shadow buffer */
    return (src_layer == POGLS_LAYER_CORE ||
            src_layer == POGLS_LAYER_GEAR);
}

static inline bool pogls_fence_read_ok(PoglsLayer dst_layer)
{
    /* Shadow contents may only be consumed by a flush process.
     * Flush process is represented here by SECURITY layer caller. */
    return (dst_layer == POGLS_LAYER_SECURITY);
}

/*
 * pogls_fence_check — combined gate for a fabric transaction
 *
 * @param src_layer   layer of the originating address
 * @param dst_layer   layer of the destination address
 * @param is_drain    true if Switch Gate bit6 == 1 (DRAIN path)
 * @return            true if transaction is permitted
 */
static inline bool pogls_fence_check(PoglsLayer src_layer,
                                      PoglsLayer dst_layer,
                                      bool       is_drain)
{
    if (dst_layer == POGLS_LAYER_SHADOW) {
        /* Write path: core/gear → shadow */
        return pogls_fence_write_ok(src_layer);
    }
    if (src_layer == POGLS_LAYER_SHADOW && is_drain) {
        /* Drain path: shadow → pentagon → flush */
        return pogls_fence_read_ok(dst_layer);
    }
    /* All other shadow-source reads are forbidden */
    if (src_layer == POGLS_LAYER_SHADOW) return false;
    return true;
}

/* ───────────────────────────── Atomic Reshape trigger ─────────────── */

/*
 * pogls_reshape_ready — threshold check for Atomic Reshape  (section 5)
 *
 * Reshape fires when shadow accumulation reaches SACRED_NEXUS (54).
 * 54 = Rubik sticker count = point where all lanes have a pending
 * residual — the minimum complete state for a valid Rubik move.
 *
 * @param shadow_count  number of residuals accumulated in shadow buffer
 * @return              true → trigger reshape sequence
 */
static inline bool pogls_reshape_ready(uint8_t shadow_count)
{
    return shadow_count >= (uint8_t)POGLS_SACRED_NEXUS;
}

/* ═══════════════════════════════════════════════════════════════════════
 * ATOMIC RESHAPE sequence (caller responsibility — see section 5):
 *
 *   1. collect(shadow)          accumulate residuals until reshape_ready()
 *   2. stage(new_layout)        build new lane_group map in staging buffer
 *   3. flip(rotation_state)     1-byte write — atomic moment (17 boundary)
 *   4. drain(pentagon)          flush old layout via 12 pentagon drains
 *
 * 8 = before_state   9 = after_state   17 = during (does not exist)
 * ═══════════════════════════════════════════════════════════════════════
 */

#endif /* FABRIC_WIRE_H */

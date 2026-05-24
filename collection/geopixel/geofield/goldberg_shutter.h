/*
 * goldberg_shutter.h — Goldberg Sphere Shutter System
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  Geometry chain (LOCKED):
 *    Dodecahedron (12 pentagons)
 *      → icosa fan (6 tri × 27 = 162 = NODE_MAX) per pentagon
 *        → hex fabric (54 DiamondBlocks)
 *          → pentagon drain → big flush → spawn (future)
 *
 *  Shutter families:
 *  ─────────────────
 *  PENTAGON  5 trapezoids × 12 = 60 total   SECURITY/DRAIN  gap 0 or 4
 *  HEXAGON   6 triangles  × 18 = 108 total  GEAR            routing/phase
 *  ICOSA     6 triangles  per pentagon       BRIDGE          penta↔hex
 *
 *  Ring distance = confidence gradient (Goldberg sphere):
 *    distance 5 = far field, just written, unstable  (SHADOW)
 *    distance 4 = staging                            (SHADOW)
 *    distance 3 = pending confirm                    (GEAR)
 *    distance 2 = near confirmed                     (GEAR)
 *    distance 1 = proximity-confirmed, ready-to-die  (CORE)
 *    pentagon   = absolute commit → big flush → seal (SECURITY)
 *
 *  Big flush sequence:
 *    5 → 4 → 3 → 2 → 1 → pentagon seal
 *    (confidence increases inward, flush propagates inward)
 *
 *  Hilbert address: stored in trapezoid struct, NOT wired yet.
 *  Spawn hook: stub only — scale-out is future work.
 *
 *  No malloc. No float. No heap.
 *  All constants geometry-derived, none arbitrary.
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef GOLDBERG_SHUTTER_H
#define GOLDBERG_SHUTTER_H

#include <stdint.h>
#include <stdbool.h>
#include "fabric_wire.h"        /* PoglsLayer, POGLS_* constants        */
#include "fabric_wire_drain.h"  /* DRAIN_BIT_*, SHADOW_BIT_*, WireResult */
#include "heptagon_fence.h"     /* FenceResult, heptagon_fence_*        */
#include "frustum_layout_v2.h"  /* FrustumBlock, FrustumMeta            */

/* ═══════════════════════════════════════════════════════════════════════
 *  FROZEN GEOMETRY CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════ */

#define GS_PENTAGON_TRAP     5u    /* trapezoids per pentagon             */
#define GS_PENTAGON_COUNT    12u   /* Goldberg/Euler forced               */
#define GS_TRAP_TOTAL        60u   /* 5 × 12                              */

#define GS_HEX_TRI           6u    /* triangles per hexagon               */
#define GS_HEX_SHUTTER       18u   /* 6×3 = GEAR family (2×3²)           */

#define GS_ICOSA_TRI         6u    /* icosa fan triangles per pentagon    */
#define GS_ICOSA_TRIT        27u   /* 3³ trit space per triangle          */
#define GS_NODE_MAX          162u  /* 6×27 = NODE_MAX (geometry-proven)   */

#define GS_RING_MAX          5u    /* max ring distance from pentagon      */
#define GS_RING_MIN          1u    /* min ring distance (confirmed zone)   */

/* ═══════════════════════════════════════════════════════════════════════
 *  RING DISTANCE → LAYER MAPPING
 *  Confidence gradient: 5=unstable → 1=confirmed → 0=pentagon(drain)
 * ═══════════════════════════════════════════════════════════════════════ */

static inline PoglsLayer gs_ring_layer(uint8_t ring)
{
    switch (ring) {
        case 5: return POGLS_LAYER_SHADOW;    /* far field, unstable      */
        case 4: return POGLS_LAYER_SHADOW;    /* staging                  */
        case 3: return POGLS_LAYER_GEAR;      /* pending confirm          */
        case 2: return POGLS_LAYER_GEAR;      /* near confirmed           */
        case 1: return POGLS_LAYER_CORE;      /* proximity-confirmed      */
        case 0: return POGLS_LAYER_SECURITY;  /* pentagon = drain/seal    */
        default: return POGLS_LAYER_REJECT;
    }
}

/* confidence: ring 1 = highest (4), ring 5 = lowest (0) */
static inline uint8_t gs_ring_confidence(uint8_t ring)
{
    if (ring == 0u || ring > GS_RING_MAX) return 0u;
    return (uint8_t)(GS_RING_MAX + 1u - ring);   /* ring1=5, ring5=1 */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  TRAPEZOID — pentagon shutter unit
 *  5 per pentagon, 60 total across Goldberg sphere
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Trapezoid gap states (from section 8, LOCKED):
 *   gap 0 = STORE  upright triangle  World A (2^n)
 *   gap 4 = DRAIN  inverted triangle World B (3^n)
 *   gap 1,2,3 = FORBIDDEN (rotation zone)
 *
 * Shutter state bits:
 *   bit0 = open     (1 = shutter open, data can pass)
 *   bit1 = flushing (big flush in progress)
 *   bit2 = sealed   (pentagon commit, irreversible until spawn)
 *   bit3 = hilbert_valid (Hilbert address computed)
 *   bit4-7 = ring_distance stored at write time (0..5)
 */

#define TRAP_BIT_OPEN         (1u << 0)
#define TRAP_BIT_FLUSHING     (1u << 1)
#define TRAP_BIT_SEALED       (1u << 2)
#define TRAP_BIT_HILBERT      (1u << 3)
#define TRAP_RING_SHIFT       4u
#define TRAP_RING_MASK        0xF0u   /* bits 4..7 = ring distance */

typedef struct {
    uint8_t  gap;            /* canonical: 0=STORE, 4=DRAIN              */
    uint8_t  state;          /* TRAP_BIT_* flags + ring in bits 4..7     */
    uint8_t  trap_id;        /* 0..4 within pentagon                      */
    uint8_t  penta_id;       /* 0..11 pentagon index                      */
    uint32_t hilbert_addr;   /* Hilbert address (stored, not wired yet)   */
    uint32_t hilbert_pair;   /* paired address on target dodeca (future)  */
} GsTrapezoid;

static inline void gs_trap_set_ring(GsTrapezoid *t, uint8_t ring)
{
    t->state = (uint8_t)((t->state & ~TRAP_RING_MASK)
                         | ((ring & 0x0Fu) << TRAP_RING_SHIFT));
}

static inline uint8_t gs_trap_get_ring(const GsTrapezoid *t)
{
    return (uint8_t)((t->state & TRAP_RING_MASK) >> TRAP_RING_SHIFT);
}

static inline bool gs_trap_sealed(const GsTrapezoid *t)
{
    return (bool)(t->state & TRAP_BIT_SEALED);
}

static inline bool gs_trap_open(const GsTrapezoid *t)
{
    return (bool)(t->state & TRAP_BIT_OPEN);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  HEX SHUTTER — hexagon routing unit
 *  6 triangles × 3 = 18 per hexagon (GEAR family = 2×3²)
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Hex shutter is GEAR layer — routing/phase ring, not drain.
 * 18 triangles = phase slots for ring distance transitions.
 *
 * state bits:
 *   bit0 = active     (routing enabled)
 *   bit1 = phase_lock (locked to current 1440 phase)
 *   bit2 = ring_dirty (ring distance needs recompute)
 *   bit3-7 = lane_group (0..5, which rotation lane owns this hex)
 */

#define HEX_BIT_ACTIVE      (1u << 0)
#define HEX_BIT_PHASE_LOCK  (1u << 1)
#define HEX_BIT_RING_DIRTY  (1u << 2)
#define HEX_LANE_SHIFT      3u
#define HEX_LANE_MASK       0xF8u   /* bits 3..7 = lane_group */

typedef struct {
    uint8_t  state;          /* HEX_BIT_* + lane_group in bits 3..7      */
    uint8_t  ring;           /* distance from nearest pentagon (1..5)     */
    uint8_t  hex_id;         /* index within fabric (0..53 for 54 diamonds)*/
    uint8_t  tri_mask;       /* 6-bit mask: which triangles active        */
} GsHexShutter;

static inline void gs_hex_set_lane(GsHexShutter *h, uint8_t lane_group)
{
    h->state = (uint8_t)((h->state & ~HEX_LANE_MASK)
                         | ((lane_group & 0x1Fu) << HEX_LANE_SHIFT));
}

static inline uint8_t gs_hex_get_lane(const GsHexShutter *h)
{
    return (uint8_t)((h->state & HEX_LANE_MASK) >> HEX_LANE_SHIFT);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ICOSA FAN — bridge adapter between pentagon and hex ring 1
 *  6 triangles per pentagon × 27 (3³) = 162 = NODE_MAX
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  penta_id;       /* which pentagon this fan belongs to (0..11)*/
    uint8_t  tri_active;     /* 6-bit mask: which of 6 triangles active   */
    uint8_t  trit_state[6];  /* per-triangle trit (0..26 = 3³)            */
    uint8_t  _pad;
} GsIcosaFan;

/* node address within icosa fan: tri_idx × 27 + trit */
static inline uint8_t gs_icosa_node(uint8_t tri_idx, uint8_t trit)
{
    return (uint8_t)((tri_idx % GS_ICOSA_TRI) * GS_ICOSA_TRIT
                     + (trit % GS_ICOSA_TRIT));
    /* result: 0..161 = NODE_MAX-1 ✓ */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PENTAGON SHUTTER STATE — full per-pentagon context
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t      penta_id;               /* 0..11                         */
    GsTrapezoid  trap[GS_PENTAGON_TRAP]; /* 5 trapezoids                  */
    GsIcosaFan   icosa;                  /* 6-tri adapter fan             */
    uint8_t      flush_ring;             /* current flush ring (5..0)     */
    uint8_t      sealed;                 /* 1 = pentagon fully sealed      */
    uint8_t      _pad[2];
} GsPentagonShutter;

/* ═══════════════════════════════════════════════════════════════════════
 *  GOLDBERG SHUTTER CONTEXT — full sphere state
 *  Lives on stack or in caller-managed buffer — no heap
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    GsPentagonShutter penta[GS_PENTAGON_COUNT];  /* 12 pentagons          */
    GsHexShutter      hex[FGLS_DIAMOND_COUNT];   /* 54 hex shutters       */
    uint8_t           active_flush_ring;          /* global flush progress */
    uint8_t           sealed_count;               /* how many pentas sealed*/
    uint8_t           _pad[2];
} GoldbergShutter;

/* ═══════════════════════════════════════════════════════════════════════
 *  INIT
 * ═══════════════════════════════════════════════════════════════════════ */

static inline void gs_init(GoldbergShutter *gs)
{
    uint8_t p, t, h;

    gs->active_flush_ring = GS_RING_MAX;   /* start at ring 5 */
    gs->sealed_count      = 0u;
    gs->_pad[0] = gs->_pad[1] = 0u;

    for (p = 0u; p < GS_PENTAGON_COUNT; p++) {
        GsPentagonShutter *ps = &gs->penta[p];
        ps->penta_id   = p;
        ps->flush_ring = GS_RING_MAX;
        ps->sealed     = 0u;

        for (t = 0u; t < GS_PENTAGON_TRAP; t++) {
            GsTrapezoid *tr = &ps->trap[t];
            tr->gap          = 0u;           /* STORE by default           */
            tr->state        = 0u;
            tr->trap_id      = t;
            tr->penta_id     = p;
            tr->hilbert_addr = 0u;
            tr->hilbert_pair = 0u;
            gs_trap_set_ring(tr, GS_RING_MAX);
        }

        ps->icosa.penta_id   = p;
        ps->icosa.tri_active = 0x3Fu;   /* all 6 triangles active (0b111111) */
        for (t = 0u; t < GS_ICOSA_TRI; t++)
            ps->icosa.trit_state[t] = 0u;
        ps->icosa._pad = 0u;
    }

    for (h = 0u; h < FGLS_DIAMOND_COUNT; h++) {
        GsHexShutter *hs = &gs->hex[h];
        hs->state    = HEX_BIT_ACTIVE;
        hs->ring     = GS_RING_MAX;      /* start far, confirm inward      */
        hs->hex_id   = h;
        hs->tri_mask = 0x3Fu;            /* all 6 triangles active         */
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  RING DISTANCE OPERATIONS
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * gs_hex_confirm — move hex one ring closer to pentagon
 * Called when data at this hex survives a reshape cycle.
 * ring 5→4→3→2→1 = confidence increases each cycle.
 */
static inline bool gs_hex_confirm(GsHexShutter *h)
{
    if (h->ring <= GS_RING_MIN) return false;   /* already at ring 1      */
    h->ring--;
    h->state |= HEX_BIT_RING_DIRTY;
    return true;
}

/*
 * gs_hex_reset_ring — reset to ring 5 after flush
 * Called after hex data is flushed through pentagon drain.
 */
static inline void gs_hex_reset_ring(GsHexShutter *h)
{
    h->ring   = GS_RING_MAX;
    h->state &= ~HEX_BIT_RING_DIRTY;
}

/*
 * gs_trap_open_shutter — open trapezoid for flush
 * gap=4 (DRAIN) only — gap=0 stays STORE, never flushes
 */
static inline bool gs_trap_open_shutter(GsTrapezoid *t, uint8_t ring)
{
    if (gs_trap_sealed(t)) return false;      /* sealed = no re-open      */
    t->gap    = 4u;                           /* DRAIN = inverted triangle */
    t->state |= TRAP_BIT_OPEN | TRAP_BIT_FLUSHING;
    gs_trap_set_ring(t, ring);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  BIG FLUSH SEQUENCE
 *  Propagates inward: ring 5 → 4 → 3 → 2 → 1 → pentagon seal
 *  Each step: flush all hex at current ring through open trapezoids
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    GS_FLUSH_OK        =  0,   /* ring flushed successfully              */
    GS_FLUSH_CONTINUE  =  1,   /* more rings remaining                   */
    GS_FLUSH_SEALED    =  2,   /* pentagon sealed — flush complete        */
    GS_FLUSH_SKIP      =  3,   /* no data at this ring                   */
    GS_FLUSH_DENY      = -1,   /* fence denied flush                     */
} GsFlushResult;

/*
 * gs_flush_ring — flush one ring level on one pentagon
 *
 * Opens all 5 trapezoids of pentagon p at current ring level.
 * Updates hex shutters at this ring → reset to ring 5 after flush.
 * Advances flush_ring inward (ring-- toward pentagon).
 *
 * @param gs        GoldbergShutter context
 * @param block     FrustumBlock to update drain_state
 * @param penta_id  which pentagon to flush (0..11)
 * @return          GsFlushResult
 */
static inline GsFlushResult gs_flush_ring(GoldbergShutter *gs,
                                           FrustumBlock    *block,
                                           uint8_t          penta_id)
{
    GsPentagonShutter *ps = &gs->penta[penta_id % GS_PENTAGON_COUNT];

    if (ps->sealed) return GS_FLUSH_SEALED;

    uint8_t ring = ps->flush_ring;
    if (ring == 0u) {
        /* ring 0 = pentagon seal */
        ps->sealed = 1u;
        gs->sealed_count++;
        /* seal all trapezoids */
        for (uint8_t t = 0u; t < GS_PENTAGON_TRAP; t++) {
            ps->trap[t].state |= TRAP_BIT_SEALED;
            ps->trap[t].state &= ~(TRAP_BIT_OPEN | TRAP_BIT_FLUSHING);
        }
        /* trigger drain on FrustumBlock */
        fabric_drain_flush(block, penta_id);
        return GS_FLUSH_SEALED;
    }

    /* open all 5 trapezoids for this ring */
    uint8_t opened = 0u;
    for (uint8_t t = 0u; t < GS_PENTAGON_TRAP; t++) {
        if (gs_trap_open_shutter(&ps->trap[t], ring))
            opened++;
    }

    if (opened == 0u) return GS_FLUSH_SKIP;

    /* reset hex shutters at this ring (mark as flushed) */
    uint8_t flushed_hex = 0u;
    for (uint8_t h = 0u; h < FGLS_DIAMOND_COUNT; h++) {
        if (gs->hex[h].ring == ring) {
            gs_hex_reset_ring(&gs->hex[h]);
            flushed_hex++;
        }
    }

    /* advance ring inward */
    ps->flush_ring = (ring > 0u) ? ring - 1u : 0u;
    gs->active_flush_ring = ps->flush_ring;

    return (ps->flush_ring == 0u) ? GS_FLUSH_CONTINUE : GS_FLUSH_CONTINUE;
}

/*
 * gs_big_flush — full big flush sequence on one pentagon
 * Runs all rings 5→4→3→2→1→seal in sequence.
 * Permanent delete: data that passes through cannot be recovered.
 *
 * Analogy: like draining a swimming pool ring by ring from the walls
 * inward — water near the drain exits last, most confirmed.
 *
 * @param gs        GoldbergShutter context
 * @param block     FrustumBlock to commit drain state
 * @param penta_id  pentagon to flush (0..11)
 * @return          number of rings flushed (0..5), negative = error
 */
static inline int8_t gs_big_flush(GoldbergShutter *gs,
                                   FrustumBlock    *block,
                                   uint8_t          penta_id)
{
    GsPentagonShutter *ps = &gs->penta[penta_id % GS_PENTAGON_COUNT];
    if (ps->sealed) return -1;

    /* reset flush_ring to start from ring 5 */
    ps->flush_ring = GS_RING_MAX;

    int8_t rings_done = 0;
    GsFlushResult fr;

    /* flush 5→4→3→2→1→0(seal) */
    do {
        fr = gs_flush_ring(gs, block, penta_id);
        if (fr == GS_FLUSH_OK || fr == GS_FLUSH_CONTINUE || fr == GS_FLUSH_SKIP)
            rings_done++;
    } while (fr == GS_FLUSH_CONTINUE && rings_done <= (int8_t)(GS_RING_MAX + 1u));

    return rings_done;
}

/*
 * gs_big_flush_all — big flush all 12 pentagons
 * Called after Atomic Reshape confirms system is clean.
 * Returns count of successfully sealed pentagons.
 */
static inline uint8_t gs_big_flush_all(GoldbergShutter *gs,
                                        FrustumBlock    *block)
{
    uint8_t sealed = 0u;
    for (uint8_t p = 0u; p < GS_PENTAGON_COUNT; p++) {
        int8_t r = gs_big_flush(gs, block, p);
        if (r > 0) sealed++;
    }
    return sealed;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  HILBERT ADDRESS — stored, not wired (future scale-out)
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * gs_trap_set_hilbert — store Hilbert address in trapezoid
 *
 * Address encoding (future spec, placeholder):
 *   hilbert_addr = f(penta_id, trap_id, ring, trit)
 *   hilbert_pair = address of matching trapezoid on target dodeca
 *
 * Currently: stored but not broadcast.
 * Future: wireless handshake when scale-out implemented.
 */
static inline void gs_trap_set_hilbert(GsTrapezoid *t,
                                        uint32_t     addr,
                                        uint32_t     pair)
{
    t->hilbert_addr = addr;
    t->hilbert_pair = pair;
    t->state |= TRAP_BIT_HILBERT;
}

/*
 * gs_trap_hilbert_ready — true if Hilbert address computed and stored
 * Future: also check pair response received
 */
static inline bool gs_trap_hilbert_ready(const GsTrapezoid *t)
{
    return (bool)(t->state & TRAP_BIT_HILBERT);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SPAWN HOOK — stub only (future dodecahedron scale-out)
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * gs_pentagon_spawn — called after pentagon sealed + Hilbert ready
 * Currently a stub. Future: spawn new dodecahedron origin at this face.
 *
 * When implemented:
 *   1. broadcast hilbert_addr from all 5 trapezoids
 *   2. wait for hilbert_pair response from target
 *   3. wire icosa fan to new dodeca origin
 *   4. reset pentagon shutter for next cycle
 */
static inline bool gs_pentagon_spawn(GoldbergShutter *gs, uint8_t penta_id)
{
    GsPentagonShutter *ps = &gs->penta[penta_id % GS_PENTAGON_COUNT];
    if (!ps->sealed) return false;
    /* TODO: implement wireless Hilbert handshake + dodeca spawn */
    (void)gs;
    return true;   /* stub: always "succeeds" */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  INSPECT HELPERS
 * ═══════════════════════════════════════════════════════════════════════ */

/* count hex at a given ring distance */
static inline uint8_t gs_count_at_ring(const GoldbergShutter *gs, uint8_t ring)
{
    uint8_t n = 0u;
    for (uint8_t h = 0u; h < FGLS_DIAMOND_COUNT; h++)
        if (gs->hex[h].ring == ring) n++;
    return n;
}

/* is full sphere flush complete? (all 12 pentagons sealed) */
static inline bool gs_sphere_sealed(const GoldbergShutter *gs)
{
    return gs->sealed_count >= GS_PENTAGON_COUNT;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  GEOMETRY PROOF REFERENCE (compile-time assertions)
 * ═══════════════════════════════════════════════════════════════════════ */

_Static_assert(GS_PENTAGON_TRAP * GS_PENTAGON_COUNT == GS_TRAP_TOTAL,
               "5 trap × 12 penta = 60");
_Static_assert(GS_ICOSA_TRI * GS_ICOSA_TRIT == GS_NODE_MAX,
               "6 tri × 27 = 162 = NODE_MAX");
_Static_assert(GS_HEX_TRI * 3u == GS_HEX_SHUTTER,
               "6 × 3 = 18 = GEAR family");

#endif /* GOLDBERG_SHUTTER_H */

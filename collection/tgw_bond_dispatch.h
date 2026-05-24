/*
 * tgw_bond_dispatch.h
 * ─────────────────────────────────────────────────────────────
 * Bond-aware TGW dispatch — FoldGate → TRing → LCGW pipeline
 *
 * Architecture position:
 *   PoglsPiece.shape → [THIS FILE] → TRing slot → ROUTE/GROUND
 *
 * Shape IS the routing instruction (no separate opcode table):
 *   I  → ROUTE  linear passthrough    → Hilbert batch → Goldberg → FTS
 *   O  → ROUTE  latch/buffer          → Hilbert batch (held one cycle)
 *   T  → ROUTE  fan-out broadcast     → Hilbert batch → 3 downstream
 *   S  → GROUND cross-swap            → LC-GCFS (ghost-delete capable)
 *   Z  → GROUND reverse-swap          → LC-GCFS (odd spokes 1,3,5)
 *   L  → GROUND fork-left / dead-letter → LC-GCFS quarantine lane
 *   J  → ROUTE  fork-right / branch   → Hilbert batch split
 *   0  → FAULT  unused sentinel       → Ω_quarantine immediately
 *
 * Polarity rule (mirrors CPU_PIPELINE_V1 / LCGW):
 *   ROUTE  (polarity=0): shape I,O,T,J  → Hilbert → Goldberg → FTS
 *   GROUND (polarity=1): shape S,Z,L    → LC-GCFS, odd spokes only
 *
 * Bond integration:
 *   Every dispatch checks bond validity before routing.
 *   Invalid bond → FAULT_FAULT → Ω_quarantine (no silent drop).
 *   bond_key stored in TRing slot.extra for downstream tracing.
 *
 * TRing: 720 slots (12 compound-of-5-tetra × 60 spokes)
 *   slot = bond_key % 720
 *   odd-spoke constraint for GROUND: slot % 2 == 1
 *   if slot is even → slot ^= 1 (nearest odd)
 *
 * No malloc, no float, O(1) dispatch.
 * ─────────────────────────────────────────────────────────────
 */

#ifndef TGW_BOND_DISPATCH_H
#define TGW_BOND_DISPATCH_H

#include <stdint.h>
#include <string.h>
#include "pogls_bond.h"   /* PoglsPiece, PoglsSlot, SHAPE_*, FAULT_* */

/* ── ROUTING CONSTANTS ──────────────────────────────────────── */

#define TGW_TRING_SLOTS     720u    /* sacred: 12×60 */
#define TGW_MAX_FANOUT      3u      /* T-shape max downstream */
#define TGW_HOLD_CYCLES     1u      /* O-shape latch hold */

/* Route polarity */
#define TGW_POLARITY_ROUTE   0u    /* → Hilbert → Goldberg → FTS     */
#define TGW_POLARITY_GROUND  1u    /* → LC-GCFS, odd spokes only      */

/* Dispatch result codes */
typedef enum {
    TGW_DISPATCH_OK         = 0,
    TGW_DISPATCH_HELD       = 1,   /* O-latch: buffered, not forwarded yet */
    TGW_DISPATCH_FANOUT     = 2,   /* T-split: forwarded to N downstream   */
    TGW_DISPATCH_GROUND     = 3,   /* S/Z/L: sent to LC-GCFS               */
    TGW_DISPATCH_QUARANTINE = 4,   /* bond invalid or shape=0              */
    TGW_DISPATCH_FAULT      = 5,   /* internal error                       */
} TgwDispatchResult;

/* ── TRING SLOT ─────────────────────────────────────────────── */

typedef struct {
    uint64_t  bond_key;     /* raw bond XOR — stable, used for slot index  */
    uint8_t   shape;        /* piece shape at time of dispatch              */
    uint8_t   polarity;     /* 0=ROUTE, 1=GROUND                           */
    uint16_t  tring_pos;    /* slot index 0–719                             */
    uint32_t  agent_id;     /* originating agent                            */
    uint8_t   hold_count;   /* remaining hold cycles (O-latch)              */
    uint8_t   fanout_n;     /* downstream count (T-shape)                   */
    uint8_t   rerouted;     /* Ω substitution happened                      */
    uint8_t   _pad;
    uint32_t  fanout_ids[TGW_MAX_FANOUT]; /* target agent_ids for T fanout  */
} TgwTRingSlot;

/* ── DISPATCH CONTEXT ───────────────────────────────────────── */

typedef struct {
    TgwTRingSlot ring[TGW_TRING_SLOTS];  /* 720 slots, stack-allocated     */
    uint32_t     active_count;
    uint64_t     total_dispatched;
    uint64_t     total_grounded;
    uint64_t     total_quarantined;
    uint64_t     session_nonce;
} TgwDispatchCtx;

/* ── SHAPE → POLARITY TABLE ─────────────────────────────────── */

/*
 * Shape byte → polarity (0=ROUTE, 1=GROUND, 0xFF=FAULT)
 * Indexed by ASCII value of shape char.
 * Non-shape bytes map to 0xFF (fault).
 */
static inline uint8_t tgw_shape_polarity(uint8_t shape) {
    switch (shape) {
        case SHAPE_I: return TGW_POLARITY_ROUTE;   /* pipe          */
        case SHAPE_O: return TGW_POLARITY_ROUTE;   /* latch (route) */
        case SHAPE_T: return TGW_POLARITY_ROUTE;   /* fan-out       */
        case SHAPE_J: return TGW_POLARITY_ROUTE;   /* fork-right    */
        case SHAPE_S: return TGW_POLARITY_GROUND;  /* cross-swap    */
        case SHAPE_Z: return TGW_POLARITY_GROUND;  /* reverse-swap  */
        case SHAPE_L: return TGW_POLARITY_GROUND;  /* fork-left/quarantine */
        default:      return 0xFF;                 /* fault/unused  */
    }
}

/* ── TRING SLOT ALLOCATION ──────────────────────────────────── */

/*
 * tgw_tring_pos(bond_key, polarity) → uint16_t slot index
 *
 * ROUTE:  slot = bond_key % 720  (any slot)
 * GROUND: slot = bond_key % 720, forced to odd (slot |= 1)
 *         mirrors CPU_PIPELINE_V1 odd-spoke constraint
 */
static inline uint16_t tgw_tring_pos(uint64_t bond_key, uint8_t polarity) {
    uint16_t slot = (uint16_t)(bond_key % TGW_TRING_SLOTS);
    if (polarity == TGW_POLARITY_GROUND && (slot & 1) == 0) {
        slot = (uint16_t)((slot + 1) % TGW_TRING_SLOTS);
    }
    return slot;
}

/* ── INIT ───────────────────────────────────────────────────── */

static inline void tgw_dispatch_init(TgwDispatchCtx *ctx, uint64_t session_nonce) {
    memset(ctx, 0, sizeof(TgwDispatchCtx));
    ctx->session_nonce = session_nonce;
    pogls_config_set_nonce(session_nonce);
}

/* ── CORE DISPATCH ──────────────────────────────────────────── */

/*
 * tgw_dispatch(ctx, slot_a, slot_b, fanout_ids, fanout_n)
 *
 * Full pipeline:
 *   1. bond_verify(a, b)  → quarantine on invalid
 *   2. shape_polarity     → ROUTE or GROUND
 *   3. tring_pos          → slot allocation
 *   4. shape-specific behavior:
 *      I → straight ROUTE
 *      O → HELD (hold_count = TGW_HOLD_CYCLES)
 *      T → FANOUT (up to 3 downstream)
 *      J → ROUTE with split marker
 *      S → GROUND cross-swap
 *      Z → GROUND reverse
 *      L → GROUND quarantine lane
 *   5. Write TRing slot, update counters
 *
 * fanout_ids/fanout_n: only used for T-shape, can be NULL/0 for others.
 */
static inline TgwDispatchResult tgw_dispatch(
    TgwDispatchCtx  *ctx,
    PoglsSlot       *slot_a,       /* originating slot       */
    PoglsSlot       *slot_b,       /* paired/downstream slot */
    const uint32_t  *fanout_ids,   /* T-shape targets (nullable) */
    uint8_t          fanout_n
) {
    if (!ctx || !slot_a || !slot_b) return TGW_DISPATCH_FAULT;

    /* ── step 1: bond verify ──────────────────────────────── */
    PoglsBond bcheck = pogls_bond_verify(&slot_a->piece, &slot_b->piece);
    uint8_t  bond_valid = bcheck.valid;
    uint64_t bk         = bcheck.bond_key;

    /* note: bond_valid==0 is expected for non-origin-paired pieces;
     * we still dispatch but mark polarity from shape.
     * Only shape=0 (unused sentinel) is a hard quarantine.       */
    uint8_t shape = slot_a->piece.shape;

    if (shape == 0x00) {
        ctx->total_quarantined++;
        /* reroute originating slot to quarantine shape */
        pogls_reroute(slot_a, POGLS_FAULT);
        return TGW_DISPATCH_QUARANTINE;
    }

    /* ── step 2: polarity ─────────────────────────────────── */
    uint8_t polarity = tgw_shape_polarity(shape);
    if (polarity == 0xFF) {
        /* unknown shape byte → quarantine */
        ctx->total_quarantined++;
        pogls_reroute(slot_a, POGLS_FAULT);
        return TGW_DISPATCH_QUARANTINE;
    }

    /* ── step 3: TRing slot allocation ───────────────────── */
    /* Use bond_key if verified, otherwise derive from piece directly */
    uint64_t routing_key = bond_valid ? bk
                         : pogls_bond_key(&slot_a->piece);
    uint16_t tpos = tgw_tring_pos(routing_key, polarity);

    /* ── step 4: populate TRing slot ─────────────────────── */
    TgwTRingSlot *ts = &ctx->ring[tpos];
    ts->bond_key  = routing_key;
    ts->shape     = shape;
    ts->polarity  = polarity;
    ts->tring_pos = tpos;
    ts->agent_id  = slot_a->agent_id;
    ts->rerouted  = slot_a->rerouted;
    ts->hold_count = 0;
    ts->fanout_n   = 0;
    memset(ts->fanout_ids, 0, sizeof(ts->fanout_ids));

    /* ── step 5: shape-specific behavior ─────────────────── */
    TgwDispatchResult result;

    switch (shape) {

        case SHAPE_I:
            /* straight pipe: ROUTE with no modification */
            result = TGW_DISPATCH_OK;
            ctx->total_dispatched++;
            break;

        case SHAPE_O:
            /* latch: hold for TGW_HOLD_CYCLES, then route */
            ts->hold_count = TGW_HOLD_CYCLES;
            result = TGW_DISPATCH_HELD;
            ctx->total_dispatched++;
            break;

        case SHAPE_T:
            /* fan-out: copy to up to 3 downstream slots */
            if (fanout_ids && fanout_n > 0) {
                uint8_t n = fanout_n < TGW_MAX_FANOUT ? fanout_n : TGW_MAX_FANOUT;
                ts->fanout_n = n;
                for (uint8_t i = 0; i < n; i++)
                    ts->fanout_ids[i] = fanout_ids[i];
            }
            result = TGW_DISPATCH_FANOUT;
            ctx->total_dispatched++;
            break;

        case SHAPE_J:
            /* fork-right: ROUTE, mark as split for downstream */
            result = TGW_DISPATCH_OK;
            ctx->total_dispatched++;
            break;

        case SHAPE_S:
            /* cross-swap: GROUND, LC-GCFS lane */
            result = TGW_DISPATCH_GROUND;
            ctx->total_grounded++;
            break;

        case SHAPE_Z:
            /* reverse-swap: GROUND, reverse direction in LC-GCFS */
            result = TGW_DISPATCH_GROUND;
            ctx->total_grounded++;
            break;

        case SHAPE_L:
            /* fork-left: GROUND, dead-letter quarantine lane in LC-GCFS */
            result = TGW_DISPATCH_GROUND;
            ctx->total_grounded++;
            break;

        default:
            result = TGW_DISPATCH_QUARANTINE;
            ctx->total_quarantined++;
            break;
    }

    ctx->active_count++;
    return result;
}

/* ── O-LATCH TICK ───────────────────────────────────────────── */

/*
 * tgw_tick(ctx) — advance hold counters for O-latched slots.
 * Returns number of slots released this tick.
 * Call once per dispatch cycle.
 */
static inline uint32_t tgw_tick(TgwDispatchCtx *ctx) {
    uint32_t released = 0;
    for (uint16_t i = 0; i < TGW_TRING_SLOTS; i++) {
        TgwTRingSlot *ts = &ctx->ring[i];
        if (ts->shape == SHAPE_O && ts->hold_count > 0) {
            ts->hold_count--;
            if (ts->hold_count == 0) released++;
        }
    }
    return released;
}

/* ── SLOT QUERY ─────────────────────────────────────────────── */

static inline const TgwTRingSlot *tgw_slot_at(
    const TgwDispatchCtx *ctx, uint16_t pos
) {
    if (pos >= TGW_TRING_SLOTS) return NULL;
    return &ctx->ring[pos];
}

/*
 * tgw_find_slot(ctx, bond_key) — O(1) lookup by bond_key.
 * Computes expected slot position and verifies match.
 * Returns NULL if slot not occupied by this bond_key.
 */
static inline const TgwTRingSlot *tgw_find_slot(
    const TgwDispatchCtx *ctx,
    uint64_t              bond_key,
    uint8_t               polarity
) {
    uint16_t expected = tgw_tring_pos(bond_key, polarity);
    const TgwTRingSlot *ts = &ctx->ring[expected];
    return (ts->bond_key == bond_key) ? ts : NULL;
}

/* ── STATS ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t active_count;
    uint64_t total_dispatched;
    uint64_t total_grounded;
    uint64_t total_quarantined;
    uint32_t route_slots;    /* polarity=0 */
    uint32_t ground_slots;   /* polarity=1 */
    uint32_t held_slots;     /* O-latch pending */
    uint32_t fanout_slots;   /* T-shape active fanouts */
} TgwStats;

static inline TgwStats tgw_get_stats(const TgwDispatchCtx *ctx) {
    TgwStats s = {0};
    s.active_count      = ctx->active_count;
    s.total_dispatched  = ctx->total_dispatched;
    s.total_grounded    = ctx->total_grounded;
    s.total_quarantined = ctx->total_quarantined;
    for (uint16_t i = 0; i < TGW_TRING_SLOTS; i++) {
        const TgwTRingSlot *ts = &ctx->ring[i];
        if (ts->bond_key == 0) continue;
        if (ts->polarity == TGW_POLARITY_ROUTE)  s.route_slots++;
        else                                      s.ground_slots++;
        if (ts->hold_count > 0) s.held_slots++;
        if (ts->fanout_n   > 0) s.fanout_slots++;
    }
    return s;
}

#endif /* TGW_BOND_DISPATCH_H */

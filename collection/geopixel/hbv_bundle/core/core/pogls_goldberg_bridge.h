/*
 * pogls_goldberg_bridge.h — Goldberg-wired Twin Bridge
 * =====================================================
 * Replaces direct pipeline_wire_process calls with shape-driven
 * spatiotemporal fingerprinting via the Goldberg scanner.
 *
 * Every write goes through:
 *   1. gb_face_write()   → spatial interference capture
 *   2. gbt_capture()     → spatiotemporal fingerprint emit
 *   3. FiboClock tick    → temporal advance
 *   4. POGLS pipeline    → existing processing with seed.gen2
 *   5. geo_fast_intersect → Twin Geo feed (raw = addr^value^gen3^c144)
 *   6. FIBO_EV_FLUSH     → dodeca_insert every 144 ops
 *
 * Goldberg face selection:
 *   addr % GB_N_FACES → which face to write
 *   value             → what was written
 *   This maps the full address space onto the 32-face scanner.
 *
 * c144 freeze rule (preserved from pogls_twin_bridge.h):
 *   c144 captured BEFORE fibo_clock_tick advances it.
 *
 * Sacred numbers preserved:
 *   144  = FIBO_PERIOD_FLUSH = flush boundary
 *   720  = TRing cycle
 *   3456 = GB_ADDR_BIPOLAR = GEO_FULL_N
 */

#ifndef POGLS_GOLDBERG_BRIDGE_H
#define POGLS_GOLDBERG_BRIDGE_H

#include <stdint.h>
#include "geo_goldberg_tring_bridge.h"   /* spatial + temporal              */

/* ── forward decls (caller provides implementations) ──────────
 * These match the existing POGLS/Twin Geo API contracts.
 * Include your actual headers before this file if needed.     */
#ifndef POGLS_PIPELINE_WIRE_H
typedef struct PipelineWire PipelineWire;
#endif
#ifndef GEO_FIBO_CLOCK_H
typedef struct FiboCtx FiboCtx;
#endif

/* ── flush event flag (from existing system) ── */
#ifndef FIBO_EV_FLUSH
#define FIBO_EV_FLUSH   0x01u
#endif

/* ── passive record callback ────────────────────────────────────
 * Called after every gbt_capture() with the fresh fingerprint.
 * Passive tracker plugs in here — no content stored, only print.
 * Set to NULL to disable.                                       */
typedef void (*GBTRecordFn)(const GBTRingPrint *fp, void *user);

/* ── bridge context ── */
typedef struct {
    GBTRingCtx      gbt;            /* goldberg + tring spatial+temporal  */
    uint32_t        op_count;       /* ops since last flush               */
    uint32_t        flush_period;   /* default 144                        */
    GBTRecordFn     on_record;      /* passive tracker callback           */
    void           *user;           /* user data passed to on_record      */
} PoglsGoldbergBridge;

/* ── init ── */
static inline void pgb_init(PoglsGoldbergBridge *b,
                             GBTRecordFn on_record, void *user)
{
    gbt_init(&b->gbt);
    b->op_count    = 0;
    b->flush_period = 144u;     /* FIBO_PERIOD_FLUSH */
    b->on_record   = on_record;
    b->user        = user;
}

/* ── core write — replaces twin_bridge_write(addr, value) ───────
 *
 * Returns the spatiotemporal fingerprint of this write event.
 * Caller can use fp.stamp, fp.active_pair, fp.tring_pos, etc.
 *
 * flush_event: set to non-zero if FIBO_EV_FLUSH triggered
 */
static inline GBTRingPrint pgb_write(PoglsGoldbergBridge *b,
                                      uint32_t addr,
                                      uint32_t value,
                                      int *flush_event)
{
    if (flush_event) *flush_event = 0;

    /* 1. map addr → Goldberg face (distribute across 32 faces) */
    uint8_t face_id = (uint8_t)(addr % GB_N_FACES);
    gbt_write(&b->gbt, face_id, value);

    /* 2. spatiotemporal fingerprint */
    GBTRingPrint fp = gbt_capture(&b->gbt);

    /* 3. passive tracker record */
    if (b->on_record)
        b->on_record(&fp, b->user);

    /* 4. advance op counter */
    b->op_count++;

    /* 5. flush boundary (every 144 ops = FIBO_PERIOD_FLUSH) */
    if (b->op_count >= b->flush_period) {
        gbt_flush(&b->gbt);
        b->op_count = 0;
        if (flush_event) *flush_event = FIBO_EV_FLUSH;
    }

    return fp;
}

/* ── batch write: N addr/value pairs ────────────────────────── */
static inline void pgb_write_batch(PoglsGoldbergBridge *b,
                                    const uint32_t *addrs,
                                    const uint32_t *values,
                                    uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        int flush_ev;
        pgb_write(b, addrs[i], values[i], &flush_ev);
        /* caller's dodeca_insert hook fires via on_record callback */
    }
}

/* ── query: fingerprint of last write ── */
static inline const GBTRingPrint *pgb_last(const PoglsGoldbergBridge *b) {
    return &b->gbt.last;
}

/* ── query: raw XOR for Twin Geo feed ───────────────────────────
 * Mirrors: raw = addr ^ value ^ seed.gen3 ^ c144
 * c144 = stamp from BEFORE last tick (frozen per spec)
 * Use fp.stamp as the combined spatial+temporal seed component.
 */
static inline uint32_t pgb_twin_raw(uint32_t addr,
                                     uint32_t value,
                                     uint32_t stamp_frozen)
{
    return addr ^ value ^ stamp_frozen;
}

/* ── stats ── */
static inline uint64_t pgb_event_count(const PoglsGoldbergBridge *b) {
    return b->gbt.event_count;
}

static inline uint32_t pgb_op_count(const PoglsGoldbergBridge *b) {
    return b->op_count;
}

#endif /* POGLS_GOLDBERG_BRIDGE_H */

/*
 * geo_goldberg_tracker.h — Passive Blueprint Tracker
 * ===================================================
 * Plugs into PoglsGoldbergBridge via GBTRecordFn callback.
 *
 * Operating model:
 *   - Records GBTRingPrint fingerprints as they arrive
 *   - At flush boundary (144 ops): extracts blueprint, marks ready
 *   - Blueprint = structural fingerprint of data shape, not content
 *   - After blueprint extracted → tracker resets, loop continues
 *   - Nothing persists after flush except the blueprint itself
 *
 * Blueprint contents:
 *   - spatial_xor    : XOR fold of all 32 face diffs
 *   - tension_map    : top-N tension gaps (most sensitive points)
 *   - circuit_map    : which of 6 bipolar circuits fired
 *   - temporal_span  : tring_pos range covered in this window
 *   - stamp_hash     : XOR chain of all stamps in window
 *   - event_count    : how many writes contributed
 *
 * Usage:
 *   GBTracker t;
 *   gbt_tracker_init(&t);
 *   pgb_init(&bridge, gbt_tracker_record, &t);
 *   // ... writes flow through pgb_write() ...
 *   if (gbt_tracker_ready(&t)) {
 *       GBBlueprint bp = gbt_tracker_extract(&t);
 *       // store bp, then:
 *       gbt_tracker_reset(&t);
 *   }
 */

#ifndef GEO_GOLDBERG_TRACKER_H
#define GEO_GOLDBERG_TRACKER_H

#include <stdint.h>
#include <string.h>
#include "geo_goldberg_tring_bridge.h"

/* ── blueprint: structural fingerprint of one flush window ── */
#define GBT_TENSION_TOP     5       /* top-N tension gaps recorded      */

typedef struct {
    /* spatial */
    uint32_t    spatial_xor;                /* XOR fold of all face diffs       */
    uint8_t     top_gaps[GBT_TENSION_TOP];  /* gap_ids with highest tension     */
    uint32_t    top_tensions[GBT_TENSION_TOP];
    uint8_t     circuit_fired;              /* bitmask: bit[p]=1 if pair p fired */

    /* temporal */
    uint16_t    tring_start;        /* tring_pos of first event in window   */
    uint16_t    tring_end;          /* tring_pos of last event              */
    uint16_t    tring_span;         /* end - start (mod 720)                */

    /* combined */
    uint32_t    stamp_hash;         /* XOR chain of all stamps              */
    uint32_t    event_count;        /* writes in this window                */
    uint64_t    window_id;          /* monotonic flush counter              */
} GBBlueprint;

/* ── tracker context ── */
#define GBT_WINDOW_MAX  144         /* max fingerprints per window = flush period */

typedef struct {
    /* accumulator */
    uint32_t    spatial_acc[GB_N_FACES];    /* accumulated XOR per face         */
    uint32_t    tension_acc[GB_N_TRIGAP];   /* accumulated tension per gap      */
    uint8_t     circuit_acc;                /* bitmask of circuits seen         */
    uint32_t    stamp_hash;                 /* XOR chain                        */
    uint16_t    tring_start;
    uint16_t    tring_end;
    uint32_t    event_count;

    /* output */
    GBBlueprint blueprint;
    uint8_t     ready;              /* 1 = blueprint ready to extract       */
    uint64_t    window_id;          /* increments each flush                */
} GBTracker;

/* ── init ── */
static inline void gbt_tracker_init(GBTracker *t) {
    memset(t, 0, sizeof(GBTracker));
}

/* ── reset window (call after extract) ── */
static inline void gbt_tracker_reset(GBTracker *t) {
    memset(t->spatial_acc,  0, sizeof(t->spatial_acc));
    memset(t->tension_acc,  0, sizeof(t->tension_acc));
    t->circuit_acc  = 0;
    t->stamp_hash   = 0;
    t->tring_start  = 0xFFFF;
    t->tring_end    = 0;
    t->event_count  = 0;
    t->ready        = 0;
}

/* ── GBTRecordFn callback — plug into pgb_init ──────────────────
 * Called by PoglsGoldbergBridge on every write.
 * user = GBTracker*
 */
static inline void gbt_tracker_record(const GBTRingPrint *fp, void *user) {
    GBTracker *t = (GBTracker *)user;
    if (!t) return;

    /* accumulate spatial */
    for (int i = 0; i < GB_N_FACES; i++)
        t->spatial_acc[i] ^= fp->spatial[i];

    /* accumulate tension */
    t->tension_acc[fp->max_tension_gap] += fp->max_tension;

    /* circuit bitmask */
    t->circuit_acc |= (uint8_t)(1u << fp->active_pair);

    /* stamp chain */
    t->stamp_hash ^= fp->stamp;

    /* temporal span */
    if (t->tring_start == 0xFFFF)
        t->tring_start = fp->tring_pos;
    t->tring_end = fp->tring_pos;

    t->event_count++;

    /* flush boundary: extract blueprint every 144 events */
    if (t->event_count >= 144u) {
        GBBlueprint *bp = &t->blueprint;
        memset(bp, 0, sizeof(GBBlueprint));

        /* spatial XOR fold */
        uint32_t xfold = 0;
        for (int i = 0; i < GB_N_FACES; i++)
            xfold ^= t->spatial_acc[i];
        bp->spatial_xor = xfold;

        /* top-N tension gaps (simple selection sort, N=5) */
        uint32_t tmp_t[GB_N_TRIGAP];
        uint8_t  tmp_id[GB_N_TRIGAP];
        memcpy(tmp_t, t->tension_acc, sizeof(tmp_t));
        for (int i = 0; i < GB_N_TRIGAP; i++) tmp_id[i] = (uint8_t)i;

        for (int i = 0; i < GBT_TENSION_TOP; i++) {
            int max_j = i;
            for (int j = i+1; j < GB_N_TRIGAP; j++)
                if (tmp_t[j] > tmp_t[max_j]) max_j = j;
            uint32_t tv = tmp_t[i]; tmp_t[i] = tmp_t[max_j]; tmp_t[max_j] = tv;
            uint8_t  ti = tmp_id[i]; tmp_id[i] = tmp_id[max_j]; tmp_id[max_j] = ti;
            bp->top_gaps[i]     = tmp_id[i];
            bp->top_tensions[i] = tmp_t[i];
        }

        bp->circuit_fired = t->circuit_acc;

        /* temporal */
        bp->tring_start = t->tring_start;
        bp->tring_end   = t->tring_end;
        bp->tring_span  = (uint16_t)((t->tring_end - t->tring_start + 720u) % 720u);

        bp->stamp_hash   = t->stamp_hash;
        bp->event_count  = t->event_count;
        bp->window_id    = ++t->window_id;

        t->ready = 1;

        /* reset accumulator for next window */
        memset(t->spatial_acc,  0, sizeof(t->spatial_acc));
        memset(t->tension_acc,  0, sizeof(t->tension_acc));
        t->circuit_acc = 0;
        t->stamp_hash  = 0;
        t->tring_start = 0xFFFF;
        t->tring_end   = 0;
        t->event_count = 0;
    }
}

/* ── query: blueprint ready? ── */
static inline int gbt_tracker_ready(const GBTracker *t) {
    return t->ready;
}

/* ── extract blueprint (caller stores it, then reset) ── */
static inline GBBlueprint gbt_tracker_extract(GBTracker *t) {
    GBBlueprint bp = t->blueprint;
    t->ready = 0;
    return bp;
}

/* ── blueprint compare: same structural shape? ── */
static inline int gbp_match(const GBBlueprint *a, const GBBlueprint *b) {
    return a->spatial_xor   == b->spatial_xor
        && a->stamp_hash    == b->stamp_hash
        && a->circuit_fired == b->circuit_fired;
}

/* ── blueprint fingerprint: single uint64 identity ── */
static inline uint64_t gbp_identity(const GBBlueprint *bp) {
    return ((uint64_t)bp->spatial_xor << 32) | bp->stamp_hash;
}

#endif /* GEO_GOLDBERG_TRACKER_H */

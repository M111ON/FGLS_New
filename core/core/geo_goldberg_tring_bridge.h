/*
 * geo_goldberg_tring_bridge.h — Spatiotemporal Fingerprint Bridge
 * ===============================================================
 * Combines:
 *   GBScanCtx  (geo_goldberg_scan.h)  — SPATIAL:   how data bent the net
 *   TRingCtx   (geo_temporal_ring.h)  — TEMPORAL:  where in the clock cycle
 *
 * Output: GBTRingPrint — spatiotemporal fingerprint of one data event
 *
 * Operating model:
 *   1. Data warps into Goldberg shell → gb_face_write() per face
 *   2. gb_scan()     → spatial fingerprint (32 diffs + 60 tensions)
 *   3. tring_tick()  → advance temporal position
 *   4. gbt_capture() → bind spatial + temporal into one fingerprint
 *   5. Flush both → ready for next event
 *
 * Key insight:
 *   spatial  = WHAT shape warped the net (XOR interference pattern)
 *   temporal = WHEN it happened (walk position 0..719 on TRing)
 *   combined = unique identity of data event without storing content
 *
 * Bipolar routing:
 *   gb_active_pair() → which of 6 circuits fired most
 *   TRING_CPAIR(enc) → chiral partner on opposite pentagon
 *   together: route from + pole to - pole at correct temporal position
 */

#ifndef GEO_GOLDBERG_TRING_BRIDGE_H
#define GEO_GOLDBERG_TRING_BRIDGE_H

#include <stdint.h>
#include <string.h>
#include "geo_goldberg_scan.h"
#include "geo_temporal_ring.h"

/* ── spatiotemporal fingerprint ── */
typedef struct {
    /* spatial */
    uint32_t    spatial[GB_N_FACES];    /* fingerprint[32] from gb_scan     */
    uint32_t    max_tension;            /* highest tetra node tension       */
    uint8_t     max_tension_gap;        /* gap_id with max tension          */
    uint8_t     active_pair;            /* bipolar pair that fired most     */
    uint8_t     active_pole;            /* GB_POLE_POSITIVE or NEGATIVE     */

    /* temporal */
    uint16_t    tring_pos;              /* walk position 0..719             */
    uint32_t    tring_enc;              /* tuple encoding at this position  */
    uint16_t    tring_pair_pos;         /* chiral partner walk position     */

    /* combined */
    uint32_t    stamp;                  /* spatial XOR folded ^ tring_pos   */
    uint64_t    scan_id;                /* monotonic scan counter           */
} GBTRingPrint;

/* ── bridge context ── */
typedef struct {
    GBScanCtx   gb;                     /* spatial scanner                  */
    TRingCtx    tr;                     /* temporal ring                    */
    GBTRingPrint last;                  /* most recent fingerprint          */
    uint64_t    event_count;            /* total events captured            */
} GBTRingCtx;

/* ── init ── */
static inline void gbt_init(GBTRingCtx *b) {
    memset(b, 0, sizeof(GBTRingCtx));
    gb_scan_init(&b->gb);
    tring_init(&b->tr);
}

/* ── write data into a specific face ── */
static inline void gbt_write(GBTRingCtx *b, uint8_t face_id, uint32_t value) {
    gb_face_write(&b->gb, face_id, value);
}

/* ── capture: scan spatial + tick temporal → emit fingerprint ── */
static inline GBTRingPrint gbt_capture(GBTRingCtx *b) {
    GBTRingPrint fp;
    memset(&fp, 0, sizeof(GBTRingPrint));

    /* 1. spatial scan */
    gb_scan(&b->gb);
    memcpy(fp.spatial, b->gb.fingerprint, sizeof(fp.spatial));

    /* max tension gap */
    fp.max_tension_gap = gb_max_tension_gap(&b->gb);
    fp.max_tension     = b->gb.gaps[fp.max_tension_gap].tension;

    /* active bipolar pair */
    fp.active_pair = gb_active_pair(&b->gb);
    fp.active_pole = b->gb.faces[b->gb.pairs[fp.active_pair].pen_positive].diff
                   > b->gb.faces[b->gb.pairs[fp.active_pair].pen_negative].diff
                   ? GB_POLE_POSITIVE : GB_POLE_NEGATIVE;

    /* 2. temporal tick */
    uint16_t tpos   = tring_tick(&b->tr);
    uint32_t tenc   = b->tr.slots[tpos].enc;
    fp.tring_pos      = tpos;
    fp.tring_enc      = tenc;
    fp.tring_pair_pos = tring_pair_pos(tenc);

    /* 3. combined stamp: fold spatial fingerprint XOR temporal position */
    uint32_t spatial_fold = 0;
    for (int i = 0; i < GB_N_FACES; i++)
        spatial_fold ^= fp.spatial[i];
    fp.stamp   = spatial_fold ^ (uint32_t)tpos ^ fp.max_tension;
    fp.scan_id = b->gb.scan_count;

    b->last = fp;
    b->event_count++;
    return fp;
}

/* ── flush: reset spatial diffs, keep temporal position ── */
static inline void gbt_flush(GBTRingCtx *b) {
    gb_flush(&b->gb);
    /* TRing head keeps advancing — temporal position never resets */
}

/* ── full reset: both spatial and temporal ── */
static inline void gbt_reset(GBTRingCtx *b) {
    uint64_t ec = b->event_count;
    gbt_init(b);
    b->event_count = ec;
}

/* ── query: is stamp unique from previous? ── */
static inline int gbt_stamp_changed(const GBTRingCtx *b, uint32_t prev_stamp) {
    return b->last.stamp != prev_stamp;
}

/* ── route: given fingerprint, return bipolar routing info ──
 * returns 0 = route via + pole, 1 = route via - pole
 * chiral_pos = temporal position of partner pentagon */
static inline int gbt_route(const GBTRingPrint *fp, uint16_t *chiral_pos) {
    if (chiral_pos) *chiral_pos = fp->tring_pair_pos;
    return (int)fp->active_pole;
}

#endif /* GEO_GOLDBERG_TRING_BRIDGE_H */

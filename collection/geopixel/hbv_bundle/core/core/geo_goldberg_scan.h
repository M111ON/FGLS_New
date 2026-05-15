/*
 * geo_goldberg_scan.h — Goldberg GP(1,1) Spatial Scanner
 * =======================================================
 * Outer shell of FGLS — shape-driven interference detector
 *
 * GP(1,1) topology:
 *   12 pentagons  (fixed — warp gateways, bipolar 6 pairs)
 *   20 hexagons   (buffer/routing faces)
 *   60 tri-gaps   (tetra node connectors = I-symmetry group)
 *   32 frustum rings total (1 per face)
 *
 * Operating principle:
 *   Baseline net calibrated to 0.
 *   Data warps in → each face node deviates from baseline.
 *   XOR diff = interference pattern = spatial fingerprint of data shape.
 *   No content stored — only fingerprint. Flush and loop.
 *
 * Bipolar structure (electrical model):
 *   6 pentagon pairs (opposite) = 6 independent circuits
 *   Pentagon A = positive pole (+)
 *   Pentagon B = negative pole (-) [directly opposite]
 *   GEO_SPOKES = 6 = N_BIPOLAR_PAIRS
 *
 * Address derivation:
 *   12 compounds × 12 simulated = 144 = FIBO_PERIOD_FLUSH
 *   144 × 12 = 1728 (unipolar)
 *   1728 × 2 = 3456 = GEO_FULL_N  ← bipolar, exact
 *
 * Role vs TRing:
 *   geo_goldberg_scan = SPATIAL (where/how shape bent the net)
 *   geo_temporal_ring = TEMPORAL (when in the clock cycle)
 *   Output of both combined = spatiotemporal fingerprint
 */

#ifndef GEO_GOLDBERG_SCAN_H
#define GEO_GOLDBERG_SCAN_H

#include <stdint.h>
#include <string.h>
#include "geo_goldberg_lut.h"

/* ── constants ── */
#define GB_N_PENTAGON       12
#define GB_N_HEXAGON        20
#define GB_N_FACES          32      /* 12 pen + 20 hex              */
#define GB_N_TRIGAP         60      /* triangle gaps = I-sym order  */
#define GB_N_BIPOLAR_PAIRS   6      /* GEO_SPOKES = 6               */
#define GB_ADDR_UNIPOLAR  1728      /* 144 × 12                     */
#define GB_ADDR_BIPOLAR   3456      /* = GEO_FULL_N                 */

/* ── face type ── */
typedef enum {
    GB_FACE_PENTAGON = 0,   /* warp gateway — fixed anchor  */
    GB_FACE_HEXAGON  = 1,   /* buffer / routing face        */
} GBFaceType;

/* ── pole: bipolar assignment ── */
typedef enum {
    GB_POLE_POSITIVE = 0,   /* + live                       */
    GB_POLE_NEGATIVE = 1,   /* - mirror (opposite pentagon) */
    GB_POLE_NEUTRAL  = 2,   /* hexagon — not a pole         */
} GBPole;

/* ── single face descriptor ── */
typedef struct {
    uint8_t     face_id;    /* 0..31                            */
    GBFaceType  type;       /* PENTAGON or HEXAGON              */
    GBPole      pole;       /* POSITIVE / NEGATIVE / NEUTRAL    */
    uint8_t     pair_id;    /* pentagon: 0..5 (bipolar pair)
                               hexagon: 0xFF                    */
    uint32_t    baseline;   /* calibrated zero state            */
    uint32_t    state;      /* current node value               */
    uint32_t    diff;       /* state ^ prev_state (XOR delta)   */
} GBFace;

/* ── tetra gap node (triangle gap between 3 faces) ── */
typedef struct {
    uint8_t     gap_id;     /* 0..59                            */
    uint8_t     face_a;     /* first adjacent face              */
    uint8_t     face_b;     /* second adjacent face             */
    uint8_t     face_c;     /* third adjacent face              */
    uint32_t    tension;    /* combined diff from 3 faces       */
                            /* highest tension = most sensitive */
} GBTetraNode;

/* ── bipolar pair ── */
typedef struct {
    uint8_t     pair_id;        /* 0..5                         */
    uint8_t     pen_positive;   /* face_id of + pentagon        */
    uint8_t     pen_negative;   /* face_id of - pentagon        */
    uint32_t    circuit_diff;   /* XOR of both pole states      */
} GBBipolarPair;

/* ── scanner context ── */
typedef struct {
    GBFace          faces[GB_N_FACES];          /* 32 faces          */
    GBTetraNode     gaps[GB_N_TRIGAP];          /* 60 tetra nodes    */
    GBBipolarPair   pairs[GB_N_BIPOLAR_PAIRS];  /* 6 circuits        */
    uint32_t        fingerprint[GB_N_FACES];    /* spatial snapshot  */
    uint64_t        scan_count;                 /* total scans done  */
    uint8_t         dirty;                      /* 1 = unflushed diff*/
} GBScanCtx;

/* ─────────────────────────────────────────────
 * Static pentagon opposite-pair table (GP(1,1))
 * 12 pentagons = 6 opposite pairs, fixed by geometry
 * pair[i] = {positive_face_id, negative_face_id}
 * ───────────────────────────────────────────── */
/* pentagon pairs: use GB_PEN_PAIRS[p][0/1] from geo_goldberg_lut.h */

/* ── init: calibrate all nodes to baseline = 0 ── */
static inline void gb_scan_init(GBScanCtx *g) {
    memset(g, 0, sizeof(GBScanCtx));

    /* assign face types */
    for (int i = 0; i < GB_N_PENTAGON; i++) {
        g->faces[i].face_id = (uint8_t)i;
        g->faces[i].type    = GB_FACE_PENTAGON;
        g->faces[i].pole    = GB_POLE_NEUTRAL; /* assigned below */
        g->faces[i].pair_id = 0xFF;
    }
    for (int i = GB_N_PENTAGON; i < GB_N_FACES; i++) {
        g->faces[i].face_id = (uint8_t)i;
        g->faces[i].type    = GB_FACE_HEXAGON;
        g->faces[i].pole    = GB_POLE_NEUTRAL;
        g->faces[i].pair_id = 0xFF;
    }

    /* assign bipolar pairs to pentagons */
    for (int p = 0; p < GB_N_BIPOLAR_PAIRS; p++) {
        uint8_t pos = GB_PEN_PAIRS[p][0];
        uint8_t neg = GB_PEN_PAIRS[p][1];
        g->faces[pos].pole    = GB_POLE_POSITIVE;
        g->faces[pos].pair_id = (uint8_t)p;
        g->faces[neg].pole    = GB_POLE_NEGATIVE;
        g->faces[neg].pair_id = (uint8_t)p;
        g->pairs[p].pair_id       = (uint8_t)p;
        g->pairs[p].pen_positive  = pos;
        g->pairs[p].pen_negative  = neg;
    }

    /* gap adjacency from baked LUT */
    for (int i = 0; i < GB_N_TRIGAP; i++) {
        g->gaps[i].gap_id = (uint8_t)i;
        g->gaps[i].face_a = GB_TRIGAP_ADJ[i][0];
        g->gaps[i].face_b = GB_TRIGAP_ADJ[i][1];
        g->gaps[i].face_c = GB_TRIGAP_ADJ[i][2];
    }
}

/* ── write: data warps into face node ── */
static inline void gb_face_write(GBScanCtx *g, uint8_t face_id, uint32_t value) {
    GBFace *f  = &g->faces[face_id];
    f->diff    = f->state ^ value;     /* XOR delta = interference  */
    f->state   = value;
    g->dirty   = 1;
}

/* ── scan: capture diff across all faces ── */
static inline void gb_scan(GBScanCtx *g) {
    /* update fingerprint from current diffs */
    for (int i = 0; i < GB_N_FACES; i++)
        g->fingerprint[i] = g->faces[i].diff;

    /* update tetra node tension (3-face XOR) */
    for (int i = 0; i < GB_N_TRIGAP; i++) {
        GBTetraNode *tn = &g->gaps[i];
        tn->tension = g->faces[tn->face_a].diff
                    ^ g->faces[tn->face_b].diff
                    ^ g->faces[tn->face_c].diff;
    }

    /* update bipolar circuit diff */
    for (int p = 0; p < GB_N_BIPOLAR_PAIRS; p++) {
        GBBipolarPair *bp = &g->pairs[p];
        bp->circuit_diff  = g->faces[bp->pen_positive].state
                          ^ g->faces[bp->pen_negative].state;
    }

    g->scan_count++;
}

/* ── flush: reset all diffs, keep state (baseline = current) ── */
static inline void gb_flush(GBScanCtx *g) {
    for (int i = 0; i < GB_N_FACES; i++) {
        g->faces[i].baseline = g->faces[i].state;
        g->faces[i].diff     = 0;
    }
    for (int i = 0; i < GB_N_TRIGAP; i++)
        g->gaps[i].tension = 0;
    memset(g->fingerprint, 0, sizeof(g->fingerprint));
    g->dirty = 0;
}

/* ── reset: full calibrate back to zero ── */
static inline void gb_reset(GBScanCtx *g) {
    uint64_t sc = g->scan_count;
    gb_scan_init(g);
    g->scan_count = sc;
}

/* ── query: highest tension gap (most sensitive diff point) ── */
static inline uint8_t gb_max_tension_gap(const GBScanCtx *g) {
    uint8_t  max_id  = 0;
    uint32_t max_val = 0;
    for (int i = 0; i < GB_N_TRIGAP; i++) {
        if (g->gaps[i].tension > max_val) {
            max_val = g->gaps[i].tension;
            max_id  = (uint8_t)i;
        }
    }
    return max_id;
}

/* ── query: is pentagon a gateway (non-zero diff)? ── */
static inline int gb_pen_active(const GBScanCtx *g, uint8_t pen_id) {
    if (pen_id >= GB_N_PENTAGON) return 0;
    return g->faces[pen_id].diff != 0;
}

/* ── query: which bipolar pair is most active ── */
static inline uint8_t gb_active_pair(const GBScanCtx *g) {
    uint8_t  max_id  = 0;
    uint32_t max_val = 0;
    for (int p = 0; p < GB_N_BIPOLAR_PAIRS; p++) {
        if (g->pairs[p].circuit_diff > max_val) {
            max_val = g->pairs[p].circuit_diff;
            max_id  = (uint8_t)p;
        }
    }
    return max_id;
}

#endif /* GEO_GOLDBERG_SCAN_H */

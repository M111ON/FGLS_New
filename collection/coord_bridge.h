/*
 * coord_bridge.h — Unified Coordinate Bridge
 * ═══════════════════════════════════════════════════════════════
 *
 * Purpose: Pure functions to convert between Bermuda, TW, and POGLS
 *   coordinate systems. All conversions are lossless where possible.
 *
 * UnifiedCoord is the common intermediate format:
 *   zone   : 0-11 (Bermuda 12-zone, TW 10-sector mapped, POGLS face)
 *   slot   : 0-719 (Bermuda TRing, TW 60-slot mapped, POGLS position)
 *   resid  : residual from nearest centroid (TW carries full x/y)
 *   source : which system produced this coordinate
 *
 * Usage:
 *   #include "coord_bridge.h"
 *   UnifiedCoord u = bermuda_to_coord(bermuda_entry);
 *   TWCaptureInt tw = coord_to_tw(u);
 *   BermudaRouteEntry b = coord_to_bermuda(u);
 *
 * All functions are pure (no side effects, no global state).
 * All functions are O(1) or O(zone_count) = O(12) = O(1).
 *
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef COORD_BRIDGE_H
#define COORD_BRIDGE_H

#include "bermuda_export.h"    /* BermudaRouteEntry, bermuda_* functions */
#include "tw_capture_int.h"    /* TWCaptureInt, tw_* functions           */
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
   UNIFIED COORDINATE TYPE
   ═══════════════════════════════════════════════════════════════ */

typedef enum {
    COORD_SOURCE_BERMUDA = 0,
    COORD_SOURCE_TW      = 1,
    COORD_SOURCE_POGLS   = 2,
    COORD_SOURCE_UNKNOWN = 3
} CoordSource;

typedef struct {
    uint16_t    zone;        /* 0-11 (unified zone)                */
    uint16_t    slot;        /* 0-719 (unified slot)               */
    int32_t     resid_x;     /* residual X (0 for Bermuda/POGLS)   */
    int32_t     resid_y;     /* residual Y (0 for Bermuda/POGLS)   */
    CoordSource source;      /* which system produced this          */
    uint8_t     pole;        /* 0=south, 1=north (Bermuda)         */
    uint8_t     shape;       /* I/O/S/L (Bermuda)                  */
    uint8_t     is_tri;      /* 0=hex, 1=tri (TW combined grid)    */
} UnifiedCoord;

/* ═══════════════════════════════════════════════════════════════
   BERMUDA BRIDGE
   ═══════════════════════════════════════════════════════════════ */

/* Bermuda → UnifiedCoord */
static inline UnifiedCoord bermuda_to_coord(BermudaRouteEntry r) {
    UnifiedCoord u;
    u.zone    = r.zone;                    /* 0-11 */
    u.slot    = r.tring_slot;              /* 0-719 */
    u.resid_x = 0;                        /* no residual in Bermuda */
    u.resid_y = 0;
    u.source  = COORD_SOURCE_BERMUDA;
    u.pole    = r.pole;
    u.shape   = r.shape;
    u.is_tri  = 0;
    return u;
}

/* UnifiedCoord → Bermuda (partial: returns route entry) */
static inline BermudaRouteEntry coord_to_bermuda(UnifiedCoord u) {
    BermudaRouteEntry r;
    r.idx_in    = u.slot;                 /* tring_slot = idx_in */
    r.idx_out   = 0;                     /* must re-traverse */
    r.zone      = (uint8_t)(u.zone % BERMUDA_N_ZONES);
    r.pole      = u.pole;
    r.shape     = u.shape;
    r.polarity  = (u.pole == 0) ? 0 : 1; /* south=ROUTE, north=GROUND */
    r.tring_slot = u.slot;
    return r;
}

/* ═══════════════════════════════════════════════════════════════
   TW BRIDGE
   ═══════════════════════════════════════════════════════════════ */

/* TW → UnifiedCoord */
static inline UnifiedCoord tw_to_coord(TWCaptureInt r) {
    UnifiedCoord u;
    u.zone    = r.zone;                    /* 0-9 */
    u.slot    = r.slot;                    /* 0-59 */
    u.resid_x = (int32_t)r.resid_x;      /* truncate to int32 */
    u.resid_y = (int32_t)r.resid_y;
    u.source  = COORD_SOURCE_TW;
    u.pole    = (r.zone < 5) ? 0 : 1;    /* rough pole mapping */
    u.shape   = 0;                        /* not applicable */
    u.is_tri  = 0;
    return u;
}

/* UnifiedCoord → TW (partial: returns capture with zero drain) */
static inline TWCaptureInt coord_to_tw(UnifiedCoord u) {
    TWCaptureInt c;
    c.zone          = (uint8_t)(u.zone % TW_N_SECTORS);
    c.slot          = (uint8_t)(u.slot % TW_N_SLOTS);
    c.resid_x       = u.resid_x;
    c.resid_y       = u.resid_y;
    c.drain         = 0;
    c.drain_zone    = 0;
    c.drain_slot    = 0;
    c.drain_resid_x = 0;
    c.drain_resid_y = 0;
    return c;
}

/* ═══════════════════════════════════════════════════════════════
   POGLS BRIDGE
   ═══════════════════════════════════════════════════════════════ */

/* POGLS address (simplified from pogls_fold.h CoreSlot) */
typedef struct {
    uint8_t  face_id;      /* 0-31 (5-bit)                        */
    uint8_t  engine_id;    /* 0-127 (7-bit)                       */
    uint32_t vector_pos;   /* A = floor(θ × 2²⁰) per face        */
} PoglsAddr;

/* POGLS → UnifiedCoord */
static inline UnifiedCoord pogls_to_coord(PoglsAddr a) {
    UnifiedCoord u;
    u.zone    = a.face_id;                 /* 0-31 → mapped to zone */
    u.slot    = (uint16_t)(a.vector_pos % (BERMUDA_TRING_SLOTS));
    u.resid_x = 0;
    u.resid_y = 0;
    u.source  = COORD_SOURCE_POGLS;
    u.pole    = (a.face_id < 16) ? 0 : 1; /* rough pole mapping */
    u.shape   = 0;
    u.is_tri  = 0;
    return u;
}

/* UnifiedCoord → POGLS (partial: fills addr from coord) */
static inline PoglsAddr coord_to_pogls(UnifiedCoord u) {
    PoglsAddr a;
    a.face_id   = (uint8_t)(u.zone % FACES_RAW);
    a.engine_id = 0;                      /* must be provided */
    a.vector_pos = u.slot;
    return a;
}

/* ═══════════════════════════════════════════════════════════════
   ZONE MAPPING TABLES
   ═══════════════════════════════════════════════════════════════ */

/*
 * Bermuda 12-zone → TW 10-sector mapping
 * Bermuda zones 0-11 map to TW sectors 0-9
 * Some TW sectors receive 2 Bermuda zones (sector 4,9)
 */
static const uint8_t BERMUDA_TO_TW_ZONE[BERMUDA_N_ZONES] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1
};

/*
 * TW 10-sector → Bermuda 12-zone mapping (reverse)
 * Each TW sector maps to a primary Bermuda zone
 */
static const uint8_t TW_TO_BERMUDA_ZONE[TW_N_SECTORS] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9
};

/* ═══════════════════════════════════════════════════════════════
   CROSS-SYSTEM CONVERSION
   ═══════════════════════════════════════════════════════════════ */

/* Bermuda → TW (lossy: 12 zones → 10 sectors) */
static inline TWCaptureInt bermuda_to_tw(BermudaRouteEntry r) {
    TWCaptureInt c;
    c.zone          = BERMUDA_TO_TW_ZONE[r.zone % BERMUDA_N_ZONES];
    c.slot          = (uint8_t)(r.tring_slot % TW_N_SLOTS);
    c.resid_x       = 0;
    c.resid_y       = 0;
    c.drain         = 0;
    c.drain_zone    = 0;
    c.drain_slot    = 0;
    c.drain_resid_x = 0;
    c.drain_resid_y = 0;
    return c;
}

/* TW → Bermuda (lossy: 10 sectors → 12 zones) */
static inline BermudaRouteEntry tw_to_bermuda(TWCaptureInt c) {
    BermudaRouteEntry r;
    r.idx_in    = c.slot;
    r.idx_out   = 0;
    r.zone      = TW_TO_BERMUDA_ZONE[c.zone % TW_N_SECTORS];
    r.pole      = (c.zone < 5) ? 0 : 1;
    r.shape     = 73;                    /* 'I' default */
    r.polarity  = 0;
    r.tring_slot = c.slot;
    return r;
}

/* ═══════════════════════════════════════════════════════════════
   UTILITY FUNCTIONS
   ═══════════════════════════════════════════════════════════════ */

/* Check if two coordinates are in the same zone */
static inline int coord_same_zone(UnifiedCoord a, UnifiedCoord b) {
    return a.zone == b.zone;
}

/* Check if two coordinates are in the same slot */
static inline int coord_same_slot(UnifiedCoord a, UnifiedCoord b) {
    return a.zone == b.zone && a.slot == b.slot;
}

/* Distance between two coordinates (squared, rough) */
static inline int64_t coord_distance2(UnifiedCoord a, UnifiedCoord b) {
    int64_t dz = (int64_t)a.zone - (int64_t)b.zone;
    int64_t ds = (int64_t)a.slot - (int64_t)b.slot;
    int64_t dx = (int64_t)a.resid_x - (int64_t)b.resid_x;
    int64_t dy = (int64_t)a.resid_y - (int64_t)b.resid_y;
    return dz*dz + ds*ds + dx*dx + dy*dy;
}

/* Print unified coordinate (debug) */
static inline void coord_print(UnifiedCoord u) {
    const char *src[] = {"BERMUDA", "TW", "POGLS", "UNKNOWN"};
    printf("Coord[src=%s zone=%u slot=%u resid=(%d,%d) pole=%u shape=%c is_tri=%u]\n",
           src[u.source], u.zone, u.slot, u.resid_x, u.resid_y,
           u.pole, u.shape ? u.shape : '-', u.is_tri);
}

#ifdef __cplusplus
}
#endif

#endif /* COORD_BRIDGE_H */

/*
 * tri_hex_tess.h — TriHex Tessellation Bond Layer
 *
 * Y-triangle retarget: THCoord is now a single node_id (0..20735).
 * Bond metric = step count on trihex graph (same pentagon=0-2, diff=3)
 * Coverage: 12 pentagons × 1728 nodes = 20736 nodes, zero gaps
 *
 * toggle_level 0 = off (raw bond_discovery)
 * toggle_level 1 = coarse (pentagon-only, 12 nodes)
 * toggle_level 2 = hex (within pentagon)
 * toggle_level 3 = trihex full (within pentagon)
 */

#ifndef TRI_HEX_TESS_H
#define TRI_HEX_TESS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tw_capture_int.h"
#define GEO_JUMP_INLINE
#include "geo_jump.h"

/* ── Constants ──────────────────────────────────────────────── */
#define TH_PENTAGON_NODES  (GEO_FULL / GEO_PENTAGONS)  /* 1728 nodes per pentagon */

/* ── Types ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t node_id;     /* 0..20735 Y-triangle node_id */
} THCoord;               /* 4B total */

typedef struct {
    int toggle_level;     /* 0=off 1=pentagon 2=hex 3=trihex */
    int aperture;         /* 3/4/7 — subdivision factor (future) */
} THGrid;

/* ── Inline helpers ─────────────────────────────────────────── */

/* node_id → pentagon id (1..12) */
static inline uint8_t th_pentagon(THCoord c) {
    return (uint8_t)geo_pentagon_id(c.node_id);
}

/* node_id → local position within pentagon (0..1727) */
static inline uint16_t th_local(uint32_t node_id) {
    return (uint16_t)(node_id % TH_PENTAGON_NODES);
}

/* node_id → shell level (0..11) */
static inline uint8_t th_shell(uint32_t node_id) {
    return (uint8_t)geo_shell_level(node_id);
}

/* snap (vx,vy) → THCoord for given face, respects toggle_level */
static inline THCoord th_snap(uint8_t face, int32_t vx, int32_t vy,
                               const THGrid *g)
{
    THCoord c = {0};
    if (g->toggle_level == 0) return c;

    /* sector via cross-product (same as tw_capture_int) */
    int best_sec = 0;
    int64_t best = INT64_MIN;
    for (int i = 0; i < TW_N_SECTORS; i++) {
        int64_t dot = (int64_t)vx * TW_BOUNDARY_DIR[i][0]
                    + (int64_t)vy * TW_BOUNDARY_DIR[i][1];
        if (dot > best) { best = dot; best_sec = i; }
    }
    if (g->toggle_level == 1) {
        /* coarse: map to node_id at sector base within face */
        uint32_t base = (uint32_t)face * TH_PENTAGON_NODES;
        c.node_id = GEO_WRAP(base + (uint32_t)best_sec * TW_SLOTS_PER);
        return c;
    }

    /* pick hex slot */
    int64_t bd2 = INT64_MAX; int best_slot = 0;
    for (int j = 0; j < TW_SLOTS_PER; j++) {
        int64_t dx = vx - TW_SLOT_LOCAL_I[best_sec][j][0];
        int64_t dy = vy - TW_SLOT_LOCAL_I[best_sec][j][1];
        int64_t d2 = dx*dx + dy*dy;
        if (d2 < bd2) { bd2 = d2; best_slot = j; }
    }
    if (g->toggle_level >= 3) {
        /* compare against tri centroid */
        for (int j = 0; j < TW_SLOTS_PER; j++) {
            int64_t dx = vx - TW_TRI_SLOT_LOCAL_I[best_sec][j][0];
            int64_t dy = vy - TW_TRI_SLOT_LOCAL_I[best_sec][j][1];
            int64_t d2 = dx*dx + dy*dy;
            if (d2 < bd2) { bd2 = d2; best_slot = j; }
        }
    }
    /* map to node_id: face * 1728 + sector * 6 + slot */
    uint32_t base = (uint32_t)face * TH_PENTAGON_NODES;
    c.node_id = GEO_WRAP(base + (uint32_t)best_sec * TW_SLOTS_PER
                         + (uint32_t)best_slot);
    return c;
}

/* ── Bond metric ────────────────────────────────────────────── */

/* step count on trihex graph:
 *   same node        = 0
 *   same pentagon    = 1-2 (within same face)
 *   diff pentagon    = 3  (cross-pentagon bond, always warm)
 */
static inline int th_steps(THCoord a, THCoord b) {
    uint32_t pa = geo_pentagon_id(a.node_id);
    uint32_t pb = geo_pentagon_id(b.node_id);
    if (pa != pb) return 3;
    uint16_t la = th_local(a.node_id);
    uint16_t lb = th_local(b.node_id);
    if (la == lb) return 0;
    /* same pentagon: distance based on local position difference */
    uint16_t diff = (la > lb) ? (la - lb) : (lb - la);
    return (diff <= TW_SLOTS_PER) ? 1 : 2;
}

/* bond strength [0..1] from step count */
static inline float th_bond_strength(THCoord a, THCoord b) {
    static const float W[4] = {1.0f, 0.85f, 0.5f, 0.05f};
    int s = th_steps(a, b);
    return W[s < 4 ? s : 3];
}

/* norm.weight override: cross-pentagon always warm regardless cold filter */
static inline int th_is_always_warm(THCoord norm, THCoord weight) {
    return th_steps(norm, weight) <= 1;
}

#endif /* TRI_HEX_TESS_H */

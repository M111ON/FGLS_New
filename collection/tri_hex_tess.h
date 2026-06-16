/*
 * tri_hex_tess.h — TriHex Tessellation Bond Layer
 *
 * Replaces SID resid_x/y (30B) with (face, tring_pos) = 3B
 * Bond metric = step count on trihex graph (hex↔tri=1, hex↔hex=2)
 * Coverage: 12 faces × 120 positions = 1440 nodes, zero gaps
 *
 * toggle_level 0 = off (raw bond_discovery)
 * toggle_level 1 = coarse (sector-only, 10 nodes/face)
 * toggle_level 2 = hex (60 nodes/face)
 * toggle_level 3 = trihex full (120 nodes/face)
 */

#ifndef TRI_HEX_TESS_H
#define TRI_HEX_TESS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tw_capture_int.h"

/* ── Types ──────────────────────────────────────────────────── */

typedef struct {
    uint8_t  face;        /* 0..11 */
    uint16_t tring_pos;   /* 0..1439 = face*120 + is_tri*60 + sector*6 + slot */
} THCoord;               /* 3B total */

typedef struct {
    int toggle_level;     /* 0=off 1=sector 2=hex 3=trihex */
    int aperture;         /* 3/4/7 — subdivision factor (future) */
} THGrid;

/* ── Inline helpers ─────────────────────────────────────────── */

/* tring_pos → face-local index (0..119) */
static inline uint8_t th_local(uint16_t tring_pos) {
    return (uint8_t)(tring_pos % 120);
}

/* face-local → is_tri, sector, slot */
static inline void th_unpack(uint8_t local,
                              uint8_t *is_tri, uint8_t *sector, uint8_t *slot) {
    *is_tri  = local >= 60 ? 1 : 0;
    uint8_t l = local % 60;
    *sector  = l / TW_SLOTS_PER;
    *slot    = l % TW_SLOTS_PER;
}

/* snap (vx,vy) → THCoord for given face, respects toggle_level */
static inline THCoord th_snap(uint8_t face, int32_t vx, int32_t vy,
                               const THGrid *g)
{
    THCoord c = {face, 0};
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
        c.tring_pos = (uint16_t)(face * 120 + best_sec * TW_SLOTS_PER);
        return c;
    }

    /* pick hex slot */
    int64_t bd2 = INT64_MAX; int best_slot = 0; uint8_t is_tri = 0;
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
            if (d2 < bd2) { bd2 = d2; best_slot = j; is_tri = 1; }
        }
    }
    c.tring_pos = (uint16_t)(face * 120 + is_tri * 60
                              + best_sec * TW_SLOTS_PER + best_slot);
    return c;
}

/* ── Bond metric ────────────────────────────────────────────── */

/* step count on trihex graph:
 *   same node      = 0
 *   hex↔tri same sector = 1
 *   same face diff sector = 2
 *   diff face      = 3  (cross-face bond, always warm)
 */
static inline int th_steps(THCoord a, THCoord b) {
    if (a.face != b.face) return 3;
    uint8_t la = th_local(a.tring_pos);
    uint8_t lb = th_local(b.tring_pos);
    if (la == lb) return 0;
    uint8_t ta, sa, sla, tb, sb, slb;
    th_unpack(la, &ta, &sa, &sla);
    th_unpack(lb, &tb, &sb, &slb);
    if (sa == sb) return 1;   /* hex↔tri same sector */
    return 2;
}

/* bond strength [0..1] from step count */
static inline float th_bond_strength(THCoord a, THCoord b) {
    static const float W[4] = {1.0f, 0.85f, 0.5f, 0.05f};
    int s = th_steps(a, b);
    return W[s < 4 ? s : 3];
}

/* norm.weight override: cross-face always warm regardless cold filter */
static inline int th_is_always_warm(THCoord norm, THCoord weight) {
    return th_steps(norm, weight) <= 1;
}

#endif /* TRI_HEX_TESS_H */

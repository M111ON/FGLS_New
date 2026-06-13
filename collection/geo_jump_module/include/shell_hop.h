/*
 * shell_hop.h — Hop + Bridge: shell_container ↔ geo_jump
 * ════════════════════════════════════════════════════════
 *
 * Connects two address spaces:
 *   shell_container : JUNCTION=6912  (geometry/weight layer)
 *   geo_jump        : GEO_FULL=20736 (routing/navigation layer)
 *
 * Bridge formula (exact integer, no float):
 *   shell → geo : (addr × 20736) / 6912  = addr × 3
 *   geo → shell : (node × 6912)  / 20736 = node / 3
 *
 * Hop modes (map to geo_jump types):
 *   HOP_NEIGHBOR  → JUMP_PENTAGON  (anchor to anchor, elevator)
 *   HOP_WALK      → JUMP_HILBERT   (face walk within anchor zone)
 *   HOP_STREAM    → JUMP_PEANO     (sequential stream cross-layer)
 *   HOP_SCATTER   → JUMP_MOD       (hash scatter, blind spot fill)
 *   HOP_RECOVER   → JUMP_INVERT    (residual/recovery path)
 *
 * Depends on: shell_container.h, geo_jump.h
 * No malloc. No float.
 * ════════════════════════════════════════════════════════
 */

#ifndef SHELL_HOP_H
#define SHELL_HOP_H

#include <stdint.h>
#include "shell_container.h"
#include "geo_jump.h"

/* ── Bridge constants ────────────────────────────────── */
/* 20736 / 6912 = 3 exactly — clean integer ratio */
#define SHELL_TO_GEO_SCALE  3u
#define GEO_TO_SHELL_SCALE  3u   /* divide */

/* ── Hop mode (maps to GeoJumpType) ──────────────────── */
typedef enum {
    HOP_NEIGHBOR = 0,   /* pentagon → pentagon (11 neighbors)     */
    HOP_WALK     = 1,   /* anchor → face walk (Hilbert locality)  */
    HOP_STREAM   = 2,   /* cross-layer Peano stream               */
    HOP_SCATTER  = 3,   /* mod scatter (hash distribution)        */
    HOP_RECOVER  = 4,   /* invert — hit blind spots               */
} HopMode;

/* ── Hop result ──────────────────────────────────────── */
typedef struct {
    uint64_t shell_addr;   /* destination in shell space 0-6911  */
    uint32_t geo_node;     /* destination in geo space 0-20735   */
    uint8_t  anchor_id;    /* which pentagon anchor (0-11)        */
    uint8_t  shell_level;  /* shell layer of destination          */
} HopResult;

/* ── Bridge: shell ↔ geo ─────────────────────────────── */

static inline uint32_t shell_to_geo(uint64_t shell_addr) {
    return (uint32_t)((shell_addr * SHELL_TO_GEO_SCALE) % GEO_FULL);
}

static inline uint64_t geo_to_shell(uint32_t geo_node) {
    return (uint64_t)(geo_node / GEO_TO_SHELL_SCALE) % SHELL_JUNCTION;
}

/* ── HopMode → GeoJumpType ───────────────────────────── */
static inline GeoJumpType hop_to_jump(HopMode mode) {
    switch (mode) {
        case HOP_NEIGHBOR: return JUMP_PENTAGON;
        case HOP_WALK:     return JUMP_HILBERT;
        case HOP_STREAM:   return JUMP_PEANO;
        case HOP_SCATTER:  return JUMP_MOD;
        case HOP_RECOVER:  return JUMP_INVERT;
        default:           return JUMP_PEANO;
    }
}

/* ── Core hop ────────────────────────────────────────── */

/*
 * shell_hop — jump from current address to next via hop mode
 *
 * from_addr : current address in shell space (0-6911)
 * mode      : HOP_NEIGHBOR / WALK / STREAM / SCATTER / RECOVER
 * param     : anchor_id for NEIGHBOR (1-12), quad for WALK, etc.
 *
 * Returns HopResult with shell_addr + geo_node of destination
 */
static inline HopResult shell_hop(uint64_t from_addr,
                                   HopMode mode,
                                   uint32_t param)
{
    /* bridge to geo space */
    uint32_t geo_from = shell_to_geo(from_addr);

    /* jump in geo space */
    GeoJumpType jtype = hop_to_jump(mode);
    uint32_t    geo_to = geo_jump(geo_from, jtype, param);

    /* bridge back to shell space */
    HopResult r;
    r.geo_node    = geo_to;
    r.shell_addr  = geo_to_shell(geo_to);
    r.anchor_id   = (uint8_t)((geo_pentagon_id(geo_to) - 1u) % SHELL_N_ANCHORS);
    r.shell_level = (uint8_t)(geo_shell_level(geo_to) % (SHELL_MAX_ID + 1u));
    return r;
}

/*
 * shell_hop_anchor — hop from one pentagon anchor to another
 *
 * Highest-level API: give me anchor A, jump to anchor B
 * Uses JUMP_PENTAGON elevator — O(1)
 *
 * s           : shell (for anchor table)
 * from_anchor : source anchor 0-11
 * to_anchor   : destination anchor 0-11
 */
static inline HopResult shell_hop_anchor(const ShellContainer *s,
                                          uint8_t from_anchor,
                                          uint8_t to_anchor)
{
    if (from_anchor >= SHELL_N_ANCHORS) from_anchor = 0u;
    if (to_anchor   >= SHELL_N_ANCHORS) to_anchor   = 0u;

    uint64_t from_addr = s->anchor[from_anchor];

    /* param = to_anchor+1 (geo_jump pentagon_id is 1-based) */
    return shell_hop(from_addr, HOP_NEIGHBOR, (uint32_t)to_anchor + 1u);
}

/*
 * shell_hop_walk — anchor as base, walk N steps via Hilbert
 *
 * Useful for: reading neighboring weights around an anchor
 */
static inline HopResult shell_hop_walk(const ShellContainer *s,
                                        uint8_t anchor_id,
                                        uint32_t quad)
{
    if (anchor_id >= SHELL_N_ANCHORS) anchor_id = 0u;
    uint64_t from_addr = s->anchor[anchor_id];
    return shell_hop(from_addr, HOP_WALK, quad);
}

/*
 * shell_hop_cross — hop across shell boundary via stream
 *
 * Used for composite mode: anchor in shell N → land in shell M
 * param = target shell_id (0-11)
 */
static inline HopResult shell_hop_cross(const ShellContainer *s,
                                         uint8_t anchor_id,
                                         uint8_t target_shell)
{
    if (anchor_id >= SHELL_N_ANCHORS) anchor_id = 0u;
    uint64_t from_addr = s->anchor[anchor_id];

    /* stream with stride = target_shell+1 to land in right layer */
    return shell_hop(from_addr, HOP_STREAM, (uint32_t)target_shell + 1u);
}

/* ── Verify round-trip ───────────────────────────────── */

/*
 * shell_bridge_verify — confirm bridge is lossless for all anchors
 * returns 0=OK, -1=drift detected
 */
static inline int shell_bridge_verify(const ShellContainer *s)
{
    for (uint8_t i = 0u; i < SHELL_N_ANCHORS; i++) {
        uint64_t orig    = s->anchor[i];
        uint32_t geo     = shell_to_geo(orig);
        uint64_t back    = geo_to_shell(geo);

        /* allow 1-unit drift from integer division */
        int64_t drift = (int64_t)orig - (int64_t)back;
        if (drift < -2 || drift > 2) return -1;
    }
    return 0;
}

#endif /* SHELL_HOP_H */

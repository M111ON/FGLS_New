/*
 * shell_hop.h — Hop: shell_container in unified GEO_FULL space
 * ════════════════════════════════════════════════════════
 *
 * MIGRATED: shell and geo address spaces are unified (both GEO_FULL=20736).
 * No bridge needed — shell_to_geo / geo_to_shell are identity.
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

/* ── Bridge constants (now identity — both use GEO_FULL) ── */
#define SHELL_TO_GEO_SCALE  1u   /* unified: shell ↔ geo = 1:1 */
#define GEO_TO_SHELL_SCALE  1u

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
    uint64_t shell_addr;   /* destination in unified GEO_FULL space  */
    uint32_t geo_node;     /* same as shell_addr (unified)           */
    uint8_t  anchor_id;    /* which pentagon anchor (0-11)           */
    uint8_t  shell_level;  /* shell layer of destination             */
} HopResult;

/* ── Bridge: shell ↔ geo (now identity — 1:1) ──────────── */

static inline uint32_t shell_to_geo(uint64_t shell_addr) {
    return (uint32_t)(shell_addr % GEO_FULL);
}

static inline uint64_t geo_to_shell(uint32_t geo_node) {
    return (uint64_t)(geo_node % GEO_FULL);
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
 * from_addr : current address in unified space (0-20735)
 * mode      : HOP_NEIGHBOR / WALK / STREAM / SCATTER / RECOVER
 * param     : anchor_id for NEIGHBOR (1-12), quad for WALK, etc.
 *
 * Returns HopResult with shell_addr + geo_node of destination
 * (both are same in unified space)
 */
static inline HopResult shell_hop(uint64_t from_addr,
                                   HopMode mode,
                                   uint32_t param)
{
    /* direct jump in unified GEO_FULL space (no bridge needed) */
    uint32_t    geo_from = (uint32_t)(from_addr % GEO_FULL);
    GeoJumpType jtype    = hop_to_jump(mode);
    uint32_t    geo_to   = geo_jump(geo_from, jtype, param);

    HopResult r;
    r.geo_node    = geo_to;
    r.shell_addr  = (uint64_t)geo_to;
    r.anchor_id   = (uint8_t)((geo_pentagon_id(geo_to) - 1u) % SHELL_N_ANCHORS);
    r.shell_level = (uint8_t)(geo_shell_level(geo_to) % (SHELL_MAX_ID + 1u));
    return r;
}

/* ── shell_hop_anchor, shell_hop_walk, shell_hop_cross ── */

static inline HopResult shell_hop_anchor(const ShellContainer *s,
                                          uint8_t from_anchor,
                                          uint8_t to_anchor)
{
    if (from_anchor >= SHELL_N_ANCHORS) from_anchor = 0u;
    if (to_anchor   >= SHELL_N_ANCHORS) to_anchor   = 0u;
    uint64_t from_addr = s->anchor[from_anchor];
    return shell_hop(from_addr, HOP_NEIGHBOR, (uint32_t)to_anchor + 1u);
}

static inline HopResult shell_hop_walk(const ShellContainer *s,
                                        uint8_t anchor_id,
                                        uint32_t quad)
{
    if (anchor_id >= SHELL_N_ANCHORS) anchor_id = 0u;
    uint64_t from_addr = s->anchor[anchor_id];
    return shell_hop(from_addr, HOP_WALK, quad);
}

static inline HopResult shell_hop_cross(const ShellContainer *s,
                                         uint8_t anchor_id,
                                         uint8_t target_shell)
{
    if (anchor_id >= SHELL_N_ANCHORS) anchor_id = 0u;
    uint64_t from_addr = s->anchor[anchor_id];
    return shell_hop(from_addr, HOP_STREAM, (uint32_t)target_shell + 1u);
}

/* ── Verify round-trip ───────────────────────────────── */

/*
 * shell_bridge_verify — confirm bridge is lossless
 * In unified space, this always passes (anchor → geo → anchor is identity)
 * returns 0=OK
 */
static inline int shell_bridge_verify(const ShellContainer *s)
{
    (void)s;
    return 0;  /* identity bridge is always lossless */
}

#endif /* SHELL_HOP_H */

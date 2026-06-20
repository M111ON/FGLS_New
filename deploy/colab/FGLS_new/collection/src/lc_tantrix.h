#pragma once
#ifndef LC_TANTRIX_H
#define LC_TANTRIX_H

#include <stdbool.h>

/*
 * lc_tantrix.h -- Tantrix 256 Routing Layer
 *
 * 1 byte = 1 tile = 1 routing instruction
 * bits [1:0] = entry gate  (LC_GATE_*)
 * bits [3:2] = exit gate   (LC_GATE_*)
 * bits [5:4] = spoke pair  (0..3 → which spoke cluster)
 * bits [7:6] = tile class  (NORMAL/SKIP/MIRROR/SPECIAL)
 *
 * 256 states: 252 normal + 4 special
 * Connects to Wang edge: entry/exit gate = edge color
 * Connects to lc_twin_gate: gate enum shared directly
 */

#include "lc_twin_gate.h"

/* -- tile classes -- */
typedef enum {
    TANTRIX_CLASS_NORMAL  = 0,  /* standard entry→exit routing */
    TANTRIX_CLASS_SKIP    = 1,  /* is_skip frame, invert polarity */
    TANTRIX_CLASS_MIRROR  = 2,  /* cpair: north↔south flip */
    TANTRIX_CLASS_SPECIAL = 3,  /* NULL/CROSS/MERGE/SPLIT */
} TantrixClass;

/* -- 4 special tiles -- */
#define TANTRIX_NULL   0x00u    /* empty junction -- drop packet */
#define TANTRIX_CROSS  0xAAu    /* WARP↔COLLISION passthrough */
#define TANTRIX_MERGE  0x55u    /* ROUTE↔GROUND merge */
#define TANTRIX_SPLIT  0xFFu    /* broadcast all 4 gates */

/* -- tile type -- */
typedef uint8_t TantrixTile;

/* -- decode tile fields -- */
static inline uint8_t tantrix_entry(TantrixTile t)  { return  t       & 0x3u; }
static inline uint8_t tantrix_exit(TantrixTile t)   { return (t >> 2) & 0x3u; }
static inline uint8_t tantrix_spoke(TantrixTile t)  { return (t >> 4) & 0x3u; }
static inline uint8_t tantrix_class(TantrixTile t)  { return (t >> 6) & 0x3u; }

/* -- encode tile -- */
static inline TantrixTile tantrix_make(uint8_t entry, uint8_t exit,
                                        uint8_t spoke, TantrixClass cls)
{
    return (TantrixTile)((entry & 0x3u)
                       | ((exit  & 0x3u) << 2)
                       | ((spoke & 0x3u) << 4)
                       | ((cls   & 0x3u) << 6));
}

/* -- special tile checks -- */
static inline bool tantrix_is_null(TantrixTile t)    { return t == TANTRIX_NULL; }
static inline bool tantrix_is_cross(TantrixTile t)   { return t == TANTRIX_CROSS; }
static inline bool tantrix_is_merge(TantrixTile t)   { return t == TANTRIX_MERGE; }
static inline bool tantrix_is_split(TantrixTile t)   { return t == TANTRIX_SPLIT; }
static inline bool tantrix_is_special(TantrixTile t) {
    return t == TANTRIX_NULL || t == TANTRIX_CROSS
        || t == TANTRIX_MERGE || t == TANTRIX_SPLIT;
}

/* -- Wang edge compatibility -- */
/* Two tiles connect if: left.exit == right.entry (same gate color) */
static inline bool tantrix_connects(TantrixTile left, TantrixTile right) {
    if (tantrix_is_null(left) || tantrix_is_null(right)) return false;
    if (tantrix_is_split(left) || tantrix_is_split(right)) return true;
    return tantrix_exit(left) == tantrix_entry(right);
}

/* -- gate routing -- */
typedef enum {
    TANTRIX_ROUTE_FORWARD,   /* normal entry→exit */
    TANTRIX_ROUTE_DROP,      /* NULL tile */
    TANTRIX_ROUTE_INVERT,    /* SKIP class: flip gate polarity */
    TANTRIX_ROUTE_BROADCAST, /* SPLIT: all gates active */
    TANTRIX_ROUTE_MERGE,     /* MERGE: two streams become one */
} TantrixRouteResult;

static inline TantrixRouteResult tantrix_route(TantrixTile t,
                                                uint8_t  incoming_gate,
                                                uint8_t *out_gate)
{
    if (tantrix_is_null(t))  { *out_gate = LC_GATE_GROUND; return TANTRIX_ROUTE_DROP; }
    if (tantrix_is_split(t)) { *out_gate = incoming_gate;  return TANTRIX_ROUTE_BROADCAST; }
    if (tantrix_is_merge(t)) { *out_gate = LC_GATE_GROUND; return TANTRIX_ROUTE_MERGE; }
    if (tantrix_is_cross(t)) {
        /* WARP↔COLLISION swap, ROUTE↔GROUND swap */
        static const uint8_t cross_map[4] = {
            LC_GATE_COLLISION, LC_GATE_GROUND,
            LC_GATE_WARP,      LC_GATE_ROUTE
        };
        *out_gate = cross_map[incoming_gate & 3u];
        return TANTRIX_ROUTE_FORWARD;
    }

    /* normal tile: entry must match incoming */
    if (tantrix_entry(t) != incoming_gate) {
        *out_gate = LC_GATE_GROUND;
        return TANTRIX_ROUTE_DROP;   /* gate mismatch = drop */
    }

    uint8_t exit = tantrix_exit(t);
    /* SKIP class: invert polarity (XOR with 0x3) */
    if (tantrix_class(t) == TANTRIX_CLASS_SKIP)
        exit ^= 0x3u;
    /* MIRROR class: swap bit 0 and bit 1 */
    if (tantrix_class(t) == TANTRIX_CLASS_MIRROR)
        exit = (uint8_t)(((exit & 1u) << 1) | ((exit >> 1) & 1u));

    *out_gate = exit;
    return TANTRIX_ROUTE_FORWARD;
}

/* -- junction: route through 6-spoke frustum face -- */
/* spoke_pair (bits[5:4]) selects which 2 of 6 spokes are active */
static inline uint8_t tantrix_active_spokes(TantrixTile t) {
    /* spoke_pair 0→{0,3}, 1→{1,4}, 2→{2,5}, 3→{0,1,2,3,4,5} (split) */
    static const uint8_t spoke_mask[4] = {
        0x09u,  /* 0b00001001 → spokes 0,3 */
        0x12u,  /* 0b00010010 → spokes 1,4 */
        0x24u,  /* 0b00100100 → spokes 2,5 */
        0x3Fu,  /* 0b00111111 → all 6      */
    };
    return spoke_mask[tantrix_spoke(t)];
}

/* -- Domino pair: entry+exit tile -- */
typedef struct {
    TantrixTile entry_tile;
    TantrixTile exit_tile;
} TantrixDomino;

static inline TantrixDomino tantrix_domino(uint8_t gate_in, uint8_t gate_out,
                                            uint8_t spoke)
{
    TantrixDomino d;
    d.entry_tile = tantrix_make(gate_in,  gate_out, spoke, TANTRIX_CLASS_NORMAL);
    d.exit_tile  = tantrix_make(gate_out, gate_in,  spoke, TANTRIX_CLASS_NORMAL);
    return d;
}

static inline bool tantrix_domino_valid(TantrixDomino d) {
    return tantrix_connects(d.entry_tile, d.exit_tile)
        && tantrix_exit(d.entry_tile) == tantrix_entry(d.exit_tile);
}

/* -- verify -- */
static inline int lc_tantrix_verify(void)
{
    /* [T1] decode/encode roundtrip */
    for (uint16_t i = 1u; i < 253u; i++) {
        TantrixTile t = (TantrixTile)i;
        TantrixTile r = tantrix_make(tantrix_entry(t), tantrix_exit(t),
                                     tantrix_spoke(t),
                                     (TantrixClass)tantrix_class(t));
        if (r != t) return -1;
    }

    /* [T2] special tiles correctly identified */
    if (!tantrix_is_null(TANTRIX_NULL))   return -2;
    if (!tantrix_is_cross(TANTRIX_CROSS)) return -3;
    if (!tantrix_is_merge(TANTRIX_MERGE)) return -4;
    if (!tantrix_is_split(TANTRIX_SPLIT)) return -5;

    /* [T3] SPLIT connects to everything */
    for (uint16_t i = 1u; i < 256u; i++) {
        if (!tantrix_connects(TANTRIX_SPLIT, (TantrixTile)i)) return -6;
    }

    /* [T4] NULL connects to nothing */
    for (uint16_t i = 0u; i < 256u; i++) {
        if (tantrix_connects(TANTRIX_NULL, (TantrixTile)i)) return -7;
        if (tantrix_connects((TantrixTile)i, TANTRIX_NULL)) return -8;
    }

    /* [T5] domino self-consistency */
    for (uint8_t g = 0u; g < 4u; g++) {
        TantrixDomino d = tantrix_domino(g, (g + 1u) & 3u, 0u);
        if (!tantrix_domino_valid(d)) return -9;
    }

    /* [T6] CROSS swap is self-inverse */
    for (uint8_t g = 0u; g < 4u; g++) {
        uint8_t out1, out2;
        tantrix_route(TANTRIX_CROSS, g, &out1);
        tantrix_route(TANTRIX_CROSS, out1, &out2);
        if (out2 != g) return -10;
    }

    /* [T7] spoke_mask coverage: all 6 spokes covered across pairs 0-2 */
    uint8_t coverage = tantrix_active_spokes(tantrix_make(0,0,0,TANTRIX_CLASS_NORMAL))
                     | tantrix_active_spokes(tantrix_make(0,0,1,TANTRIX_CLASS_NORMAL))
                     | tantrix_active_spokes(tantrix_make(0,0,2,TANTRIX_CLASS_NORMAL));
    if (coverage != 0x3Fu) return -11;

    return 0;
}

#endif /* LC_TANTRIX_H */

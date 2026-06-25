/*
 * geo_shell_fold.h — Fibonacci Shell Fold (layer existence + hot path)
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Shell layers exist only on Fibonacci clock ticks: tick % fibo[layer] == 0
 * Layer 0 (fibo=1) → always live. Layer 11 (fibo=144) → rare.
 *
 * shell_fold_nearest() — find nearest live layer if target is frozen
 * ring_hot_path() — O(1) pentagon axis access (always live)
 *
 * Depends on: geo_jump.h, geo_dodeca_adj.h
 */
#pragma once
#include <stdint.h>
#include "geo_jump.h"
#include "geo_dodeca_adj.h"
#include "geo_shell.h"

/* Fibonacci clock table: F(0)..F(11) */
static const uint16_t GEO_FIBO[12] = {1,1,2,3,5,8,13,21,34,55,89,144};

/* Check if layer exists at given tick */
static inline uint8_t shell_layer_live(uint8_t layer, uint32_t tick) {
    return (tick % GEO_FIBO[layer % 12u] == 0) ? 1u : 0u;
}

/*
 * shell_fold_nearest — find nearest live layer
 * pent_axis=1: bypass (hot path always live)
 * Otherwise: walk outward layer-1, layer+1, layer-2, layer+2 ...
 * Falls back to layer 0 (always live, fibo=1)
 */
static inline uint8_t shell_fold_nearest(uint8_t layer,
                                          uint32_t tick,
                                          uint8_t  pent_axis) {
    if (pent_axis) return layer;
    layer %= 12u;
    if (tick % GEO_FIBO[layer] == 0) return layer;
    for (uint8_t delta = 1u; delta < 12u; delta++) {
        if (layer >= delta) {
            uint8_t lo = layer - delta;
            if (tick % GEO_FIBO[lo] == 0) return lo;
        }
        uint8_t hi = layer + delta;
        if (hi < 12u && tick % GEO_FIBO[hi] == 0) return hi;
    }
    return 0u;
}

/*
 * ring_hot_path — O(1) pentagon axis access
 * Bypasses fibonacci clock — always live at any tick.
 * pent_id: 0-11, layer: 0-11, globe: 0-1
 * Returns geo_jump node_id of the pentagon center at given layer.
 *
 * Address = face_base + layer×GEO_TOWER + globe_offset
 */
static inline uint32_t ring_hot_path(uint8_t pent_id,
                                      uint8_t layer,
                                      uint8_t globe) {
    uint32_t face_stride = GEO_FULL / GEO_PENTAGONS;
    uint32_t base = (uint32_t)(pent_id % GEO_PENTAGONS) * face_stride;
    uint32_t offset = dodeca_globe_offset(globe);
    return GEO_WRAP(base + (uint32_t)layer * GEO_TOWER + offset);
}

#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SYNC_THRESH 8   // arriving window (tunable)

// XOR angular distance → sync state
static inline uint16_t rail_angular_dist(uint16_t a, uint16_t b) {
    return (a ^ b) % 360;
}

static inline bool rail_sync_ready(uint16_t src, uint16_t expected) {
    return rail_angular_dist(src, expected) == 0;
}

static inline bool rail_sync_arriving(uint16_t src, uint16_t expected) {
    return rail_angular_dist(src, expected) < SYNC_THRESH;
}

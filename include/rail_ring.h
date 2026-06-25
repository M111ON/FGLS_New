#pragma once
#include <stdint.h>

// Ring_A/B/C[512] — angular LUT layer on TRing
// ptr = (θ << 1) & 0x1FF  →  index into ring

#define RAIL_RING_SIZE 1440
#define RAIL_FRAME_STRIDE 37
#define RAIL_FRAME_CYCLE  1440

typedef struct {
    uint16_t enc;      // TRing enc (0..1439)
    uint8_t  zone;     // 0..23
    uint8_t  slot;     // 0..59
} RailRingEntry;

typedef struct {
    RailRingEntry A[RAIL_RING_SIZE];
    RailRingEntry B[RAIL_RING_SIZE];
    RailRingEntry C[RAIL_RING_SIZE];
} RailRing;

// Build ring from TRing stride-37 walk, offset per layer (120° apart = 480 ticks)
static inline void rail_ring_build(RailRing *r) {
    for (int i = 0; i < RAIL_RING_SIZE; i++) {
        uint16_t base = (uint16_t)((i * RAIL_FRAME_STRIDE) % RAIL_FRAME_CYCLE);
        r->A[i].enc  = base;
        r->B[i].enc  = (base + 480)  % RAIL_FRAME_CYCLE;  // +1/3 cycle
        r->C[i].enc  = (base + 960)  % RAIL_FRAME_CYCLE;  // +2/3 cycle
        for (int l = 0; l < 3; l++) {
            RailRingEntry *e = (l==0)?&r->A[i]:(l==1)?&r->B[i]:&r->C[i];
            e->zone = (uint8_t)(e->enc / 60);
            e->slot = (uint8_t)(e->enc % 60);
        }
    }
}

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "rail_ring.h"
#include "rail_sync.h"

#define RAIL_LANES 3

typedef enum { LANE_PARK=0, LANE_OPEN=1, LANE_REWIND=2 } LaneState;

typedef struct {
    uint16_t expected_phase;
    uint8_t  source_lane;
    bool     confirmed;     // heartbeat from source
} ParkCondition;

typedef struct {
    uint16_t     theta[RAIL_LANES];       // θ_A, θ_B, θ_C
    uint16_t     ptr[RAIL_LANES];         // bitshift pointers
    LaneState    state[RAIL_LANES];
    ParkCondition park[RAIL_LANES];
    uint8_t      active;                  // diverge bitmask
} PhaseRail;

// angular ptr: θ × 4 → Ring index (360° → 1440 slots)
static inline uint16_t rail_ptr(uint16_t theta) {
    return theta << 2;
}

// gate(target): peers XOR → {3x invariant}
// returns OPEN/PARK/REWIND
static inline LaneState rail_gate(uint16_t p, uint16_t q) {
    uint16_t xr = (p ^ q) % 360;
    if (xr == 0)          return LANE_PARK;
    if (xr > 180)         return LANE_REWIND;  // overflow half-circle
    return LANE_OPEN;
}

static inline void rail_init(PhaseRail *r, uint16_t tA, uint16_t tB, uint16_t tC) {
    r->theta[0]=tA; r->theta[1]=tB; r->theta[2]=tC;
    for (int i=0;i<RAIL_LANES;i++) r->ptr[i] = rail_ptr(r->theta[i]);
    r->active = (r->ptr[0]^r->ptr[1]) | (r->ptr[1]^r->ptr[2]);
}

// step: update θ by stride, recompute gates
static inline void rail_step(PhaseRail *r, uint16_t step) {
    for (int i=0;i<RAIL_LANES;i++)
        r->theta[i] = (r->theta[i] + step) % 360;

    int peers[3][2] = {{1,2},{0,2},{0,1}};
    for (int i=0;i<RAIL_LANES;i++) {
        LaneState g = rail_gate(r->theta[peers[i][0]], r->theta[peers[i][1]]);
        if (g == LANE_PARK) {
            // check park condition
            ParkCondition *pc = &r->park[i];
            if (!pc->confirmed)
                r->state[i] = LANE_REWIND;
            else if (rail_sync_ready(r->theta[pc->source_lane], pc->expected_phase))
                r->state[i] = LANE_OPEN;
            // else stay PARK
        } else {
            r->state[i] = g;
        }
    }
    r->active = (r->ptr[0]^r->ptr[1]) | (r->ptr[1]^r->ptr[2]);
}

// confirm a parked lane's source is arriving
static inline void rail_confirm(PhaseRail *r, uint8_t lane, uint16_t src_phase, uint8_t src_lane) {
    r->park[lane].confirmed     = rail_sync_arriving(r->theta[src_lane], src_phase);
    r->park[lane].expected_phase = src_phase;
    r->park[lane].source_lane   = src_lane;
}

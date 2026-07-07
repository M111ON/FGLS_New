#pragma once
#include <stdint.h>
#include "geo_temporal_lut.h"

#define GEO_PYR_DEPTH        5u
#define GEO_PYR_PHASE_LEN  144u
#define GEO_PYR_TOTAL      720u

typedef struct {
    uint8_t  level;
    uint8_t  slot;
    uint16_t pos;
} PyramidPos;

typedef struct {
    uint8_t  level;
    uint8_t  present;
    uint8_t  residual;
    uint8_t  complete;
    uint16_t first_gap;
} PyramidPhase;

static inline PyramidPos pyr_decompose(uint16_t pos)
{
    PyramidPos p;
    p.pos   = pos;
    p.level = (uint8_t)(pos / GEO_PYR_PHASE_LEN);
    p.slot  = (uint8_t)(pos % GEO_PYR_PHASE_LEN);
    return p;
}

static inline uint16_t pyr_compose(uint8_t level, uint8_t slot)
{
    return (uint16_t)(level * GEO_PYR_PHASE_LEN + slot);
}

static inline uint16_t pyr_phase_start(uint8_t level)
{
    return (uint16_t)(level * GEO_PYR_PHASE_LEN);
}

static inline uint16_t pyr_phase_end(uint8_t level)
{
    return (uint16_t)(level * GEO_PYR_PHASE_LEN + GEO_PYR_PHASE_LEN - 1u);
}

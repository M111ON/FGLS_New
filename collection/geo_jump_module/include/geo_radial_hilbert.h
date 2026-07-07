#pragma once
#include <stdint.h>
#include "geo_config.h"
#include "geo_net.h"
typedef struct {
    uint16_t unit;
    uint8_t  spoke;
    uint8_t  group;
    uint8_t  hidx;
} RHLine;
typedef struct {
    uint8_t  ok;
    uint8_t  fail_pos;
    uint8_t  repair_spoke;
} RHAuditResult;
static inline uint8_t _rh_rev3(uint8_t v) {
    return (uint8_t)(((v & 1u) << 2) | (v & 2u) | ((v >> 2) & 1u));
}
static inline RHLine rh_map(const GeoNetAddr *a) {
    uint8_t group = (uint8_t)(a->unit >> 3);
    uint8_t lane  = (uint8_t)(a->unit & 7u);
    uint8_t hidx  = (uint8_t)((lane << 3) | _rh_rev3(group));
    return (RHLine){
        .unit  = a->unit,
        .spoke = a->spoke,
        .group = group,
        .hidx  = hidx,
    };
}
static inline RHAuditResult rh_audit_group(
    const uint8_t buf[GEO_GROUP_SIZE],
    uint8_t spoke,
    uint8_t expected_xor
) {
    RHAuditResult r = { .ok = 1u, .fail_pos = 0xFFu,
                        .repair_spoke = (uint8_t)((spoke + 3u) % GEO_SPOKES) };
    for (uint8_t i = 0; i < 4u; i++) {
        if ((buf[i] ^ buf[7u - i]) != expected_xor) {
            r.ok = 0u;
            r.fail_pos = i;
            break;
        }
    }
    return r;
}
static inline uint8_t rh_mirror_spokes(uint8_t spoke, uint8_t mirror_mask) {
    uint8_t inv = (uint8_t)((spoke + 3u) % GEO_SPOKES);
    uint8_t out = (uint8_t)(1u << spoke);
    if (mirror_mask & 0x02u) out |= (uint8_t)(1u << inv);
    if (mirror_mask & 0x04u) out |= (uint8_t)(1u << ((spoke + 1u) % GEO_SPOKES));
    if (mirror_mask == 0x3Fu) out = 0x3Fu;
    return out;
}
typedef struct {
    RHLine   line;
    uint8_t  active_spokes;
    uint8_t  do_audit;
} RHStep;
static inline RHStep rh_step(const GeoNetAddr *a) {
    RHLine l = rh_map(a);
    return (RHStep){
        .line          = l,
        .active_spokes = rh_mirror_spokes(a->spoke, a->mirror_mask),
        .do_audit      = (uint8_t)((a->unit & 7u) == 7u ? 1u : 0u),
    };
}

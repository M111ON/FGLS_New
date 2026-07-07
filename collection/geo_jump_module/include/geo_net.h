#pragma once
#include <stdint.h>
#include <string.h>
#include "geo_cylinder.h"
#include "geo_thirdeye.h"
#define GN_GROUP_SIZE    8u
#define GN_AUDIT_DEPTH   8u
#define GN_GROUPS_FACE   8u
#define GN_LINE_MAX      CYL_FULL_N
#define BARRETT_MOD6_K14  2731U
#define DODECA_PATHS 12
#define DODECA_WINDOW_LO 96
#define DODECA_WINDOW_HI 128
typedef struct {
    uint32_t dodeca_score;
    uint32_t valid_paths;
    uint8_t  last_verdict;
} DodecaStats;
typedef struct {
    uint8_t  spoke;
    uint16_t slot;
    uint8_t  face;
    uint8_t  unit;
    uint8_t  inv_spoke;
    uint8_t  mirror_mask;
    uint8_t  group;
    uint8_t  is_center;
} GeoNetAddr;
typedef struct {
    ThirdEye    eye;
    uint32_t    line_cursor;
    uint32_t    op_count;
    uint32_t    anomaly_signals;
    DodecaStats stats;
    uint64_t    path_cores[DODECA_PATHS];
} GeoNet;
static inline void geo_net_init(GeoNet *gn, GeoSeed seed) {
    memset(gn, 0, sizeof(GeoNet));
    te_init(&gn->eye, seed);
}
static inline uint8_t _gn_mod6(uint32_t n) {
    uint32_t q = (n * BARRETT_MOD6_K14) >> 14;
    return (uint8_t)(n - q * 6U);
}
static inline uint32_t _gn_calc_dodeca_score(const uint64_t *cores, uint32_t n) {
    if (!cores || n == 0) return 0;
    uint32_t score = 0;
    for (uint32_t i = 0; i < n && i < DODECA_PATHS; i++) {
        uint32_t pc = (uint32_t)__builtin_popcountll(cores[i]);
        score += (pc / 8);
    }
    return score;
}
static inline uint8_t _gn_dodeca_verdict(uint32_t score) {
    return (score >= DODECA_WINDOW_LO && score <= DODECA_WINDOW_HI) ? 1 : 0;
}
static inline GeoNetAddr geo_net_route(GeoNet    *gn,
                                        uint64_t   addr,
                                        uint64_t   value,
                                        uint8_t    slot_hot,
                                        GeoSeed    cur)
{
    (void)value;
    uint16_t full_idx = (uint16_t)(addr % CYL_FULL_N);
    uint8_t  spoke    = _gn_mod6(full_idx);
    uint16_t slot     = full_idx / CYL_SPOKES;
    uint8_t  face     = geo_slot_face(slot);
    uint8_t  unit     = geo_slot_unit(slot);
    uint8_t  inv      = geo_spoke_invert(spoke);
    uint8_t  group    = unit / GN_GROUP_SIZE;
    te_tick(&gn->eye, cur, spoke, slot_hot, 0u);
    uint32_t path_idx = gn->op_count % DODECA_PATHS;
    gn->path_cores[path_idx] = cur.gen2;
    if (path_idx == DODECA_PATHS - 1) {
        gn->stats.dodeca_score = _gn_calc_dodeca_score(gn->path_cores, DODECA_PATHS);
        gn->stats.valid_paths  = DODECA_PATHS;
        gn->stats.last_verdict = _gn_dodeca_verdict(gn->stats.dodeca_score);
    }
    uint8_t mask = te_get_mask(&gn->eye, spoke);
    gn->op_count++;
    return (GeoNetAddr){
        .spoke       = spoke,
        .slot        = slot,
        .face        = face,
        .unit        = unit,
        .inv_spoke   = inv,
        .mirror_mask = mask,
        .group       = group,
        .is_center   = geo_slot_is_center(slot),
    };
}
static inline uint8_t geo_net_is_audit_point(const GeoNetAddr *a) {
    return (a->unit % GN_GROUP_SIZE == GN_GROUP_SIZE - 1) ? 1u : 0u;
}
static inline void geo_net_signal_fail(GeoNet *gn) {
    gn->anomaly_signals++;
    gn->eye.cur.hot_slots++;
    if (gn->eye.cur.hot_slots > QRPN_ANOMALY_HOT) {
        gn->eye.qrpn_state = QRPN_ANOMALY;
    }
}
static inline GeoSeed geo_net_force_rewind(GeoNet *gn, int steps) {
    return te_rewind(&gn->eye, steps);
}
static inline uint8_t geo_net_state(const GeoNet *gn) {
    return gn->eye.qrpn_state;
}
static inline const char* geo_net_state_name(const GeoNet *gn) {
    return te_state_name(gn->eye.qrpn_state);
}
static inline void geo_net_status(const GeoNet *gn) {
    printf("[GeoNet] ops=%u  anomaly_signals=%u  state=%s\n",
           gn->op_count, gn->anomaly_signals,
           geo_net_state_name(gn));
    te_status(&gn->eye);
}

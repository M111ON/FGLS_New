#pragma once
#include <stdint.h>
#include <stdio.h>
#include "geomatrix_shared.h"
#include "geo_config.h"
#define CYL_SPOKES      GEO_SPOKES
#define CYL_FACES       GEO_FACES
#define CYL_FACE_UNITS  GEO_FACE_UNITS
#define CYL_SLOTS       GEO_SLOTS
#define CYL_FULL_N      GEO_FULL_N
#define CYL_OUTER_SLOTS GEO_OUTER_SLOTS
#define CYL_CENTER_BASE GEO_CENTER_BASE
#define CYL_CENTER_END  (GEO_CENTER_BASE + GEO_FACE_UNITS - 1)
#define CYL_SIDE_FULL   GEO_SIDE_FULL
#define CYL_SIDE_HALF   27u
#define CYL_EVEN  0
#define CYL_ODD   1
static inline uint8_t geo_slot_is_center(uint16_t slot) {
    return slot >= CYL_CENTER_BASE;
}
static inline uint8_t geo_spoke(uint16_t idx) {
    uint32_t q = ((uint32_t)idx * 10923U) >> 16;
    return (uint8_t)(idx - q * 6);
}
static inline uint16_t geo_spoke_slot(uint16_t idx) {
    return idx / CYL_SPOKES;
}
static inline uint16_t geo_full_idx(uint8_t spoke, uint16_t slot) {
    return (uint16_t)(slot * CYL_SPOKES + spoke);
}
static inline uint8_t geo_spoke_invert(uint8_t spoke) {
    return (spoke + 3) % CYL_SPOKES;
}
static inline uint8_t geo_spoke_ring(uint8_t spoke) {
    return spoke & 1;
}
static inline uint8_t geo_slot_face(uint16_t slot) {
    return (uint8_t)(slot / CYL_FACE_UNITS);
}
static inline uint8_t geo_slot_unit(uint16_t slot) {
    return (uint8_t)(slot % CYL_FACE_UNITS);
}
static inline void geo_idx_to_cyl(
    uint16_t idx,
    uint8_t  *spoke_out,
    uint8_t  *ring_out,
    uint8_t  *cross_out,
    uint8_t  *face_out)
{
    uint16_t slot  = geo_spoke_slot(idx);
    uint8_t  s     = geo_spoke(idx);
    *spoke_out = s;
    *ring_out  = geo_spoke_ring(s);
    *cross_out = geo_spoke_invert(s);
    *face_out  = geo_slot_face(slot);
}
static inline void geo_hilbert_cylinder(uint16_t *out, int n) {
    static const uint8_t order[6] = {0, 3, 1, 4, 2, 5};
    for (int i = 0; i < n; i++) {
        uint8_t  spoke = order[i % CYL_SPOKES];
        uint16_t slot  = (uint16_t)(i / CYL_SPOKES);
        out[i] = geo_full_idx(spoke, slot);
    }
}
static inline void geo_fill_hilbert_cylinder(uint16_t *H_inv, int n) {
    geo_hilbert_cylinder(H_inv, n);
}
static inline void geo_cylinder_stats(const uint16_t *H_inv, int n) {
    int spoke_count[CYL_SPOKES] = {0};
    int center_count = 0;
    int cross_ok = 0;
    for (int i = 0; i < n; i++) {
        uint16_t slot = geo_spoke_slot(H_inv[i]);
        spoke_count[geo_spoke(H_inv[i])]++;
        if (geo_slot_is_center(slot)) center_count++;
        if (i % 2 == 0 && i + 1 < n) {
            uint8_t s0 = geo_spoke(H_inv[i]);
            uint8_t s1 = geo_spoke(H_inv[i+1]);
            if (geo_spoke_invert(s0) == s1) cross_ok++;
        }
    }
    printf("[geo_cylinder_stats] n=%d / full=%d\n", n, CYL_FULL_N);
    printf("  geometry: %d side x %d face x %d unit\n",
           CYL_SPOKES, CYL_FACES, CYL_FACE_UNITS);
    for (int s = 0; s < CYL_SPOKES; s++)
        printf("  spoke %d (%s): %d slots (expect %d)\n",
               s, (s&1) ? "bot/odd " : "top/even",
               spoke_count[s], n / CYL_SPOKES);
    printf("  center face slots: %d (expect %d)\n",
           center_count, (n == CYL_FULL_N) ? (int)(CYL_SPOKES * 64) : -1);
    printf("  cross-center pairs: %d/%d\n", cross_ok, n/2);
    int inv_ok = 1;
    for (int s = 0; s < 3; s++) {
        if (spoke_count[s] != spoke_count[s+3]) {
            printf("  FAIL: spoke %d vs %d\n", s, s+3);
            inv_ok = 0;
        }
    }
    if (inv_ok) printf("  invert symmetry: OK\n");
    printf("  closure: 144x24=%d\n", 144*24);
}

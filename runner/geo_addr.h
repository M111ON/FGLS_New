#ifndef GEO_ADDR_H
#define GEO_ADDR_H

#include <stdint.h>

/*
 * geo_addr.h — Geometric routing layer for tensor storage
 *
 * 24-bit address layout:
 *   bits 23-21: octant (3 bits, 0-7)
 *   bits 20-16: anchor (5 bits, 0-19)
 *   bits 15-8:  sid_mode (8 bits, 256 patterns)
 *   bits 7-0:   reserved (8 bits, future weight-level I/O)
 */

/* precomputed from sign(Mx,My,Mz) of mid(Rx(V,36), Ry(V,36)) */
static const uint8_t GEO_OCTANT[20] = {
    0,1,2,3,4,5,6,7,
    0,0,2,5,3,1,2,4,
    6,7,7,5
};

/* encode: name → 24-bit geo address */
static inline uint32_t geo_addr(const char *name, int ft_idx, uint8_t sid_mode) {
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    uint32_t anchor = h % 20;
    return (GEO_OCTANT[anchor] << 21) | (anchor << 16) | ((uint32_t)sid_mode << 8);
}

/* decode fields */
static inline uint32_t geo_octant(uint32_t a) { return (a >> 21) & 0x07; }
static inline uint32_t geo_anchor(uint32_t a) { return (a >> 16) & 0x1F; }
static inline uint32_t geo_sid_mode(uint32_t a) { return (a >> 8) & 0xFF; }
static inline uint32_t geo_reserved(uint32_t a) { return a & 0xFF; }

/* SID mode encode: pattern(2 bits) + byte_value(6 bits)
 *   00xxxxxx = xor:byte_value
 *   01xxxxxx = set:byte_value
 *   10xxxxxx = rot:K
 *   11xxxxxx = reserved
 */
static inline uint8_t geo_encode_sid(uint8_t pattern, uint8_t byte_val) {
    return (pattern << 6) | (byte_val & 0x3F);
}
static inline uint8_t geo_sid_pattern(uint8_t mode) { return (mode >> 6) & 0x03; }
static inline uint8_t geo_sid_byte(uint8_t mode) { return mode & 0x3F; }

/* SIDSwapEntry: store geo_addr in ft_idx field */
static inline void     geo_sid_set(void *entry, const char *name, int ft_idx, uint8_t sid_mode)
                       { *(int*)entry = (int)geo_addr(name, ft_idx, sid_mode); }
static inline uint32_t geo_sid_get(const void *entry)
                       { return (uint32_t)*(const int*)entry; }

/* delta arrays: write geo_addr into delta_ft_idx[slot] */
static inline void geo_delta_set(int slot, const char *name, int ft_idx, uint8_t sid_mode,
                                 int *delta_ft_idx_arr) {
    delta_ft_idx_arr[slot] = (int)geo_addr(name, ft_idx, sid_mode);
}

#endif /* GEO_ADDR_H */

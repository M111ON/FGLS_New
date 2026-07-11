#ifndef POGLS_GEO_H
#define POGLS_GEO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POGLS_GEO_METATRON_COLS   4u
#define POGLS_GEO_METATRON_ROWS   4u
#define POGLS_GEO_METATRON_FLOORS 3u
#define POGLS_GEO_METATRON_CELLS  (POGLS_GEO_METATRON_COLS * POGLS_GEO_METATRON_ROWS)
#define POGLS_GEO_BLOCK           (POGLS_GEO_METATRON_CELLS * POGLS_GEO_METATRON_FLOORS)
#define POGLS_GEO_TOWER           (POGLS_GEO_BLOCK * POGLS_GEO_METATRON_FLOORS)
#define POGLS_GEO_FULL            (POGLS_GEO_TOWER * POGLS_GEO_TOWER)
#define POGLS_GEO_MOD_PRIME       162u
#define POGLS_GEO_PENTAGONS       12u
#define POGLS_GEO_FIBO_CLOCK      1440u

#define POGLS_GEO_WRAP(x)    ((uint32_t)(x) % POGLS_GEO_FULL)

typedef struct {
    uint32_t sector;
    uint32_t block;
    uint32_t cell;
    uint32_t floor;
    uint32_t tow;
} PoglsGeoCoord;

typedef struct {
    uint8_t used;
    uint8_t triple[3];
} PoglsGeoTriplet;

void pogls_geo_coord(uint32_t addr, PoglsGeoCoord *c);
uint32_t pogls_geo_from_coord(const PoglsGeoCoord *c);
uint32_t pogls_geo_jump(uint32_t from, int delta);

uint32_t pogls_geo_face_addr(uint32_t base, uint32_t face);
int      pogls_geo_addr_valid(uint32_t addr);

uint32_t pogls_geo_name_hash(const char *name);

uint32_t pogls_geo_triplet_vert(uint32_t face, uint32_t edge,
                                 uint32_t vert_idx);

const char* pogls_geo_tier_name(uint8_t tier);

#ifdef __cplusplus
}
#endif

#endif /* POGLS_GEO_H */

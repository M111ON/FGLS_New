#include "pogls_geo.h"

void pogls_geo_coord(uint32_t addr, PoglsGeoCoord *c) {
    memset(c, 0, sizeof(*c));
    uint32_t a = addr % POGLS_GEO_FULL;
    c->tow   = a / POGLS_GEO_TOWER;
    a %= POGLS_GEO_TOWER;
    c->floor = a / POGLS_GEO_BLOCK;
    a %= POGLS_GEO_BLOCK;
    c->cell  = a / POGLS_GEO_METATRON_CELLS;
    a %= POGLS_GEO_METATRON_CELLS;
    c->sector= a;
    c->block = c->cell / POGLS_GEO_METATRON_COLS;
}

uint32_t pogls_geo_from_coord(const PoglsGeoCoord *c) {
    return (uint32_t)(
        ((uint64_t)c->tow    * (uint64_t)POGLS_GEO_TOWER) +
        ((uint64_t)c->floor  * (uint64_t)POGLS_GEO_BLOCK) +
        ((uint64_t)c->cell   * (uint64_t)POGLS_GEO_METATRON_CELLS) +
        (uint64_t)c->sector
    ) % POGLS_GEO_FULL;
}

uint32_t pogls_geo_jump(uint32_t from, int delta) {
    int64_t a = (int64_t)(from % POGLS_GEO_FULL) + delta;
    while (a < 0) a += POGLS_GEO_FULL;
    return (uint32_t)(a % POGLS_GEO_FULL);
}

uint32_t pogls_geo_face_addr(uint32_t base, uint32_t face) {
    uint64_t offset = (uint64_t)face * (uint64_t)POGLS_GEO_BLOCK;
    return (uint32_t)(((uint64_t)base + offset) % POGLS_GEO_FULL);
}

int pogls_geo_addr_valid(uint32_t addr) {
    return addr < POGLS_GEO_FULL;
}

uint32_t pogls_geo_name_hash(const char *name) {
    if (!name || !*name) return 0;
    uint32_t h = 0x811c9dc5u;
    for (const char *p = name; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 0x01000193u;
    }
    return h % POGLS_GEO_FULL;
}

uint32_t pogls_geo_triplet_vert(uint32_t face, uint32_t edge,
                                 uint32_t vert_idx)
{
    if (face >= POGLS_GEO_PENTAGONS) return 0;
    if (edge >= 5) return 0;
    if (vert_idx > 2) return 0;
    static const uint8_t triplets[12][5][3] = {
        {{0,1,2},{0,2,3},{0,3,4},{0,4,5},{0,5,1}},
        {{1,6,2},{1,2,0},{1,0,5},{1,5,7},{1,7,6}},
        {{2,6,8},{2,8,3},{2,3,0},{2,0,1},{2,1,6}},
        {{3,8,9},{3,9,4},{3,4,0},{3,0,2},{3,2,8}},
        {{4,9,10},{4,10,5},{4,5,0},{4,0,3},{4,3,9}},
        {{5,10,7},{5,7,1},{5,1,0},{5,0,4},{5,4,10}},
        {{6,7,10},{6,10,11},{6,11,8},{6,8,2},{6,2,1}},
        {{7,10,6},{7,6,1},{7,1,5},{7,5,10},{7,10,6}},
        {{8,11,9},{8,9,3},{8,3,2},{8,2,6},{8,6,11}},
        {{9,11,10},{9,10,4},{9,4,3},{9,3,8},{9,8,11}},
        {{10,11,6},{10,6,7},{10,7,5},{10,5,4},{10,4,9}},
        {{11,9,8},{11,8,6},{11,6,10},{11,10,9},{11,9,11}},
    };
    return triplets[face][edge][vert_idx % 3];
}

const char* pogls_geo_tier_name(uint8_t tier) {
    switch (tier) {
        case 0: return "144^2 (20736)";
        case 1: return "144^4 (430M)";
        case 2: return "144^6 (~8e14)";
        case 3: return "144^8 (~1.8e17)";
        default: return "unknown";
    }
}

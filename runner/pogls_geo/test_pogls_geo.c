#include "pogls_geo.h"
#include <string.h>
#include <assert.h>

static int pass = 0, fail = 0;
#define TEST(name) do { printf("  [%s]\n", name); } while(0)
#define CHECK(cond, msg) do { if (cond) { pass++; printf("    PASS: %s\n", msg); } else { fail++; printf("    FAIL: %s\n", msg); } } while(0)

int main(void) {
    printf("═══ POGLS Geo Library Test ═══\n\n");

    TEST("Constants");
    CHECK(POGLS_GEO_FULL == 20736, "GEO_FULL = 20736");
    CHECK(POGLS_GEO_TOWER == 144, "GEO_TOWER = 144");
    CHECK(POGLS_GEO_BLOCK == 48, "GEO_BLOCK = 48");
    CHECK(POGLS_GEO_METATRON_CELLS == 16, "METATRON_CELLS = 16");

    TEST("Coordinate decomposition");
    PoglsGeoCoord c;
    pogls_geo_coord(0, &c);
    CHECK(c.tow == 0 && c.floor == 0 && c.sector == 0, "addr 0 → all zero");

    uint32_t addr = 10000;
    pogls_geo_coord(addr, &c);
    uint32_t back = pogls_geo_from_coord(&c);
    CHECK(back == addr % POGLS_GEO_FULL, "coord roundtrip");

    TEST("Jump");
    uint32_t j = pogls_geo_jump(100, 50);
    CHECK(j == 150, "jump forward");
    j = pogls_geo_jump(50, -100);
    CHECK(j == POGLS_GEO_FULL - 50, "jump backward wrap");

    TEST("Face address");
    uint32_t f0 = pogls_geo_face_addr(42, 0);
    CHECK(f0 == 42, "face 0 unchanged");
    uint32_t f1 = pogls_geo_face_addr(42, 1);
    CHECK(f1 != 42, "face 1 changes address");
    CHECK(pogls_geo_addr_valid(f1), "face 1 address valid");

    TEST("Name hash");
    uint32_t h1 = pogls_geo_name_hash("blk.0.attn_q.weight");
    CHECK(h1 < POGLS_GEO_FULL, "hash in range");

    TEST("Address validity");
    CHECK(pogls_geo_addr_valid(0), "addr 0 valid");
    CHECK(pogls_geo_addr_valid(POGLS_GEO_FULL - 1), "max addr valid");
    CHECK(!pogls_geo_addr_valid(POGLS_GEO_FULL), "overflow invalid");

    TEST("Triplet verts");
    uint32_t v = pogls_geo_triplet_vert(0, 0, 0);
    CHECK(v == 0, "face 0, edge 0, vert 0 = 0");
    v = pogls_geo_triplet_vert(3, 2, 1);
    CHECK(v == 4, "face 3, edge 2, vert 1 = 4");

    TEST("Tier name");
    CHECK(strcmp(pogls_geo_tier_name(0), "144^2 (20736)") == 0, "tier 0 name");

    printf("\n═══ Results: %d pass, %d fail ═══\n", pass, fail);
    return fail > 0 ? 1 : 0;
}

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_field_icosphere.h"
#include "geo_jump.h"

static int n_tests = 0, n_pass = 0;
#define CHECK(cond, msg) do { n_tests++; \
    if (!(cond)) fprintf(stderr, "FAIL: %s\n", msg); \
    else { n_pass++; printf("PASS: %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== TOWER NAVIGATION ===\n\n");

    /* ── 1. Decomposition round-trip ── */
    printf("--- 1. Tower position round-trip ---\n");
    for (uint32_t pos = 0; pos < GF_PRIORITY_TOWER_NODES; pos += 97) {
        uint32_t s = gf_tower_shell(pos);
        uint32_t g = gf_tower_geo_slot(pos);
        uint32_t b = gf_tower_sub_slot(pos);
        CHECK(gf_tower_build(s, g, b) == pos, "round-trip");
    }

    /* ── 2. Shell wrapping ── */
    printf("\n--- 2. Shell stepping ---\n");
    uint32_t p = gf_tower_build(0, 0, 0);
    p = gf_tower_step_shell(0, p, -1);
    CHECK(gf_tower_shell(p) == 11, "step -1 from shell 0");
    p = gf_tower_step_shell(0, p, 2);
    CHECK(gf_tower_shell(p) == 1, "step +2 from shell 11");

    /* ── 3. Geo-slot wrapping ── */
    printf("\n--- 3. Geo-slot stepping ---\n");
    p = gf_tower_build(0, 0, 0);
    p = gf_tower_step_slot(0, p, -1);
    CHECK(gf_tower_geo_slot(p) == 143, "step -1 from slot 0");
    p = gf_tower_build(5, 100, 2);
    p = gf_tower_step_slot(0, p, 50);
    CHECK(gf_tower_geo_slot(p) == 6, "step +50 from slot 100");

    /* ── 4. Sub-slot wrapping ── */
    printf("\n--- 4. Sub-slot stepping ---\n");
    p = gf_tower_build(3, 50, 0);
    p = gf_tower_step_sub(0, p, 3);
    CHECK(gf_tower_sub_slot(p) == 3, "step +3 from sub 0");
    p = gf_tower_step_sub(0, p, 1);
    CHECK(gf_tower_sub_slot(p) == 0, "step +1 from sub 3 wraps");

    /* ── 5. Walker ── */
    printf("\n--- 5. GfTowerWalk ---\n");
    uint8_t t0 = icosphere_capture_key_to_trapezoid(0);
    uint32_t q0 = icosphere_capture_key_to_tower_pos(0);
    GfTowerWalk w;
    gf_tower_walk_init(&w, t0, q0, 0);
    CHECK(w.step == 0 && w.geo_trail[0] < GEO_FULL, "init");

    uint32_t g1 = gf_tower_walk_geo(&w, 2, 5);
    CHECK(w.step == 1 && g1 < GEO_FULL, "step 1 (shell +2, slot +5)");

    uint32_t g2 = gf_tower_walk_geo(&w, -1, 0);
    CHECK(w.step == 2 && g2 < GEO_FULL, "step 2 (shell -1)");

    CHECK(gf_tower_walk_geo_at(&w, 0) == w.geo_trail[0], "trail[0]");
    CHECK(gf_tower_walk_geo_at(&w, 2) == g2, "trail[2]");
    CHECK(gf_tower_walk_geo_at(&w, 99) == w.geo_trail[2], "trail clip");

    /* ── 6. Fast-flip ── */
    printf("\n--- 6. Fast-flip ---\n");
    GfTowerWalk w2;
    gf_tower_walk_init(&w2, t0, q0, 0);
    uint8_t ot = w2.trap;
    gf_tower_walk_flip(&w2);
    CHECK(w2.trap != ot, "trap changed");
    gf_tower_walk_flip(&w2);
    CHECK(w2.trap == ot, "double flip back");
    CHECK(w2.geo_trail[2] == w2.geo_trail[0], "flip geo matches original");

    /* ── 7. All keys: 4-direction bounds ── */
    printf("\n--- 7. All keys: step bounds ---\n");
    int ok = 1;
    for (uint32_t k = 0; k < 300; k++) {
        uint8_t tr = icosphere_capture_key_to_trapezoid(k);
        uint32_t po = icosphere_capture_key_to_tower_pos(k);
        uint32_t u = gf_tower_step_shell(tr, po, 1);
        uint32_t d = gf_tower_step_shell(tr, po, -1);
        uint32_t l = gf_tower_step_slot(tr, po, -1);
        uint32_t r = gf_tower_step_slot(tr, po, 1);
        if (gf_tower_shell(u) >= 12 || gf_tower_shell(d) >= 12 ||
            gf_tower_geo_slot(l) >= 144 || gf_tower_geo_slot(r) >= 144) {
            ok = 0; break;
        }
    }
    CHECK(ok, "all 300 keys: 4-direction steps valid");

    /* ── 8. All keys: walker trails ── */
    printf("\n--- 8. All keys: walker trails ---\n");
    ok = 1;
    for (uint32_t k = 0; k < 300; k++) {
        uint8_t tr = icosphere_capture_key_to_trapezoid(k);
        uint32_t po = icosphere_capture_key_to_tower_pos(k);
        GfTowerWalk w3;
        gf_tower_walk_init(&w3, tr, po, k % 12);
        gf_tower_walk_geo(&w3, 0, 1);
        gf_tower_walk_geo(&w3, 0, 1);
        gf_tower_walk_geo(&w3, 0, 1);
        for (uint32_t i = 0; i <= 3 && ok; i++)
            if (w3.geo_trail[i] >= GEO_FULL) ok = 0;
    }
    CHECK(ok, "walker trails: all GEO_FULL valid");

    printf("\n=== %d/%d PASS ===\n", n_pass, n_tests);
    return (n_pass == n_tests) ? 0 : 1;
}

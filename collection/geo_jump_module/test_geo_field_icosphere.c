#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "geo_field_icosphere.h"
#include "geo_triplet.h"
#include "geo_jump.h"

static int n_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); n_fail++; } \
    else { printf("PASS: %s\n", msg); } \
} while(0)

#define CHECK_NEAR(a, b, eps, msg) do { \
    double diff = fabs((double)(a) - (double)(b)); \
    if (diff > (eps)) { \
        fprintf(stderr, "FAIL: %s (got %.6f, expected %.6f, diff %.6f)\n", \
                msg, (double)(a), (double)(b), diff); n_fail++; \
    } else { \
        printf("PASS: %s (%.6f ≈ %.6f)\n", msg, (double)(a), (double)(b)); \
    } \
} while(0)

int main(void) {
    printf("=== GEOMETRIC FIELD ICOSPHERE TESTS ===\n\n");

    /* ── 1. Constants ── */
    printf("--- 1. Constants ---\n");
    CHECK(GF_ICOSPHERE_CAPTURE_SLOTS == 300u, "300 capture slots (20 faces × 15 verts)");
    CHECK(GP_PENT_COUNT == 12u, "12 pentagons");
    CHECK(GP_MAX_DIM == 8u, "8 dim layers");
    CHECK(ICOSA_F4 == 4, "icosa subdivision factor = 4");
    CHECK(ICOSA_VERTS_PER_FACE == 15, "15 vertices per icosa face");

    /* gp_level=4 gives 162 tiles = icosphere f=4 vertex count (10×4²+2) */
    CHECK(gp_face_count(4) == 162u, "gp_level=4 → 162 tiles = icosphere f=4 verts");
    CHECK(gp_face_count(4) == TRIPLET_ICOSPHERE_VERTS,
          "gp_level(4) == icosphere 162v (key insight)");

    /* gp_level=8 gives 642 tiles */
    CHECK(gp_face_count(8) == 642u, "gp_level=8 → 642 tiles (10×64+2)");

    /* ── 2. Capture key → dodeca face mapping ── */
    printf("\n--- 2. Capture key → Dodeca face ---\n");

    /* key 0 = face 0, idx 0 → barycentric (i=0,j=0,k=4) → dominant k=4 = ICOSA_FACES[0][2] */
    uint8_t dface, off;
    icosphere_capture_key_to_dodeca(0, &dface, &off);
    CHECK(dface == (uint8_t)ICOSA_FACES[0][2],
          "key 0 (i=0,j=0,k=4) → dominant = ICOSA_FACES[0][2]");
    CHECK(off == 0, "key 0 → offset 0 (at pentagon center)");

    /* key 14 = face 0, idx 14 → barycentric (0,4,0) → dominant = ICOSA_FACES[0][1] */
    icosphere_capture_key_to_dodeca(14, &dface, &off);
    CHECK(dface == (uint8_t)ICOSA_FACES[0][1],
          "key 14 → dodeca face = ICOSA_FACES[0][1]");

    /* key with balanced weights → dominant determined by largest */
    icosphere_capture_key_to_dodeca(7, &dface, &off);
    CHECK(dface < 12, "key 7 → valid dodeca face (0..11)");
    CHECK(off > 0, "key 7 → offset > 0 (not at pentagon center)");

    /* All keys 0..299 produce valid dodeca faces */
    int all_valid = 1;
    for (uint32_t k = 0; k < 300; k++) {
        uint8_t df, o;
        icosphere_capture_key_to_dodeca(k, &df, &o);
        if (df >= 12) { all_valid = 0; break; }
    }
    CHECK(all_valid, "all 300 capture keys map to valid dodeca face (0..11)");

    /* ── 3. Capture key → GpAddr (chunk-based) ── */
    printf("\n--- 3. Capture key → GpAddr (chunk-based) ---\n");

    /* At gp_level=4: 162 tiles, keys 0..161 → dim 0, keys 162..299 → dim 1 */
    GpAddr a;
    a = icosphere_capture_key_to_chunk_addr(0, 4);
    CHECK(a.tile_id == 0 && a.dim == 0, "key 0 @ level 4 → (tile=0, dim=0)");

    a = icosphere_capture_key_to_chunk_addr(161, 4);
    CHECK(a.tile_id == 161 && a.dim == 0, "key 161 @ level 4 → (tile=161, dim=0)");

    a = icosphere_capture_key_to_chunk_addr(162, 4);
    CHECK(a.tile_id == 0 && a.dim == 1, "key 162 @ level 4 → (tile=0, dim=1)");

    a = icosphere_capture_key_to_chunk_addr(299, 4);
    CHECK(a.tile_id == 137 && a.dim == 1, "key 299 @ level 4 → (tile=137, dim=1)");

    /* At gp_level=8: 642 tiles > 300, all keys in dim 0 */
    a = icosphere_capture_key_to_chunk_addr(0, 8);
    CHECK(a.tile_id == 0 && a.dim == 0, "key 0 @ level 8 → (tile=0, dim=0)");

    a = icosphere_capture_key_to_chunk_addr(299, 8);
    CHECK(a.tile_id == 299 && a.dim == 0, "key 299 @ level 8 → (tile=299, dim=0)");

    /* All tile_ids valid (less than face_max) */
    int all_tiles_valid = 1;
    for (uint32_t k = 0; k < 300; k++) {
        a = icosphere_capture_key_to_chunk_addr(k, 4);
        if (a.tile_id >= gp_face_count(4) || a.dim >= GP_MAX_DIM) {
            all_tiles_valid = 0; break;
        }
    }
    CHECK(all_tiles_valid, "all keys @ level 4 → valid tile_id < 162 && dim < 8");
    for (uint32_t k = 0; k < 300; k++) {
        a = icosphere_capture_key_to_chunk_addr(k, 8);
        if (a.tile_id >= gp_face_count(8) || a.dim >= GP_MAX_DIM) {
            all_tiles_valid = 0; break;
        }
    }
    CHECK(all_tiles_valid, "all keys @ level 8 → valid tile_id < 642 && dim < 8");

    /* ── 4. icosphere_capture_to_gpaddr (stereographic) ── */
    printf("\n--- 4. icosphere_capture_to_gpaddr (stereographic) ---\n");

    /* vx=0, vy=0 → projects to north-ish pole of stereographic projection */
    a = icosphere_capture_to_gpaddr(0, 0, 4);
    CHECK(a.tile_id < gp_face_count(4), "capture(0,0) @ level 4 → valid tile_id");
    CHECK(a.dim < GP_MAX_DIM, "capture(0,0) @ level 4 → valid dim");

    /* vx=207360, vy=0 → edge of projection */
    a = icosphere_capture_to_gpaddr(207360, 0, 4);
    CHECK(a.tile_id < gp_face_count(4), "capture(207360,0) @ level 4 → valid tile_id");

    /* vx=-207360, vy=207360 → opposite edge */
    a = icosphere_capture_to_gpaddr(-207360, 207360, 4);
    CHECK(a.tile_id < gp_face_count(4), "capture(-207360,207360) @ level 4 → valid tile_id");

    /* Different inputs → different positions */
    GpAddr a1 = icosphere_capture_to_gpaddr(0, 0, 4);
    GpAddr a2 = icosphere_capture_to_gpaddr(100000, 50000, 4);
    CHECK(a1.tile_id != a2.tile_id || a1.dim != a2.dim,
          "different inputs → different positions");

    /* ── 5. Reverse: GpAddr → capture key ── */
    printf("\n--- 5. GpAddr → capture key (reverse) ---\n");

    a.tile_id = 0; a.dim = 0;
    uint32_t rk = icosphere_gpaddr_to_capture_key(a, 4);
    CHECK(rk == 0, "gpaddr(0,0) @ level 4 → capture key 0");

    a.tile_id = 50; a.dim = 0;
    rk = icosphere_gpaddr_to_capture_key(a, 4);
    CHECK(rk == 50, "gpaddr(50,0) @ level 4 → capture key 50");

    a.tile_id = 0; a.dim = 1;
    rk = icosphere_gpaddr_to_capture_key(a, 4);
    CHECK(rk == 162, "gpaddr(0,1) @ level 4 → capture key 162");

    a.tile_id = 100; a.dim = 0;
    rk = icosphere_gpaddr_to_capture_key(a, 4);
    CHECK(rk == 100, "gpaddr(100,0) @ level 4 → capture key 100");

    /* ── 6. Capture key → 3D position ── */
    printf("\n--- 6. Capture key → 3D position ---\n");

    double px, py, pz, r;
    icosphere_capture_key_position(0, &px, &py, &pz);
    r = sqrt(px*px + py*py + pz*pz);
    CHECK_NEAR(r, ICOSA_R, 0.001, "key 0 at face-center radius");

    /* Keys at different faces produce different positions */
    double qx, qy, qz;
    icosphere_capture_key_position(15, &qx, &qy, &qz);
    CHECK(fabs(px - qx) + fabs(py - qy) + fabs(pz - qz) > 0.01,
          "different keys → different 3D positions");

    /* All 300 keys have finite radius ≤ ICOSA_R + eps */
    int all_radii_ok = 1;
    for (uint32_t k = 0; k < 300; k++) {
        icosphere_capture_key_position(k, &px, &py, &pz);
        r = sqrt(px*px + py*py + pz*pz);
        /* Raw barycentric blend of 3 sphere points lies INSIDE the face triangle,
         * so radius is ≤ ICOSA_R. Some blends (antipodal midpoints) give r=0. */
        if (r > ICOSA_R + 0.01 || isnan(r) || isinf(r)) { all_radii_ok = 0; break; }
    }
    CHECK(all_radii_ok, "all 300 keys have valid radius (0 ≤ r ≤ ICOSA_R)");

    /* ── 7. GEO_FULL address space mapping ── */
    printf("\n--- 7. GEO_FULL address space ---\n");

    /* All keys map to valid GEO_FULL nodes */
    int geo_valid = 1;
    uint32_t seen_nodes[300] = {0};
    for (uint32_t k = 0; k < 300; k++) {
        uint32_t gn = icosphere_capture_key_to_geo_full(k);
        if (gn >= GEO_FULL) { geo_valid = 0; break; }
        seen_nodes[k] = gn;
    }
    CHECK(geo_valid, "all 300 keys → valid GEO_FULL nodes (< 20736)");

    /* Verify diversity: not all map to same node */
    int diverse = 0;
    for (uint32_t k = 1; k < 300; k++) {
        if (seen_nodes[k] != seen_nodes[0]) { diverse = 1; break; }
    }
    CHECK(diverse, "capture keys map to diverse GEO_FULL nodes");

    /* ── 8. Triplet world compatibility ── */
    printf("\n--- 8. Triplet world compatibility ---\n");

    /* GEO_FULL node → triplet shell level */
    int shell_valid = 1;
    for (uint32_t k = 0; k < 300; k++) {
        uint32_t gn = icosphere_capture_key_to_geo_full(k);
        if (triplet_shell_level(gn) >= 12) { shell_valid = 0; break; }
    }
    CHECK(shell_valid, "all GEO_FULL nodes have valid shell levels");

    /* ── 9. Dominant-face GpAddr mapping ── */
    printf("\n--- 9. Dominant-face GpAddr ---\n");

    for (uint32_t k = 0; k < 300; k++) {
        a = icosphere_capture_key_to_pent_addr(k, 4);
        CHECK(a.tile_id < 12, "pent-face addr: tile_id is a pentagon (0..11)");
        CHECK(a.dim < GP_MAX_DIM, "pent-face addr: dim < 8");
    }

    /* ── 10. Capture key round-trip: capture_key ↔ chunk_addr ↔ capture_key ── */
    printf("\n--- 10. GpAddr round-trip ---\n");

    int roundtrip_ok = 1;
    for (uint32_t k = 0; k < 300; k++) {
        a = icosphere_capture_key_to_chunk_addr(k, 4);
        uint32_t rk2 = icosphere_gpaddr_to_capture_key(a, 4);
        /* rk2 is not always == k (multiple keys can map to same GpAddr at dim 1),
         * but the reverse should give us back the original at dim 0 */
        if (a.dim == 0 && rk2 != k) {
            roundtrip_ok = 0;
            break;
        }
    }
    CHECK(roundtrip_ok, "dim=0 capture keys survive GpAddr round-trip");

    /* ── 11. GEO_FULL → capture key approximate reverse ── */
    printf("\n--- 11. GEO_FULL reverse ---\n");

    uint32_t gn1 = icosphere_capture_key_to_geo_full(0);
    uint32_t rev1 = icosphere_geo_full_to_capture_key(gn1);
    CHECK(rev1 < 300, "geo_full→capture_key gives valid slot");

    uint32_t gn42 = icosphere_capture_key_to_geo_full(42);
    uint32_t rev42 = icosphere_geo_full_to_capture_key(gn42);
    CHECK(rev42 < 300, "geo_full→capture_key for key 42 gives valid slot");

    /* ── 12. Cross-level consistency ── */
    printf("\n--- 12. Cross-level consistency ---\n");

    /* Same capture key at different gp_levels → same chunk-based mapping */
    for (uint32_t k = 0; k < 50; k++) {
        GpAddr a4 = icosphere_capture_key_to_chunk_addr(k, 4);
        GpAddr a8 = icosphere_capture_key_to_chunk_addr(k, 8);
        CHECK(a4.dim == a8.dim && a4.tile_id == a8.tile_id,
              "same capture key → same GpAddr across levels (for small keys)");
    }

    /* ── Summary ── */
    printf("\n=== SUMMARY ===\n");
    if (n_fail == 0)
        printf("ALL TESTS PASSED\n");
    else
        printf("SOME TESTS FAILED (%d)\n", n_fail);

    return n_fail;
}

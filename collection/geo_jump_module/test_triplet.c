#include <stdio.h>
#include <math.h>
#include "include/geo_triplet.h"
#include "include/geo_hidden_pocket.h"

static int n_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); n_fail++; } \
    else { printf("PASS: %s\n", msg); } \
} while(0)

#define CHECK_NEAR(a, b, eps, msg) do { \
    double diff = fabs((double)(a) - (double)(b)); \
    if (diff > (eps)) { fprintf(stderr, "FAIL: %s (got %.6f, expected %.6f, diff %.6f)\n", msg, (double)(a), (double)(b), diff); n_fail++; } \
    else { printf("PASS: %s (%.6f ≈ %.6f)\n", msg, (double)(a), (double)(b)); } \
} while(0)

int main(void) {
    printf("=== TRIPLET WORLD TESTS ===\n\n");

    /* ── Constants ── */
    CHECK(TRIPLET_DODECA_VERTS == 20u, "dodeca has 20 vertices");
    CHECK(TRIPLET_ICOSA_VERTS == 12u, "icosa has 12 vertices");
    CHECK(TRIPLET_PENTAKIS_VERTS == 32u, "pentakis: 20+12=32 vertices");
    CHECK(TRIPLET_ICOSPHERE_VERTS == 162u, "icosphere f=4: 162 vertices");
    CHECK(TRIPLET_PENTAKIS_FACES == 60u, "pentakis: 60 triangular faces");
    CHECK(TRIPLET_ICOSPHERE_FACES == 320u, "icosphere f=4: 320 faces");

    CHECK(triplet_icosphere_vert_count(4) == 162u, "icosphere_vert_count(4) = 162");
    CHECK(triplet_icosphere_face_count(4) == 320u, "icosphere_face_count(4) = 320");
    CHECK(triplet_icosphere_vert_count(1) == 12u, "icosphere_vert_count(1) = 12");
    CHECK(triplet_icosphere_vert_count(2) == 42u, "icosphere_vert_count(2) = 42");

    /* ── Icosphere position table ── */
    CHECK(TRIPLET_ICOSPHERE_POS[0][0] == 0.0f, "icosphere pos 0 x = 0");
    CHECK(TRIPLET_ICOSPHERE_POS[4][0] < -0.7f, "icosphere pos 4 x < -0.7 (face 3 center)");
    CHECK(TRIPLET_ICOSPHERE_POS[46][0] > 1.37f, "icosphere pos 46 x ≈ 1.376 (z+ axis)");
    float px, py, pz;
    triplet_icosphere_pos(148, &px, &py, &pz);
    CHECK(px == 0.0f && py == 0.0f && pz > 1.37f, "icosphere pos 148 = north pole");
    triplet_icosphere_pos(157, &px, &py, &pz);
    CHECK(px == 0.0f && py == 0.0f && pz < -1.37f, "icosphere pos 157 = south pole");

    /* ── Icosa face and edge tables ── */
    CHECK(TRIPLET_ICOSA_FACES_TABLE[0][0] == 0u, "icosa face 0 v0 = face 0");
    CHECK(TRIPLET_ICOSA_FACES_TABLE[0][1] == 3u, "icosa face 0 v1 = face 3");
    CHECK(TRIPLET_ICOSA_FACES_TABLE[0][2] == 4u, "icosa face 0 v2 = face 4");
    CHECK(TRIPLET_ICOSA_EDGES[0][0] == 0u, "icosa edge 0 v0 = face 0");
    CHECK(TRIPLET_ICOSA_EDGES[0][1] == 3u, "icosa edge 0 v1 = face 3");

    /* ── Pentakis triangle table ── */
    CHECK(TRIPLET_PENTAKIS_TRI[0][0] == 0, "tri 0 apex = face 0");
    CHECK(TRIPLET_PENTAKIS_TRI[0][1] == 0, "tri 0 v0 = vertex 0");
    CHECK(TRIPLET_PENTAKIS_TRI[0][2] == 8, "tri 0 v1 = vertex 8");
    CHECK(TRIPLET_PENTAKIS_TRI[4][1] == 16, "tri 4 (face0,edge4) v0 = vertex 16");
    CHECK(TRIPLET_PENTAKIS_TRI[59][0] == 11, "last tri apex = face 11");
    CHECK(TRIPLET_PENTAKIS_TRI[59][2] == 1, "last tri v1 = vertex 1");

    /* ── Shell level classification ── */
    CHECK(triplet_shell_level(0) == 0u, "node 0 = shell level 0");
    CHECK(triplet_shell_level(144*11) == 11u, "node 1584 = shell level 11");
    CHECK(triplet_is_dodeca_surface(144*11) != 0, "shell level 11 = dodeca surface");
    CHECK(triplet_is_inner_surface(0) != 0, "shell level 0 = inner surface");

    /* ── Layer detection from shell level ── */
    CHECK(triplet_layer_for_shell(11) == TRIPLET_DODECA, "level 11 → dodeca");
    CHECK(triplet_layer_for_shell(0) == TRIPLET_ICOSA, "level 0 → icosa");
    CHECK(triplet_layer_for_shell(2) == TRIPLET_PENTAKIS, "level 2 → pentakis");
    CHECK(triplet_layer_for_shell(5) == TRIPLET_ICOSPHERE, "level 5 → icosphere");

    /* ── Pentakis apex computation ── */
    double ax, ay, az;
    triplet_pentakis_apex(0, 0.0, &ax, &ay, &az);
    double R = sqrt(ax*ax + ay*ay + az*az);
    CHECK_NEAR(R, 1.73205, 0.001, "pentakis apex at h=0 is on circumsphere (R=√3)");

    triplet_pentakis_apex(5, 0.3, &ax, &ay, &az);
    double R5 = sqrt(ax*ax + ay*ay + az*az);
    CHECK_NEAR(R5, 2.03205, 0.001, "pentakis apex at h=0.3, face 5: R=1.732+0.3=2.032");

    /* ── Pentakis triangle from node_id ── */
    uint8_t tri = triplet_pentakis_tri_for_node(144*11 + 0);  /* shell 11, sector 0 */
    CHECK(tri < 60, "node at shell 11, sector 0 maps to a valid pentakis triangle");

    printf("\n=== HIDDEN POCKET TESTS ===\n\n");

    /* ── Trapezoid IDs ── */
    PocketTrapId tid = pocket_trap_decode(0);
    CHECK(tid.face == 0 && tid.edge == 0, "decode trap 0 = face 0, edge 0");
    tid = pocket_trap_decode(59);
    CHECK(tid.face == 11 && tid.edge == 4, "decode trap 59 = face 11, edge 4");
    CHECK(pocket_trap_encode(3, 2) == 17u, "encode face 3, edge 2 = 3*5+2 = 17");

    /* ── Trapezoid geometry ── */
    double v[4][3];
    pocket_trapezoid_verts(0, 0, POCKET_DEFAULT_T, v);
    double top_edge_len = sqrt(pow(v[0][0]-v[1][0],2)+pow(v[0][1]-v[1][1],2)+pow(v[0][2]-v[1][2],2));
    double bot_edge_len = sqrt(pow(v[2][0]-v[3][0],2)+pow(v[2][1]-v[3][1],2)+pow(v[2][2]-v[3][2],2));
    CHECK(top_edge_len > bot_edge_len, "outer edge > inner edge (frustum narrows inward)");
    CHECK(top_edge_len > 0.1, "outer edge has non-zero length");
    CHECK(bot_edge_len > 0.01, "inner edge has non-zero length");

    /* Check inner pentagon has 5 vertices */
    double ip[5][3];
    pocket_inner_pentagon(0, POCKET_DEFAULT_T, ip);
    double e0_len = sqrt(pow(ip[0][0]-ip[1][0],2)+pow(ip[0][1]-ip[1][1],2)+pow(ip[0][2]-ip[1][2],2));
    double e1_len = sqrt(pow(ip[1][0]-ip[2][0],2)+pow(ip[1][1]-ip[2][1],2)+pow(ip[1][2]-ip[2][2],2));
    CHECK_NEAR(e0_len, e1_len, 0.001, "inner pentagon edges are equal (regular)");

    /* ── Shell level mapping ── */
    CHECK(pocket_shell_level(0) == 0u, "pocket: node 0 = shell 0");
    CHECK(pocket_shell_level(1584) == 11u, "pocket: node 1584 = shell 11");

    /* ── Outer → Inner routing ── */
    uint32_t outer = 11u * 144u;  /* first node at shell level 11, face 0 */
    uint32_t inner = pocket_outer_to_inner(outer);
    CHECK(inner == 0u, "outer_to_inner(level 11) → level 0, same intra-level pos");
    CHECK(pocket_inner_to_outer(inner) == outer, "round-trip: inner→outer→back");

    /* ── Cross-edge routing ── */
    uint32_t cross = pocket_cross_edge(outer);
    /* Shell 11 face 0 edge 0 → adjacent face from ADJ[0][0] = {4,0} */
    /* Should land on face 4, shell level 11, sector 0 */
    uint32_t x_face = cross / 1728u;
    CHECK(x_face == 4u, "cross edge: node on face 0 edge 0 → face 4");

    /* ── Interpolation ── */
    double ix, iy, iz;
    pocket_interp_pos(outer, POCKET_DEFAULT_T, &ix, &iy, &iz);
    double iR = sqrt(ix*ix + iy*iy + iz*iz);
    CHECK_NEAR(iR, 1.37638, 0.01, "interpolated outer node R ≈ face center distance");

    uint32_t inner_pos = 0;
    pocket_interp_pos(inner_pos, POCKET_DEFAULT_T, &ix, &iy, &iz);
    double iR_inner = sqrt(ix*ix + iy*iy + iz*iz);
    CHECK(iR_inner < 1.7, "interpolated inner node radius < circumsphere");

    printf("\n");
    if (n_fail == 0)
        printf("ALL TESTS PASSED (%d)\n", 51);
    else
        printf("SOME TESTS FAILED (%d)\n", n_fail);
    return n_fail;
}

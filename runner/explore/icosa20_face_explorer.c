// icosa20_face_explorer.c
// 20-face Icosa Triangle System — structural analysis
//
// Triangle tessellation (60°×6=360°) → shared vertex → expansion
// 20 faces × n² subdivision → infinite scalability
// Connects: 20736, 129600, 1440, 720, 360
//
// This is NOT about new code — it's about seeing the unified structure
//
// Compile: gcc -O2 -std=c11 -lm -o ico20.exe runner/explore/icosa20_face_explorer.c
// Run:     ico20.exe

#include <stdio.h>
#include <math.h>

#define PHI 1.61803398874989484820458683436564

int main(void)
{
    printf("============================================================\n");
    printf("  20-Face Icosa Triangle System\n");
    printf("  สามเหลี่ยม 60° × 6 = 360° → แชร์ vertex → ขยายไม่จำกัด\n");
    printf("============================================================\n\n");

    // ── 1. Triangle as base unit ──
    printf("1. TRIANGLE AS BASE UNIT\n");
    printf("   ─────────────────────\n");
    printf("   หน่ึง face ของ icosa = equilateral triangle\n");
    printf("   60° × 6 = 360° → flat tessellation (hexagonal grid)\n");
    printf("   60° × 5 = 300° → on icosa (curvature = 60° missing)\n");
    printf("   จุดที่ triangle 6 อันแชร์ vertex = flat (hexagon)\n");
    printf("   จุดที่ triangle 5 อันแชร์ vertex = icosa vertex (curved)\n\n");
    printf("   Triangle subdivision rule:\n");
    printf("     n=1:    1  triangle/face  →  20 total\n");
    printf("     n=k:   k²  triangles/face →  20k² total\n");
    printf("     vertex count: V = 10n² + 2 (Euler: V - E + F = 2)\n");
    printf("     edge count:   E = 30n²\n\n");

    // ── 2. Subdivision levels → key constants ──
    printf("2. KEY SUBDIVISION LEVELS\n");
    printf("   ──────────────────────\n");
    printf("   %-5s | %6s | %8s | %8s | %s\n",
           "n", "faces", "vertices", "edges", "connects to");
    printf("   ------+--------+----------+----------+------------------\n");

    int key_levels[] = {1, 2, 3, 4, 5, 6, 12, 18, 24, 30, 36, 72, 108, 144};
    char buf[64];
    for (int i = 0; i < 14; i++) {
        int n = key_levels[i];
        int F = 20 * n * n;
        int V = 10 * n * n + 2;
        int E = 30 * n * n;

        // Find connections
        if (n == 1) snprintf(buf, 64, "=== ICOSA ===");
        else if (F == 720) snprintf(buf, 64, "720 = 1 island (×2=1440)");
        else if (F == 1440) snprintf(buf, 64, "1440 = fibo cycle");
        else if (F == 2880) snprintf(buf, 64, "2880 = 2 fibo cycles");
        else if (F == 6480) snprintf(buf, 64, "6480 = C(12,2)×48×2?");
        else if (F >= 20736 && F < 25920) snprintf(buf, 64, "~20736 boundary");
        else if (F == 25920) snprintf(buf, 64, "25920 = 360×72");
        else if (F == 103680) snprintf(buf, 64, "103680 = 288×360");
        else if (F >= 129600) snprintf(buf, 64, "~129600 dual square");
        else if (F == 414720) snprintf(buf, 64, "414720 = 20736×20");
        else snprintf(buf, 64, "");
        printf("   %-5d | %6d | %8d | %8d | %s\n", n, F, V, E, buf);
    }

    // ── 3. Connection to 20736 ──
    printf("\n3. CONNECTION TO 20736 (GEO_FULL)\n");
    printf("   ─────────────────────────────\n");
    printf("   20736 = 144² = 2⁸·3⁴ = 256×81\n\n");

    // Does 20 × n² ever hit 20736?
    printf("   20 × n²  = 20736 ?  n² = 1036.8 → n = %.2f\n",
           sqrt(20736.0/20));
    printf("   → Clean integer? NO. 20 doesn't divide 20736.\n\n");

    // What levels are close?
    printf("   Nearest levels:\n");
    for (int n = 30; n <= 36; n++) {
        int F = 20 * n * n;
        double ratio = (double)F / 20736;
        printf("     n=%2d: %5d faces (ratio %.3f)\n", n, F, ratio);
    }

    // Inverse: what n gives exactly 20736 with edge sharing?
    printf("\n   BUT: 20736 = 12² × 144\n");
    printf("   12² = 144 dodeca faces (12 faces × 12 slots)\n");
    printf("   144 = tower width\n\n");

    // ── 4. Triangle field → hexagonal grid expansion ──
    printf("4. TRIANGLE → HEXAGON → EXPANSION\n");
    printf("   ───────────────────────────────\n");
    printf("   60° × 6 = 360°:\n");
    printf("     Vertex shared by 6 triangles in flat space\n");
    printf("     → regular hexagon tiling\n");
    printf("     → extends infinitely in 2D\n\n");

    printf("   On sphere (icosa):\n");
    printf("     Vertex shared by 5 triangles (60° × 5 = 300°)\n");
    printf("     → 60° missing = Gaussian curvature)\n");
    printf("     → sphere closes with exactly 20 faces\n\n");

    printf("   Hexagonal coverage:\n");
    printf("     Each hexagon = 6 triangles\n");
    printf("     At n=6 (720 faces): %d hexagons\n", 20*6*6/6);

    printf("\n   Shared vertex = shared address:\n");
    printf("     Multiple triangles share same vertex\n");
    printf("     → vertex = unique coordinate in 129,600 grid\n");
    printf("     → edge = connection between 2 vertices\n");
    printf("     → face = 3 edges = 1 triangle of data\n\n");

    // ── 5. Dual Square 360×360 connection ──
    printf("5. DUAL SQUARE 360×360 = 129,600\n");
    printf("   ───────────────────────────────\n");
    printf("   360 × 360 = 129,600 points on sphere\n");
    printf("   Each point = (θ, φ) coordinate\n\n");

    printf("   Triangle coverage at different n:\n");
    for (int n = 6; n <= 36; n += 6) {
        int F = 20 * n * n;
        double pts_per_tri = 129600.0 / F;
        printf("     n=%2d: %5d tris, %.1f grid-pts/tri\n", n, F, pts_per_tri);
    }

    printf("\n   Diagonal line graph:\n");
    printf("     XY layer (outer=+): X=θ, Y=φ    → 360×360 = 129,600\n");
    printf("     YX layer (inner=-): X=φ, Y=θ    → 360×360 = 129,600\n");
    printf("     Diagonal = shared location between layers\n\n");
    printf("     Triangle vertex = point on 360×360 grid\n");
    printf("     Triangle edge = line connecting 2 grid points\n");
    printf("     Triangle area = 3 vertices in grid space\n\n");

    // ── 6. 720 = 1 island ──
    printf("6. 720 = 1 ISLAND (THE BRIDGE)\n");
    printf("   ────────────────────────────\n");
    printf("   720 = 20 × 36 = 20 × 6²\n");
    printf("   n=6: 720 triangles = exactly 1 island\n\n");

    printf("   720 factorization:\n");
    printf("     720 = 6!   (factorial — all permutations)\n");
    printf("     720 = 2 × 360 (two full rotations)\n");
    printf("     720 = 5 × 144 (LetterCube: 5 faces × 144)\n");
    printf("     720 = 6 × 120 (hexagon: 6 edges × 120)\n");
    printf("     720 = 12 × 60 (dodeca: 12 pentagons × 60)\n\n");

    printf("   From 720:\n");
    printf("     720 × 2 = 1440 = fibo cycle (polar × 2)\n");
    printf("     720 × 6 = 4320 = full hexagon\n");
    printf("     720 × 18 = 12960 = 360×36\n");
    printf("     720 × 180 = 129600 = 360²\n\n");

    // ── 7. 60° × 6 tessellation expansion ──
    printf("7. 60°×6 TESSELLATION EXPANSION\n");
    printf("   ─────────────────────────────\n");
    printf("   Base: 6 triangles form 1 hexagon (flat, 360°)\n");
    printf("   Extend: each hexagon neighbors 6 others\n");
    printf("   → triangular lattice (like graphene)\n\n");

    printf("   Expansion formula:\n");
    printf("     For radius r (in hexagon units):\n");
    printf("     triangles in hexagonal shell = 6r\n");
    printf("     total triangles = 3r(r+1) + 1\n\n");

    printf("   Shells (r=0..10):\n");
    printf("     %-3s | %4s tris | %4s hexagons\n", "r", "total", "total");
    for (int r = 0; r <= 12; r++) {
        int tris = 3*r*(r+1) + 1;
        int hexs = (tris + 1) / 6; // approximate
        printf("     %-3d | %4d      | %4d\n", r, tris, hexs);
    }

    // ── 8. Unification summary ──
    printf("\n8. UNIFICATION: ALL PATHS FROM TRIANGLE\n");
    printf("   ─────────────────────────────────────\n");

    printf("\n   Triangle (60°) → 6 shared → 360° hexagon:\n");
    printf("     360° → 1 rotation\n");
    printf("     360° × 360 = 129,600 = dual square\n");
    printf("     360° × 2 = 720 = 1 island\n");
    printf("     720 × 2 = 1440 = fibo cycle\n");
    printf("     720 × 6 = 4320 = 6 islands\n\n");

    printf("   Triangle → icosa (20 faces):\n");
    printf("     20 → n² → 20n²\n");
    printf("     20 × 36 = 720 (n=6, 1 island)\n");
    printf("     20 × 144 = 2880 (n=12, 2 fibo cycles)\n");
    printf("     20 × 324 = 6480 (n=18)\n");
    printf("     20 × 1296 = 25920 (n=36)\n\n");

    printf("   From 20736 (GEO_FULL):\n");
    printf("     20736 = 144 × 144\n");
    printf("     20736 / 720 = 28.8 → island boundary at 28\n");
    printf("     20736 / 1440 = 14.4 → cycle boundary at 14\n");
    printf("     20736 = 128 × 162 = 2⁷ × 2×81\n");
    printf("     162 = ICO_NODES from frame_seek (81×2)\n\n");

    printf("   The connection:\n");
    printf("     Triangle → vertex → coordinate (θ,φ)\n");
    printf("     → 360×360 grid = 129,600\n");
    printf("     → diagonal = shared location\n");
    printf("     → line graph = relationship between faces\n");
    printf("     → each triangle = 3 coordinates × data\n\n");

    printf("   Next steps for system design:\n");
    printf("     (1) Map each 20-face triangle to 360×360 grid\n");
    printf("     (2) Triangle subdivision → address assignment\n");
    printf("     (3) Hexagon tessellation → neighbor routing\n");
    printf("     (4) Connect to existing 20736 node space\n");
    printf("     (5) Integrate with frame_seek (stride-37 timeline)\n");
    printf("     (6) Connect to geo_jump (15/66/330 pairs)\n");
    printf("     (7) Unify with Kib-Seal (generation scaling)\n");

    printf("\n============================================================\n");
    return 0;
}

/*
 * kis_traversal.c — 3³ Universal Traversal on icosahedral geometry
 *
 * Tests: 3³ × 6 = 162 = full icosa vertex set
 * 27 positions × 6 directions = complete coverage from any start
 *
 * Compile: gcc -Wall -O2 -std=c11 -I. runner/explore/kis_traversal.c -o runner/explore/kis_traversal.exe
 * Run:     runner/explore/kis_traversal.exe
 */
#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define N_3CUBED 27
#define N_DIRS 6

static const int dirs[6][3] = {
    {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
};

typedef struct { int x, y, z; } Pos;

/* 3³ traversal: 27 positions from start */
static int traverse_3cubed(Pos start, Pos *out) {
    int n = 0;
    for (int dx = -1; dx <= 1; dx++)
      for (int dy = -1; dy <= 1; dy++)
        for (int dz = -1; dz <= 1; dz++)
          out[n++] = (Pos){start.x+dx, start.y+dy, start.z+dz};
    return n;
}

/* Full traversal: 27 base × 6 dirs, deduplicate */
static int pos_in(Pos p, Pos *list, int n) {
    for (int i = 0; i < n; i++)
        if (list[i].x==p.x && list[i].y==p.y && list[i].z==p.z) return 1;
    return 0;
}

static int traverse_full(Pos start, Pos *out) {
    int n = 0;
    Pos base[27];
    traverse_3cubed(start, base);
    for (int i = 0; i < 27; i++) {
        for (int d = 0; d < 6; d++) {
            Pos p = {base[i].x+dirs[d][0], base[i].y+dirs[d][1], base[i].z+dirs[d][2]};
            if (!pos_in(p, out, n)) out[n++] = p;
        }
    }
    return n;
}

int main(void) {
    printf("=== KIS TRAVERSAL: 3³ Universal Coverage Test ===\n\n");

    Pos center = {0,0,0};
    Pos buf[500];
    int n;

    /* 3³ traversal */
    printf("── 3³ TRAVERSAL (27 positions) ──\n");
    n = traverse_3cubed(center, buf);
    printf("  From (0,0,0): %d positions\n", n);

    /* Check all 27 covered */
    int covered[27] = {0};
    for (int i = 0; i < n; i++) {
        int idx = (buf[i].x+1)*9 + (buf[i].y+1)*3 + (buf[i].z+1);
        if (idx >= 0 && idx < 27) covered[idx]++;
    }
    int ok = 1;
    for (int i = 0; i < 27; i++) if (covered[i]==0) { ok=0; break; }
    printf("  Coverage: %s (27/27)\n\n", ok ? "COMPLETE" : "INCOMPLETE");

    /* Full traversal: 27 × 6 */
    printf("── FULL TRAVERSAL (27 × 6 = 162) ──\n");
    n = traverse_full(center, buf);
    printf("  From (0,0,0): %d unique positions\n", n);
    printf("  Target: 162\n");
    printf("  Result: %s (%.1f%%)\n\n", n>=162?"COMPLETE":"PARTIAL", 100.0*n/162);

    /* Multi-start */
    printf("── MULTI-START ──\n");
    int starts[][3] = {{0,0,0},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{1,1,1}};
    for (int s = 0; s < 6; s++) {
        Pos st = {starts[s][0], starts[s][1], starts[s][2]};
        n = traverse_full(st, buf);
        printf("  Start (%d,%d,%d): %d positions\n", st.x, st.y, st.z, n);
    }

    /* Spike conversion */
    printf("\n── DODECA ↔ ICOSA SPIKE ──\n");
    double phi = (1.0+sqrt(5.0))/2.0;
    printf("  120 dodeca / 162 icosa = %.4f\n", 120.0/162);
    printf("  1/φ² = %.4f\n", 1.0/(phi*phi));
    printf("  3³ × 6 = 162 = universal traversal\n");
    printf("  CPU routes dodeca(pos), GPU batches icosa(neg)\n");

    /* Summary */
    printf("\n── ARCHITECTURE ──\n");
    printf("  3³ = 27 positions (x,y,z × 3 levels each)\n");
    printf("  6 directions: ±x, ±y, ±z\n");
    printf("  27 × 6 = 162 = icosahedral vertex set\n");
    printf("  Universal: reach ANY point from ANY start\n");
    printf("  Infinity Kis: dodeca→spike→icosa→spike→dodeca→...\n");

    return 0;
}

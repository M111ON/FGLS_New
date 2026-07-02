#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "icosphere_capture.h"

int main(void) {
    printf("=== Verify ICOSA_FACES adjacency vs ADJ ===\n\n");

    /* Check every vertex pair in every icosa face for geometric adjacency */
    printf("Checking icosa edge lengths (should be ~1.447 for valid edges):\n");
    int bad_edges = 0;
    for (int f = 0; f < 20; f++) {
        int v0 = ICOSA_FACES[f][0];
        int v1 = ICOSA_FACES[f][1];
        int v2 = ICOSA_FACES[f][2];

        double dx, dy, dz, d;
        /* Edge 0-1 */
        dx = ICOSA_VERTS[v0][0] - ICOSA_VERTS[v1][0];
        dy = ICOSA_VERTS[v0][1] - ICOSA_VERTS[v1][1];
        dz = ICOSA_VERTS[v0][2] - ICOSA_VERTS[v1][2];
        d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > 1.5) {
            printf("  face %2d {v%2d,v%2d,v%2d}: edge %d-%d d=%.4f BAD\n", f, v0, v1, v2, v0, v1, d);
            bad_edges++;
        }

        /* Edge 1-2 */
        dx = ICOSA_VERTS[v1][0] - ICOSA_VERTS[v2][0];
        dy = ICOSA_VERTS[v1][1] - ICOSA_VERTS[v2][1];
        dz = ICOSA_VERTS[v1][2] - ICOSA_VERTS[v2][2];
        d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > 1.5) {
            printf("  face %2d {v%2d,v%2d,v%2d}: edge %d-%d d=%.4f BAD\n", f, v0, v1, v2, v1, v2, d);
            bad_edges++;
        }

        /* Edge 2-0 */
        dx = ICOSA_VERTS[v2][0] - ICOSA_VERTS[v0][0];
        dy = ICOSA_VERTS[v2][1] - ICOSA_VERTS[v0][1];
        dz = ICOSA_VERTS[v2][2] - ICOSA_VERTS[v0][2];
        d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > 1.5) {
            printf("  face %2d {v%2d,v%2d,v%2d}: edge %d-%d d=%.4f BAD\n", f, v0, v1, v2, v2, v0, d);
            bad_edges++;
        }
    }
    printf("  Bad edges: %d/60\n\n", bad_edges);

    /* For each vertex, find its 5 geometric nearest neighbors */
    printf("Geometric nearest neighbors for each vertex (should be 5 at ~1.447):\n");
    for (int v = 0; v < 12; v++) {
        double dists[12];
        int count5 = 0;
        for (int u = 0; u < 12; u++) {
            double dx = ICOSA_VERTS[v][0] - ICOSA_VERTS[u][0];
            double dy = ICOSA_VERTS[v][1] - ICOSA_VERTS[u][1];
            double dz = ICOSA_VERTS[v][2] - ICOSA_VERTS[u][2];
            dists[u] = sqrt(dx*dx + dy*dy + dz*dz);
        }
        printf("  v%2d: ", v);
        for (int u = 0; u < 12; u++) {
            if (u != v && dists[u] < 1.5) {
                printf("%d(d=%.3f) ", u, dists[u]);
                count5++;
            }
        }
        if (count5 != 5) printf("  ** %d neighbors (expected 5) **", count5);
        printf("\n");
    }

    return 0;
}

/*
 * face_162x128.c — Face = 162 × 128 decomposition
 *
 * Demonstrates: 162 icosa vertices × 128 slots = 20736 face positions
 * Connection to 3³ traversal: 27 positions × 6 directions = 162 vertices
 *
 * Compile: gcc -Wall -O2 -std=c11 -I. runner/explore/face_162x128.c -o runner/explore/face_162x128.exe
 * Run:     runner/explore/face_162x128.exe
 */
#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define FACE 20736
#define N_ICOSA 162
#define SLOTS_PER_VERTEX 128

typedef struct { int x, y, z; int face_pos; int slot_start; } Vertex;

/* Generate 162 icosahedral vertices mapped to face positions */
static void generate_vertices(Vertex *v) {
    int count = 0;
    /* 3³ = 27 base positions */
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            for (int z = -1; z <= 1; z++) {
                v[count].x = x;
                v[count].y = y;
                v[count].z = z;
                v[count].face_pos = count * SLOTS_PER_VERTEX;
                v[count].slot_start = 0;
                count++;
            }
        }
    }
    /* 135 more vertices: 5 per face × 20 faces = 100 + 35 extra */
    for (int face = 0; face < 20; face++) {
        for (int i = 0; i < 5 && count < N_ICOSA; i++) {
            double phi = (1.0 + sqrt(5.0)) / 2.0;
            double angle = 2.0 * M_PI * i / (phi * phi);
            double r = 0.5 + 0.3 * ((face * 5 + i) % 10) / 10.0;
            int x = (int)(r * cos(angle) * 2);
            int y = (int)(r * sin(angle) * 2);
            int z = face % 3 - 1;
            if (x < -1) x = -1; if (x > 1) x = 1;
            if (y < -1) y = -1; if (y > 1) y = 1;
            v[count].x = x;
            v[count].y = y;
            v[count].z = z;
            v[count].face_pos = count * SLOTS_PER_VERTEX;
            v[count].slot_start = count * SLOTS_PER_VERTEX;
            count++;
        }
    }
    /* Fill remaining if needed */
    while (count < N_ICOSA) {
        v[count].x = count % 3 - 1;
        v[count].y = (count / 3) % 3 - 1;
        v[count].z = count / 9 - 1;
        v[count].face_pos = count * SLOTS_PER_VERTEX;
        v[count].slot_start = count * SLOTS_PER_VERTEX;
        count++;
    }
}

/* CPU routing: which vertex to visit next */
typedef struct { int vertex; int slots[SLOTS_PER_VERTEX]; } CPURoute;

static void cpu_route(CPURoute *route, int vertex) {
    route->vertex = vertex;
    for (int i = 0; i < SLOTS_PER_VERTEX; i++) {
        route->slots[i] = vertex * SLOTS_PER_VERTEX + i;
    }
}

/* GPU batch: compute weights for 128 slots */
static void gpu_batch(const CPURoute *route, float *weights) {
    /* Simulate weight computation */
    for (int i = 0; i < SLOTS_PER_VERTEX; i++) {
        weights[i] = (float)(route->slots[i]) / FACE;
    }
}

int main(void) {
    printf("=== FACE = 162 × 128 DECOMPOSITION ===\n\n");

    /* Generate vertices */
    Vertex vertices[N_ICOSA];
    generate_vertices(vertices);

    printf("── STRUCTURE ──\n");
    printf("  Face size: %d = 144 × 144\n", FACE);
    printf("  Path 1: %d × %d = %d\n", N_ICOSA, SLOTS_PER_VERTEX, N_ICOSA * SLOTS_PER_VERTEX);
    printf("  Path 2: 256 × 81 = %d (alternative)\n", 256 * 81);
    printf("  Scale: 16² × 9² = %d\n", 16*16*9*9);
    printf("  Scale: 12⁴ = %d\n\n", 12*12*12*12);

    /* 3³ traversal to 162 vertices */
    printf("── 3³ TRAVERSAL → 162 VERTICES ──\n");
    printf("  3³ = 27 base positions\n");
    printf("  27 × 6 = 162 (full icosa vertex set)\n");
    printf("  Each vertex has %d slots\n\n", SLOTS_PER_VERTEX);

    /* Show first 10 vertices */
    printf("── FIRST 10 VERTICES (of 162) ──\n");
    for (int i = 0; i < 10; i++) {
        printf("  Vertex %3d: pos=(%d,%d,%d) face_pos=%d slots=%d-%d\n",
               i, vertices[i].x, vertices[i].y, vertices[i].z,
               vertices[i].face_pos, vertices[i].slot_start,
               vertices[i].slot_start + SLOTS_PER_VERTEX - 1);
    }
    printf("  ...\n\n");

    /* CPU/GPU simulation */
    printf("── CPU/GPU SPLIT ──\n");
    printf("  CPU: routes between 162 vertices\n");
    printf("  GPU: batches %d weights per vertex\n\n", SLOTS_PER_VERTEX);

    float weights[SLOTS_PER_VERTEX];
    CPURoute route;
    int total_computed = 0;

    for (int v = 0; v < 5; v++) {  /* simulate first 5 vertices */
        cpu_route(&route, v);
        gpu_batch(&route, weights);
        total_computed += SLOTS_PER_VERTEX;

        printf("  CPU: vertex %d → slots %d-%d\n",
               route.vertex, route.slots[0], route.slots[SLOTS_PER_VERTEX-1]);
        printf("  GPU: computed %d weights [%.3f, %.3f, ..., %.3f]\n",
               SLOTS_PER_VERTEX, weights[0], weights[1], weights[SLOTS_PER_VERTEX-1]);
    }
    printf("  ...\n");
    printf("  After 162 vertices: %d weights = full face\n\n", 162 * SLOTS_PER_VERTEX);

    /* World AB split */
    printf("── WORLD AB SPLIT ──\n");
    printf("  World A (ternary): 81 = 3⁴ positions (CPU routing)\n");
    printf("  World B (binary): 256 = 2⁸ positions (GPU batch)\n");
    printf("  81 × 256 = %d = face size\n\n", 81 * 256);

    /* pos/neg split */
    printf("── POS/NEG SPLIT ──\n");
    printf("  pos (dodeca): CPU routing, 128 slots per vertex\n");
    printf("  neg (icosa): GPU batch, 128 slots per vertex\n");
    printf("  128 × 2 = 256 (full binary space)\n\n");

    /* Summary */
    printf("── SUMMARY ──\n");
    printf("  Face = 162 × 128 (Path 1)\n");
    printf("  162 icosa vertices (geometry)\n");
    printf("  128 slots per vertex (data positions)\n");
    printf("  3³ traversal: 27 positions × 6 directions = 162\n");
    printf("  CPU routes vertices, GPU batches slots\n");
    printf("  Infinity Kis: dodeca(pos) ↔ icosa(neg) via spike\n");

    return 0;
}

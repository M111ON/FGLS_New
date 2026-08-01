/*
 * kis_f_time.c — f(time) for Infinity Kis Timeline
 *
 * Maps time → (vertex, slot) → weight position on Face = 162 × 128
 *
 * Face = 144 × 144 = 20736 = 162 vertices × 128 slots
 *
 * The mapping:
 *   time ∈ [0, 20735] → position on face (0-20735)
 *   vertex = position / 128 (which of 162 vertices)
 *   slot = position % 128 (which of 128 slots within vertex)
 *
 * Uses geo_frame_seek.h stride-37 for timeline traversal
 *
 * Compile:
 *   gcc -O2 -std=c11 -I. -Icore runner/explore/kis_f_time.c -o runner/explore/kis_f_time.exe -lm
 * Run:
 *   runner/explore/kis_f_time.exe
 */
#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "geo_frame_seek.h"

#define FACE_SIZE 20736
#define N_VERTICES 162
#define SLOTS_PER_VERTEX 128
#define FACE_DIM 144

/* f(time) → (vertex, slot) on Face = 162 × 128 */
static void f_time(uint32_t t, int *vertex, int *slot) {
    /* Map time to position on face using stride-37 */
    uint32_t pos = (t * 37) % FACE_SIZE;

    /* Decompose into vertex and slot */
    *vertex = pos / SLOTS_PER_VERTEX;
    *slot = pos % SLOTS_PER_VERTEX;
}

/* Alternative: f(time) using frame_seek */
static void f_time_frame(uint32_t t, int *vertex, int *slot) {
    /* Use frame_seek to get position on 1440-cycle */
    uint16_t enc = frame_enc(t % 1440);
    DualFrame f = frame_at(enc);

    /* Map frame to face position */
    uint32_t face_pos = f.face * 120 + f.slot;  /* 12 faces × 120 slots = 1440 */

    /* Extend to full 20736 face using cycle depth */
    uint32_t cycle = t / 1440;
    uint32_t full_pos = (face_pos + cycle * 1440) % FACE_SIZE;

    /* Decompose into vertex and slot */
    *vertex = full_pos / SLOTS_PER_VERTEX;
    *slot = full_pos % SLOTS_PER_VERTEX;
}

/* f(time) → 3D coordinates on face */
static void f_time_3d(uint32_t t, int *x, int *y, int *z) {
    uint32_t pos = (t * 37) % FACE_SIZE;

    /* Map to 144×144 grid */
    *x = pos % FACE_DIM;
    *y = pos / FACE_DIM;
    *z = 0;  /* single layer for now */
}

/* Vertex structure: 162 positions in 3D */
typedef struct { int x, y, z; int face_pos; } VInfo;
static VInfo vertices[N_VERTICES];

static void init_vertices(void) {
    for (int i = 0; i < N_VERTICES; i++) {
        vertices[i].face_pos = i * SLOTS_PER_VERTEX;
        vertices[i].x = vertices[i].face_pos % FACE_DIM;
        vertices[i].y = vertices[i].face_pos / FACE_DIM;
        vertices[i].z = 0;
    }
}

int main(void) {
    printf("=== KIS TIMELINE: f(time) → (vertex, slot) ===\n\n");

    init_vertices();

    printf("── FACE STRUCTURE ──\n");
    printf("  Face = %d = %d × %d\n", FACE_SIZE, FACE_DIM, FACE_DIM);
    printf("  Path 1: %d × %d = %d\n", N_VERTICES, SLOTS_PER_VERTEX, N_VERTICES * SLOTS_PER_VERTEX);
    printf("  Path 2: 256 × 81 = %d (alternative)\n", 256 * 81);
    printf("  Scale: 16² × 9² = %d\n\n", 16*16*9*9);

    printf("── f(time) MAPPING ──\n");
    printf("  time ∈ [0, %d] → position on face\n", FACE_SIZE - 1);
    printf("  vertex = position / %d\n", SLOTS_PER_VERTEX);
    printf("  slot = position mod %d\n\n", SLOTS_PER_VERTEX);

    /* Test f(time) for first 20 time steps */
    printf("── FIRST 20 TIME STEPS ──\n");
    for (uint32_t t = 0; t < 20; t++) {
        int vertex, slot;
        f_time(t, &vertex, &slot);
        printf("  t=%3d → vertex=%3d, slot=%3d (pos=%5d)\n",
               t, vertex, slot, vertex * SLOTS_PER_VERTEX + slot);
    }
    printf("  ...\n\n");

    /* Test full cycle (20736 steps) */
    printf("── FULL CYCLE (20736 steps) ──\n");
    int vertex_count[N_VERTICES] = {0};
    int slot_count[SLOTS_PER_VERTEX] = {0};

    for (uint32_t t = 0; t < FACE_SIZE; t++) {
        int vertex, slot;
        f_time(t, &vertex, &slot);
        vertex_count[vertex]++;
        slot_count[slot]++;
    }

    /* Check vertex coverage */
    int vertices_visited = 0;
    for (int v = 0; v < N_VERTICES; v++) {
        if (vertex_count[v] > 0) vertices_visited++;
    }
    printf("  Vertices visited: %d / %d (%.1f%%)\n",
           vertices_visited, N_VERTICES, 100.0 * vertices_visited / N_VERTICES);

    /* Check slot coverage */
    int slots_visited = 0;
    for (int s = 0; s < SLOTS_PER_VERTEX; s++) {
        if (slot_count[s] > 0) slots_visited++;
    }
    printf("  Slots visited: %d / %d (%.1f%%)\n\n",
           slots_visited, SLOTS_PER_VERTEX, 100.0 * slots_visited / SLOTS_PER_VERTEX);

    /* CPU/GPU split */
    printf("── CPU/GPU SPLIT ──\n");
    printf("  CPU: routes between %d vertices\n", N_VERTICES);
    printf("  GPU: batches %d slots per vertex\n", SLOTS_PER_VERTEX);
    printf("  After %d steps: full face covered\n\n", N_VERTICES);

    /* 3D coordinates */
    printf("── 3D COORDINATES ──\n");
    for (uint32_t t = 0; t < 10; t++) {
        int x, y, z;
        f_time_3d(t, &x, &y, &z);
        int vertex, slot;
        f_time(t, &vertex, &slot);
        printf("  t=%2d → (%3d, %3d, %d) vertex=%d slot=%d\n",
               t, x, y, z, vertex, slot);
    }
    printf("  ...\n\n");

    /* Summary */
    printf("── SUMMARY ──\n");
    printf("  f(time) maps time → (vertex, slot)\n");
    printf("  vertex ∈ [0, %d] (162 icosahedral vertices)\n", N_VERTICES - 1);
    printf("  slot ∈ [0, %d] (128 data positions per vertex)\n", SLOTS_PER_VERTEX - 1);
    printf("  Face = %d × %d = %d\n", N_VERTICES, SLOTS_PER_VERTEX, FACE_SIZE);
    printf("  Infinity Kis: dodeca(pos) ↔ icosa(neg) via spike\n");
    printf("  CPU routes vertices, GPU batches slots\n");

    return 0;
}

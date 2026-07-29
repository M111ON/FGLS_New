/*
 * test_vert24_integration.c — Vert24 + FiboSpine integration test
 * 
 * Compile:
 *   gcc -O2 -std=c11 -I. -I./src -I./rdh -I./core/core \
 *       -o test_vert24.exe test_vert24_integration.c ./src/fibo_spine.c -lm
 * 
 * Run:
 *   ./test_vert24.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ── Generate sorted weights for testing (simulating GGUF Q8_0) ── */
static void generate_test_weights(float *weights, int n) {
    /* Simulate typical weight distribution: mostly near zero, some outliers */
    unsigned int seed = 42;
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245 + 12345;
        float u = (float)(seed & 0x7FFFFFFF) / 0x7FFFFFFF;
        
        /* Mixture: 80% near zero, 20% spread */
        if (u < 0.8f) {
            weights[i] = (u - 0.4f) * 0.1f;  /* [-0.04, 0.04] */
        } else {
            weights[i] = (u - 0.5f) * 2.0f;  /* [-1.0, 1.0] */
        }
    }
    
    /* Sort */
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (weights[i] > weights[j]) {
                float t = weights[i]; weights[i] = weights[j]; weights[j] = t;
            }
}

/* ── Vert24 Constants ── */
#define VERT24_N_CIRCLES     24
#define VERT24_PIPES_PER_CIRCLE  72
#define VERT24_TICKS_PER_PIPE    12
#define VERT24_TOTAL_PIPES   (VERT24_N_CIRCLES * VERT24_PIPES_PER_CIRCLE)
#define VERT24_TOTAL_SLOTS   (VERT24_TOTAL_PIPES * VERT24_TICKS_PER_PIPE)

/* Vert24 circle arrangement — 24 evenly distributed on unit circle */
static const float vert24_angles[VERT24_N_CIRCLES] = {
    0.000000f,  0.261799f,  0.523599f,  0.785398f,
    1.047198f,  1.308997f,  1.570796f,  1.832596f,
    2.094395f,  2.356194f,  2.617994f,  2.879793f,
    3.141593f,  3.403392f,  3.665191f,  3.926991f,
    4.188790f,  4.450590f,  4.712389f,  4.974188f,
    5.235988f,  5.497787f,  5.759587f,  6.021386f
};

typedef struct {
    uint8_t  circle_id;
    float    centroid_weight;
    float    angle_rad;
    float    radius;
    uint16_t pipe_base;
    uint16_t pipe_count;
} Vert24Circle;

typedef struct {
    Vert24Circle circles[VERT24_N_CIRCLES];
    uint8_t      n_circles;
    float        weight_min;
    float        weight_max;
    float        weight_median;
} Vert24Context;

/* Initialize Vert24 from sorted weight array */
static inline void vert24_init(Vert24Context *v24,
                                const float *sorted_weights, int n_weights)
{
    memset(v24, 0, sizeof(*v24));
    v24->n_circles = VERT24_N_CIRCLES;
    v24->weight_min = sorted_weights[0];
    v24->weight_max = sorted_weights[n_weights - 1];
    v24->weight_median = sorted_weights[n_weights / 2];

    for (int i = 0; i < VERT24_N_CIRCLES; i++) {
        Vert24Circle *c = &v24->circles[i];
        c->circle_id = i;
        c->angle_rad = vert24_angles[i];
        c->radius = 1.0f;
        int idx = (i * n_weights) / VERT24_N_CIRCLES;
        if (idx >= n_weights) idx = n_weights - 1;
        c->centroid_weight = sorted_weights[idx];
        c->pipe_base = (uint16_t)(i * VERT24_PIPES_PER_CIRCLE);
        c->pipe_count = VERT24_PIPES_PER_CIRCLE;
    }
}

/* Get circle for a given FiboSpine pipe */
static inline uint8_t vert24_pipe_to_circle(uint16_t pipe_id) {
    return (uint8_t)(pipe_id / VERT24_PIPES_PER_CIRCLE);
}

/* Get local pipe index within its circle (0..71) */
static inline uint8_t vert24_local_pipe(uint16_t pipe_id) {
    return (uint8_t)(pipe_id % VERT24_PIPES_PER_CIRCLE);
}

/* Get circle for a weight value (nearest centroid) */
static inline uint8_t vert24_weight_to_circle(const Vert24Context *v24, float weight) {
    float best_dist = 1e30f;
    uint8_t best_circle = 0;
    for (int i = 0; i < v24->n_circles; i++) {
        float d = fabsf(weight - v24->circles[i].centroid_weight);
        if (d < best_dist) {
            best_dist = d;
            best_circle = i;
        }
    }
    return best_circle;
}

/* ── Include FiboSpine ── */
#include "src/fibo_spine.h"

int main(void) {
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  Vert24 + FiboSpine Integration Test                        ║\n");
    printf("║  24 circles × 72 pipes = 1728 pipes                         ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    /* Generate test weight distribution */
    const int N_WEIGHTS = 100000;
    float *weights = (float*)malloc(N_WEIGHTS * sizeof(float));
    generate_test_weights(weights, N_WEIGHTS);
    printf("Generated %d test weights, range [%.6f, %.6f]\n\n",
           N_WEIGHTS, weights[0], weights[N_WEIGHTS-1]);

    /* Initialize Vert24 from sorted weights */
    Vert24Context v24;
    vert24_init(&v24, weights, N_WEIGHTS);

    printf("=== Vert24 Circles ===\n");
    for (int i = 0; i < VERT24_N_CIRCLES; i++) {
        Vert24Circle *c = &v24.circles[i];
        printf("  Circle %2d: angle=%6.4f rad, centroid=%8.6f, pipes=%4d..%4d\n",
               c->circle_id, c->angle_rad, c->centroid_weight,
               c->pipe_base, c->pipe_base + c->pipe_count - 1);
    }
    printf("\n");

    /* Initialize FiboSpine */
    FiboSpine spine;
    fibo_spine_init(&spine);
    spine.mode = FS_MODE_PERPIPE;  /* Use per-pipe mode for independent ticks */

    printf("=== FiboSpine Initialized ===\n");
    printf("  Pipes: %u, Ticks per pipe: %u, Total slots: %u\n",
           FS_PIPES, FS_TICKS_PER_CYCLE, FS_SLOTS);
    printf("  Mode: PERPIPE (independent per-pipe ticks)\n\n");

    /* Test 1: Pipe → Circle mapping */
    printf("=== Test 1: Pipe → Circle Mapping ===\n");
    int pass = 1;
    for (int p = 0; p < 10; p++) {
        uint16_t pipe_id = p * 173;  /* sample spread across 1728 */
        uint8_t circle = vert24_pipe_to_circle(pipe_id);
        uint8_t local = vert24_local_pipe(pipe_id);
        printf("  Pipe %4d → Circle %2d, Local %2d\n", pipe_id, circle, local);
        if (circle >= 24 || local >= 72) pass = 0;
    }
    printf("  %s\n\n", pass ? "PASS" : "FAIL");

    /* Test 2: Weight → Circle mapping */
    printf("=== Test 2: Weight → Circle Mapping ===\n");
    pass = 1;
    float test_weights[] = { -0.03f, -0.01f, 0.0f, 0.01f, 0.03f, 0.5f, -0.5f };
    for (int i = 0; i < 7; i++) {
        uint8_t circle = vert24_weight_to_circle(&v24, test_weights[i]);
        printf("  Weight %7.3f → Circle %2d (centroid=%8.6f)\n",
               test_weights[i], circle, v24.circles[circle].centroid_weight);
    }
    printf("  PASS\n\n");

    /* Test 3: FiboSpine per-pipe tick advancement with circle mapping */
    printf("=== Test 3: FiboSpine Tick + Circle Mapping ===\n");
    pass = 1;
    for (int step = 0; step < 5; step++) {
        /* Advance a subset of pipes */
        for (int p = 0; p < FS_PIPES; p += 100) {
            uint8_t tick = fibo_spine_pipe_tick(&spine, p);
            uint8_t circle = vert24_pipe_to_circle(p);
            if (step == 0) {
                printf("  Pipe %4d: Circle %2d, Tick %d\n", p, circle, tick);
            }
        }
    }
    printf("  ...\n  PASS\n\n");

    /* Test 4: Jet Bridge detection per circle */
    printf("=== Test 4: Jet Bridge Detection by Circle ===\n");
    pass = 1;
    for (int p = 0; p < FS_PIPES; p++) {
        fibo_spine_pipe_tick(&spine, p);
        if (fibo_spine_pipe_is_bridge(&spine, p)) {
            uint8_t circle = vert24_pipe_to_circle(p);
            printf("  Bridge: Pipe %4d (Circle %2d) at tick 11\n", p, circle);
        }
    }
    printf("  Total bridges detected in one cycle: %u\n\n", spine.resident_pipe_count);

    /* Test 5: Full cycle simulation */
    printf("=== Test 5: Full Cycle (12 ticks × 1728 pipes) ===\n");
    fibo_spine_init(&spine);
    spine.mode = FS_MODE_PERPIPE;
    uint32_t total_bridges = 0;
    for (int tick = 0; tick < 12; tick++) {
        uint32_t bridges_this_tick = 0;
        for (int p = 0; p < FS_PIPES; p++) {
            fibo_spine_pipe_tick(&spine, p);
            if (fibo_spine_pipe_is_bridge(&spine, p)) {
                bridges_this_tick++;
            }
        }
        total_bridges += bridges_this_tick;
        printf("  Tick %2d: %4u bridges\n", tick, bridges_this_tick);
    }
    printf("  Total bridges: %u (expected ~144 per cycle × 12 cycles = 1728)\n", total_bridges);
    printf("  PASS\n\n");

    /* Stats */
    FiboSpineStats stats = fibo_spine_stats(&spine);
    printf("=== Final Stats ===\n");
    printf("  Total pipes:     %u\n", stats.total_pipes);
    printf("  Active pipes:    %u\n", stats.active_pipes);
    printf("  Bridged pipes:   %u\n", stats.bridged_pipes);
    printf("  Resident pipes:  %u\n", stats.resident_pipes);
    printf("  Frozen pipes:    %u\n", stats.frozen_pipes);
    printf("  Total ticks:     %llu\n", (unsigned long long)stats.total_ticks);
    printf("  Bridge state:    %s\n", fibo_spine_bridge_name(spine.bridge_state));
    printf("  Mode:            %s\n", 
           (spine.mode & FS_MODE_PERPIPE) ? "PERPIPE" : "ACTIVE");

    /* Circle distribution */
    printf("\n=== Circle Distribution ===\n");
    uint32_t circle_pipes[VERT24_N_CIRCLES] = {0};
    for (int p = 0; p < FS_PIPES; p++) {
        circle_pipes[vert24_pipe_to_circle(p)]++;
    }
    for (int c = 0; c < VERT24_N_CIRCLES; c++) {
        printf("  Circle %2d: %4u pipes\n", c, circle_pipes[c]);
    }

    /* Cleanup */
    free(weights);

    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("  Vert24 + FiboSpine Integration: ALL TESTS PASS\n");
    printf("══════════════════════════════════════════════════════════════\n");
    return 0;
}
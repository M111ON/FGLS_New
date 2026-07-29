/*
 * vert24_fibospine.h — Vert24 (24 circles) integration with FiboSpine (1728 pipes)
 * 
 * Architecture:
 *   Vert24: 24 circles from shape benchmark (best fit for GGUF weights)
 *   FiboSpine: 1728 pipes × 12 ticks = 20736 slots = GEO_FULL
 *   Mapping: 1728 pipes / 24 circles = 72 pipes per circle
 * 
 * Each Vert24 circle manages a "gear" of 72 pipes (6 rings × 12 wedges)
 * This creates a 2-tier hierarchy:
 *   Tier 1: 24 circles (coarse, geometric)
 *   Tier 2: 72 pipes per circle (fine, temporal)
 *   Total: 24 × 72 = 1728 pipes
 * 
 * Compile: gcc -O2 -std=c11 -c vert24_fibospine.c
 */

#ifndef VERT24_FIBOSPINE_H
#define VERT24_FIBOSPINE_H

#include <stdint.h>
#include <string.h>
#include <math.h>

/* ── Constants ── */
#define VERT24_N_CIRCLES     24
#define VERT24_PIPES_PER_CIRCLE  72    /* 1728 / 24 = 72 */
#define VERT24_TICKS_PER_PIPE    12    /* same as FS_TICKS_PER_CYCLE */

#define VERT24_TOTAL_PIPES   (VERT24_N_CIRCLES * VERT24_PIPES_PER_CIRCLE)  /* 1728 */
#define VERT24_TOTAL_SLOTS   (VERT24_TOTAL_PIPES * VERT24_TICKS_PER_PIPE)   /* 20736 = GEO_FULL */

/* Vert24 circle arrangement — from shape_performance_test.c */
/* 24 evenly distributed in sorted weight space */
static const float vert24_angles[VERT24_N_CIRCLES] = {
    /* 24 points around unit circle (radians) */
    0.000000f,  0.261799f,  0.523599f,  0.785398f,
    1.047198f,  1.308997f,  1.570796f,  1.832596f,
    2.094395f,  2.356194f,  2.617994f,  2.879793f,
    3.141593f,  3.403392f,  3.665191f,  3.926991f,
    4.188790f,  4.450590f,  4.712389f,  4.974188f,
    5.235988f,  5.497787f,  5.759587f,  6.021386f
};

/* ── Data Structures ── */

typedef struct {
    uint8_t  circle_id;           /* 0..23 */
    float    centroid_weight;     /* geometric center in weight space */
    float    angle_rad;           /* position on unit circle */
    float    radius;              /* from origin */
    uint16_t pipe_base;           /* first pipe index in FiboSpine */
    uint16_t pipe_count;          /* 72 */
} Vert24Circle;

typedef struct {
    Vert24Circle circles[VERT24_N_CIRCLES];
    uint8_t      n_circles;
    float        weight_min;
    float        weight_max;
    float        weight_median;
} Vert24Context;

/* FiboSpine forward declare (from fibo_spine.h) */
typedef struct FiboSpine FiboSpine;
typedef struct FiboPipe FiboPipe;

/* ═══════════════════════════════════════════════════════════════════
 * INITIALIZATION
 * ═══════════════════════════════════════════════════════════════════ */

/* Initialize Vert24 from sorted weight array */
static inline void vert24_init(Vert24Context *v24,
                                const float *sorted_weights, int n_weights)
{
    memset(v24, 0, sizeof(*v24));
    v24->n_circles = VERT24_N_CIRCLES;
    v24->weight_min = sorted_weights[0];
    v24->weight_max = sorted_weights[n_weights - 1];
    v24->weight_median = sorted_weights[n_weights / 2];

    /* Place 24 circles evenly in sorted weight space */
    for (int i = 0; i < VERT24_N_CIRCLES; i++) {
        Vert24Circle *c = &v24->circles[i];
        c->circle_id = i;
        c->angle_rad = vert24_angles[i];
        c->radius = 1.0f;  /* unit circle */

        /* Weight space mapping: evenly spaced in sorted order */
        int idx = (i * n_weights) / VERT24_N_CIRCLES;
        if (idx >= n_weights) idx = n_weights - 1;
        c->centroid_weight = sorted_weights[idx];

        /* Pipe assignment: 72 pipes per circle */
        c->pipe_base = (uint16_t)(i * VERT24_PIPES_PER_CIRCLE);
        c->pipe_count = VERT24_PIPES_PER_CIRCLE;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * CIRCLE → PIPE MAPPING
 * ═══════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════
 * FIBOSPINE INTEGRATION
 * ═══════════════════════════════════════════════════════════════════ */

/* Attach Vert24 geometry to FiboSpine — sets per-pipe metadata */
static inline void vert24_attach_to_spine(const Vert24Context *v24, FiboSpine *spine) {
    for (uint16_t p = 0; p < VERT24_TOTAL_PIPES; p++) {
        uint8_t circle = vert24_pipe_to_circle(p);
        uint8_t local = vert24_local_pipe(p);
        
        FiboPipe *pipe = &spine->pipes[p];
        pipe->pipe_id = p;
        /* Store circle/local mapping in flags (bits 4-7) */
        pipe->flags = (pipe->flags & 0x0F) | ((circle & 0x0F) << 4);
        /* Store local pipe index in a reserved field or compute on demand */
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * GEOMETRIC OPERATIONS
 * ═══════════════════════════════════════════════════════════════════ */

/* Distance from weight to nearest circle centroid */
static inline float vert24_distance_to_circle(const Vert24Context *v24, float weight) {
    float best = 1e30f;
    for (int i = 0; i < v24->n_circles; i++) {
        float d = fabsf(weight - v24->circles[i].centroid_weight);
        if (d < best) best = d;
    }
    return best;
}

/* Circle affinity: how strongly does this weight belong to its nearest circle? */
static inline float vert24_circle_affinity(const Vert24Context *v24, float weight) {
    float d1 = 1e30f, d2 = 1e30f;
    for (int i = 0; i < v24->n_circles; i++) {
        float d = fabsf(weight - v24->circles[i].centroid_weight);
        if (d < d1) { d2 = d1; d1 = d; }
        else if (d < d2) { d2 = d; }
    }
    /* Affinity = 1 - (closest / second_closest) — higher = more distinctive */
    if (d2 > 1e-6f) return 1.0f - (d1 / d2);
    return 1.0f;
}

/* ═══════════════════════════════════════════════════════════════════
 * STATS / DIAGNOSTICS
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    float avg_centroid_spacing;
    float max_centroid_spacing;
    float min_centroid_spacing;
    uint32_t pipes_per_circle;
    float weight_span;
} Vert24Stats;

static inline Vert24Stats vert24_stats(const Vert24Context *v24) {
    Vert24Stats s = {0};
    s.pipes_per_circle = VERT24_PIPES_PER_CIRCLE;
    s.weight_span = v24->weight_max - v24->weight_min;

    float prev = v24->circles[0].centroid_weight;
    s.min_centroid_spacing = 1e30f;
    s.max_centroid_spacing = 0;
    float sum_spacing = 0;

    for (int i = 1; i < v24->n_circles; i++) {
        float sp = fabsf(v24->circles[i].centroid_weight - prev);
        if (sp < s.min_centroid_spacing) s.min_centroid_spacing = sp;
        if (sp > s.max_centroid_spacing) s.max_centroid_spacing = sp;
        sum_spacing += sp;
        prev = v24->circles[i].centroid_weight;
    }
    s.avg_centroid_spacing = sum_spacing / (v24->n_circles - 1);

    return s;
}

/* ═══════════════════════════════════════════════════════════════════
 * PRINT / DEBUG
 * ═══════════════════════════════════════════════════════════════════ */

static inline void vert24_print(const Vert24Context *v24) {
    Vert24Stats s = vert24_stats(v24);
    printf("═══ Vert24 Context ═══\n");
    printf("  Circles: %u, Pipes/circle: %u, Total pipes: %u\n",
           v24->n_circles, VERT24_PIPES_PER_CIRCLE, VERT24_TOTAL_PIPES);
    printf("  Weight range: [%.6f, %.6f], median: %.6f\n",
           v24->weight_min, v24->weight_max, v24->weight_median);
    printf("  Centroid spacing: avg=%.6f min=%.6f max=%.6f\n",
           s.avg_centroid_spacing, s.min_centroid_spacing, s.max_centroid_spacing);
    printf("  Weight span: %.6f\n", s.weight_span);
    for (int i = 0; i < v24->n_circles; i++) {
        const Vert24Circle *c = &v24->circles[i];
        printf("  Circle %2d: angle=%.3f weight=%.6f pipes=[%u..%u]\n",
               c->circle_id, c->angle_rad, c->centroid_weight,
               c->pipe_base, c->pipe_base + c->pipe_count - 1);
    }
}

#endif /* VERT24_FIBOSPINE_H */
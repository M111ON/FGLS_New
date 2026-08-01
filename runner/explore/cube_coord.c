/*
 * cube_coord.c — Test coordinate compressibility in 20736³ address space
 *
 * Maps weight indices to 3D coordinates in 20736³ using various strategies,
 * then measures how compressible the coordinate list is.
 *
 * Strategies:
 *   0: naive reshape (i → x,y,z) — no geometric structure
 *   1: stride-37 mapping (temporal → spatial, preserves locality)
 *   2: fibonacci spiral on face, depth = layer
 *   3: icosahedral orbit mapping (symmetry-based)
 *
 * Compile:
 *   gcc -Wall -O2 -std=c11 -I. runner/explore/cube_coord.c -o runner/explore/cube_coord.exe
 * Run:
 *   runner/explore/cube_coord.exe I:/model/Qwen3-0.6B-Q8_0.gguf
 */

#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define FACE 20736    /* 144² = 12×12×144 */
#define DEPTH 20736   /* 3D extension */
#define CUBE (FACE * (uint64_t)DEPTH)  /* 20736² */
#define CUBE3 (CUBE * (uint64_t)DEPTH) /* 20736³ */

/* coordinate struct */
typedef struct { uint32_t x, y, z; } Coord;

/* Strategy 0: naive reshape — weight i → (i%FACE, (i/FACE)%DEPTH, i/(FACE*DEPTH)) */
static Coord naive_map(uint64_t i) {
    Coord c;
    c.x = i % FACE;
    c.y = (i / FACE) % DEPTH;
    c.z = i / CUBE;
    return c;
}

/* Strategy 1: stride-37 on face, layer = depth
 * face_pos = (i * 37) % FACE (geometric jump preserves locality)
 * layer = i / FACE
 */
static Coord stride37_map(uint64_t i) {
    Coord c;
    c.x = (uint32_t)((i * 37) % FACE);
    c.y = (uint32_t)((i / FACE) % DEPTH);
    c.z = 0;  /* single-layer for now */
    return c;
}

/* Strategy 2: fibonacci spiral on 144×144 face, depth = layer
 * Maps weight index to spiral position on face
 */
static Coord fibo_map(uint64_t i) {
    Coord c;
    uint64_t face_idx = i % FACE;
    double phi = (1.0 + sqrt(5.0)) / 2.0;
    double angle = 2.0 * M_PI * face_idx / (phi * phi);
    double radius = sqrt((double)face_idx) * sqrt(FACE / (M_PI));
    int row = (int)(72 + radius * cos(angle));
    int col = (int)(72 + radius * sin(angle));
    if (row < 0) row = 0;
    if (row >= 144) row = 143;
    if (col < 0) col = 0;
    if (col >= 144) col = 143;
    c.x = (uint32_t)(row * 144 + col);
    c.y = (uint32_t)((i / FACE) % DEPTH);
    c.z = 0;
    return c;
}

/* Strategy 3: icosahedral orbit — group weights by orbit under 60-element symmetry
 * Maps i → (orbit_representative, group_element, layer)
 */
typedef struct { uint64_t rep; int ge; } OrbitInfo;
static OrbitInfo icosahedral_orbit(uint64_t i, uint64_t n_vals) {
    OrbitInfo o;
    /* group elements: 60 (icosahedral rotation group) */
    /* face positions: FACE = 20736 = 60 * 345.6 ≈ 60 * 346 */
    o.ge = i % 60;
    o.rep = i / 60;
    return o;
}

/* compressibility test: delta-encode a uint32 array, measure bits */
static double measure_delta_bits(uint32_t *arr, uint64_t n) {
    if (n < 2) return 0;
    uint64_t total_bits = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t delta = (i == 0) ? arr[0] : arr[i] - arr[i-1];
        /* count bits needed for delta (unsigned, but can wrap) */
        int bits = 0;
        uint32_t v = delta;
        while (v) { bits++; v >>= 1; }
        if (bits == 0) bits = 1;  /* at least 1 bit for zero */
        total_bits += bits;
    }
    return (double)total_bits / n;
}

/* zstd-like simple entropy estimate: byte-level Shannon */
static double measure_entropy_bytes(uint8_t *data, uint64_t len) {
    uint64_t freq[256] = {0};
    for (uint64_t i = 0; i < len; i++) freq[data[i]]++;
    double ent = 0;
    for (int i = 0; i < 256; i++) {
        if (freq[i] == 0) continue;
        double p = (double)freq[i] / len;
        ent += -p * log2(p);
    }
    return ent;
}

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    /* count Q8_0 weights */
    uint64_t n_q8 = 0;
    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) continue;
        n_q8 += (gf->tensors[t].size_bytes / 34) * 32;
    }
    printf("=== CUBE COORDINATE COMPRESSIBILITY TEST ===\n");
    printf("  20736³ address space: %" PRIu64 " positions\n", (uint64_t)CUBE3);
    printf("  Q8_0 weights: %" PRIu64 " (%.1f M)\n", n_q8, n_q8 / 1e6);
    printf("  utilization: %.4f%%\n\n", 100.0 * n_q8 / CUBE3);

    /* raw storage for comparison */
    double raw_mb = n_q8 / 1048576.0;
    printf("  raw Q8_0: %.1f MB (%.1f GB)\n", raw_mb, raw_mb / 1024);

    /* test each strategy with a sample (first 1M weights) */
    uint64_t sample = n_q8;
    if (sample > 2000000) sample = 2000000;  /* cap for speed */

    printf("\n── COORDINATE COMPRESSIBILITY (sample: %" PRIu64 " weights) ──\n", sample);

    /* Strategy 0: naive */
    {
        uint32_t *xs = malloc(sample * 4);
        uint32_t *ys = malloc(sample * 4);
        for (uint64_t i = 0; i < sample; i++) {
            Coord c = naive_map(i);
            xs[i] = c.x; ys[i] = c.y;
        }
        double xb = measure_delta_bits(xs, sample);
        double yb = measure_delta_bits(ys, sample);
        printf("  [0] naive reshape:     x_delta=%.2f bits/val, y_delta=%.2f bits/val\n", xb, yb);
        printf("      total coords: %.1f MB (delta-encoded, estimate)\n",
               (xb + yb) * sample / 8 / 1048576.0);
        free(xs); free(ys);
    }

    /* Strategy 1: stride-37 */
    {
        uint32_t *xs = malloc(sample * 4);
        uint32_t *ys = malloc(sample * 4);
        for (uint64_t i = 0; i < sample; i++) {
            Coord c = stride37_map(i);
            xs[i] = c.x; ys[i] = c.y;
        }
        double xb = measure_delta_bits(xs, sample);
        double yb = measure_delta_bits(ys, sample);
        printf("  [1] stride-37:         x_delta=%.2f bits/val, y_delta=%.2f bits/val\n", xb, yb);
        printf("      total coords: %.1f MB (delta-encoded, estimate)\n",
               (xb + yb) * sample / 8 / 1048576.0);
        free(xs); free(ys);
    }

    /* Strategy 2: fibonacci */
    {
        uint32_t *xs = malloc(sample * 4);
        uint32_t *ys = malloc(sample * 4);
        for (uint64_t i = 0; i < sample; i++) {
            Coord c = fibo_map(i);
            xs[i] = c.x; ys[i] = c.y;
        }
        double xb = measure_delta_bits(xs, sample);
        double yb = measure_delta_bits(ys, sample);
        printf("  [2] fibonacci spiral:  x_delta=%.2f bits/val, y_delta=%.2f bits/val\n", xb, yb);
        printf("      total coords: %.1f MB (delta-encoded, estimate)\n",
               (xb + yb) * sample / 8 / 1048576.0);
        free(xs); free(ys);
    }

    /* Strategy 3: icosahedral orbit */
    {
        uint32_t *reps = malloc(sample * 4);
        uint32_t *ges = malloc(sample * 4);
        for (uint64_t i = 0; i < sample; i++) {
            OrbitInfo o = icosahedral_orbit(i, n_q8);
            reps[i] = (uint32_t)(o.rep % FACE);
            ges[i] = (uint32_t)o.ge;
        }
        double rb = measure_delta_bits(reps, sample);
        double gb = measure_delta_bits(ges, sample);
        printf("  [3] icosahedral orbit: rep_delta=%.2f bits/val, ge_delta=%.2f bits/val\n", rb, gb);
        printf("      total coords: %.1f MB (delta-encoded, estimate)\n",
               (rb + gb) * sample / 8 / 1048576.0);
        free(reps); free(ges);
    }

    /* what would 43 bits per coord look like? */
    printf("\n── THEORETICAL LIMITS ──\n");
    printf("  43 bits/coord (naive): %.1f GB\n", 43.0 * n_q8 / 8 / 1073741824.0);
    printf("  32 bits/coord (if coords fit 32-bit): %.1f GB\n", 32.0 * n_q8 / 8 / 1073741824.0);
    printf("  20 bits/coord (if coords ~1M unique): %.1f GB\n", 20.0 * n_q8 / 8 / 1073741824.0);
    printf("  10 bits/coord (if coords ~1K unique): %.1f MB\n", 10.0 * n_q8 / 8 / 1048576.0);

    gguf_close(gf);
    return 0;
}

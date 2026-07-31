/* test_virtual_codec.c — Test virtual (lazy) contour codec */

#define CONTOUR_CODEC_VIRTUAL_IMPLEMENTATION
#include "contour_codec_virtual.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void test_virtual(const char *label, int faces, int W, int H, int L, ccsv_strategy strat, int sparsity_pct) {
    ccsv_config cfg = ccsv_default_config();
    cfg.faces = faces;
    cfg.W = W; cfg.H = H; cfg.L = L;
    cfg.strategy = strat;
    
    int total_cells = ccsv_total_cells(&cfg);
    
    ccsv_virtual_ctx *ctx = ccsv_create(&cfg);
    if (!ctx) {
        printf("  %s: CREATE FAILED\n", label);
        return;
    }
    
    const ccsv_config *actual = ccsv_get_config(ctx);
    int geo = actual->geo_full;
    int dim = actual->geo_dim;
    
    // Generate sparse test data
    ccsv_cell *data = (ccsv_cell*)malloc(total_cells * sizeof(ccsv_cell));
    int n = 0;
    for (int i = 0; i < total_cells; i++) {
        int face = i / (W * H * L);
        int rem = i % (W * H * L);
        int z = rem / (W * H);
        rem %= (W * H);
        int y = rem / W;
        int x = rem % W;
        
        // Sparsity: only keep some cells
        if ((i * 100) % 100 < sparsity_pct) {
            data[n].face = face;
            data[n].x = x;
            data[n].y = y;
            data[n].z = z;
            data[n].value = (int8_t)((i * 37 + 13) % 256 - 128);
            if (data[n].value == 0) data[n].value = 1;
            data[n].global_idx = i;
            n++;
        }
    }
    
    // Batch encode
    int collisions = ccsv_encode_all(ctx, data, n);
    
    // Verify count
    int stored = ccsv_count(ctx);
    
    // Verify random access
    int mismatches = 0;
    for (int i = 0; i < n; i++) {
        int8_t v = ccsv_get(ctx, data[i].face, data[i].x, data[i].y, data[i].z);
        if (v != data[i].value) mismatches++;
    }
    
    // Verify zeros return 0
    int zero_check = 0;
    for (int i = 0; i < total_cells; i++) {
        int face = i / (W * H * L);
        int rem = i % (W * H * L);
        int z = rem / (W * H);
        rem %= (W * H);
        int y = rem / W;
        int x = rem % W;
        
        int8_t v = ccsv_get(ctx, face, x, y, z);
        // Only check a few
        if (i < 100 && v != 0) zero_check++;
    }
    
    // Batch decode
    ccsv_cell *out = (ccsv_cell*)malloc(total_cells * sizeof(ccsv_cell));
    int decoded = ccsv_decode_all(ctx, out, total_cells);
    
    int decode_mismatches = 0;
    for (int i = 0; i < n; i++) {
        if (data[i].value != out[i].value) decode_mismatches++;
    }
    
    // Benchmark single ops
    const int ITERS = 50000;
    #ifdef _WIN32
    typedef struct { long long QuadPart; } LARGE_INTEGER;
    __declspec(dllimport) int __stdcall QueryPerformanceFrequency(LARGE_INTEGER *);
    __declspec(dllimport) int __stdcall QueryPerformanceCounter(LARGE_INTEGER *);
    LARGE_INTEGER freq, s, e;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&s);
    #else
    struct timespec s, e;
    clock_gettime(CLOCK_MONOTONIC, &s);
    #endif
    
    for (int i = 0; i < ITERS; i++) {
        int idx = i % n;
        ccsv_get(ctx, data[idx].face, data[idx].x, data[idx].y, data[idx].z);
        ccsv_set(ctx, data[idx].face, data[idx].x, data[idx].y, data[idx].z, data[idx].value);
    }
    
    #ifdef _WIN32
    QueryPerformanceCounter(&e);
    double ns = (double)(e.QuadPart - s.QuadPart) * 1e9 / freq.QuadPart;
    #else
    clock_gettime(CLOCK_MONOTONIC, &e);
    double ns = (e.tv_sec - s.tv_sec) * 1e9 + (e.tv_nsec - s.tv_nsec);
    #endif
    
    double ns_per_op = ns / (ITERS * 2);
    
    printf("  %s: total=%5d sparse=%4d stored=%4d geo=%5d util=%5.1f%% coll=%d mm=%d %.2f ns/op\n",
           label, total_cells, n, stored, geo, 100.0 * n / geo, collisions, mismatches + decode_mismatches, ns_per_op);
    
    ccsv_free(ctx);
    free(data);
    free(out);
}

int main() {
    printf("╔══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Virtual Contour Codec — Lazy Sparse Map Test                              ║\n");
    printf("║  6 faces = 6 cube directions (±X, ±Y, ±Z)                                 ║\n");
    printf("║  12 faces = 12 dodecahedron faces                                          ║\n");
    printf("║  No geo array allocated — O(1) hash map only for non-zero cells           ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════════╝\n\n");
    
    // Test various configurations
    printf("--- 6-face cube directions (virtual sparse) ---\n");
    test_virtual("6x5x5x5  25%% sparse", 6, 5, 5, 5, CCSV_STRIDE37, 25);
    test_virtual("6x7x7x7  10%% sparse", 6, 7, 7, 7, CCSV_STRIDE37, 10);
    test_virtual("6x10x10x10 5%% sparse", 6, 10, 10, 10, CCSV_STRIDE37, 5);
    
    printf("\n--- 12-face dodecahedron (virtual sparse) ---\n");
    test_virtual("12x8x8x8  25%% sparse", 12, 8, 8, 8, CCSV_STRIDE37, 25);
    test_virtual("12x12x12x12 10%% sparse", 12, 12, 12, 12, CCSV_STRIDE37, 10);
    
    printf("\n--- Strategy comparison (6x7x7x7, 25%% sparse) ---\n");
    test_virtual("stride37:sequential", 6, 7, 7, 7, CCSV_SEQUENTIAL, 25);
    test_virtual("stride37:stride37", 6, 7, 7, 7, CCSV_STRIDE37, 25);
    test_virtual("stride37:face_region", 6, 7, 7, 7, CCSV_FACE_REGION, 25);
    test_virtual("stride37:grid", 6, 7, 7, 7, CCSV_GRID, 25);
    
    printf("\n--- Zero deletion test ---\n");
    {
        ccsv_config cfg = ccsv_default_config();
        cfg.faces = 6; cfg.W = 10; cfg.H = 10; cfg.L = 10;
        ccsv_virtual_ctx *ctx = ccsv_create(&cfg);
        
        ccsv_set(ctx, 0, 0, 0, 0, 42);
        printf("  After set(0,0,0,0,42): count=%d, get=%d\n", ccsv_count(ctx), ccsv_get(ctx, 0, 0, 0, 0));
        
        ccsv_set(ctx, 0, 0, 0, 0, 0);  // delete
        printf("  After set(0,0,0,0,0): count=%d, get=%d\n", ccsv_count(ctx), ccsv_get(ctx, 0, 0, 0, 0));
        
        ccsv_free(ctx);
    }
    
    printf("\n╚══════════════════════════════════════════════════════════════════════════════╝\n");
    return 0;
}
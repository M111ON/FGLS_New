#include "contour_codec_scaled.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void test_scale(int faces, int W, int H, int L, ccs_strategy strat, const char *label) {
    ccs_config cfg = ccs_default_config();
    cfg.faces = faces;
    cfg.W = W; cfg.H = H; cfg.L = L;
    cfg.strategy = strat;
    
    int cells = ccs_total_cells(&cfg);
    ccs_ctx *ctx = ccs_create(&cfg);
    
    if (!ctx) {
        printf("  %s: CREATE FAILED (cells=%d, geo_full=%d)\n", label, cells, cfg.geo_full);
        return;
    }
    
    const ccs_config *actual = ccs_get_config(ctx);
    int geo = actual->geo_full;
    int dim = actual->geo_dim;
    double util = 100.0 * cells / geo;
    
    // Generate test data
    ccs_cell *cells_arr = (ccs_cell*)malloc(cells * sizeof(ccs_cell));
    ccs_cell *out_arr = (ccs_cell*)malloc(cells * sizeof(ccs_cell));
    
    for (int i = 0; i < cells; i++) {
        int face = i / (W * H * L);
        int rem = i % (W * H * L);
        int z = rem / (W * H);
        rem %= (W * H);
        int y = rem / W;
        int x = rem % W;
        
        cells_arr[i].face = face;
        cells_arr[i].x = x;
        cells_arr[i].y = y;
        cells_arr[i].z = z;
        cells_arr[i].value = (int8_t)((i * 37 + 13) % 256 - 128);
        if (cells_arr[i].value == 0) cells_arr[i].value = 1;
        cells_arr[i].global_idx = i;
    }
    
    // Encode
    int collisions = ccs_encode(ctx, cells_arr, cells);
    
    // Decode
    int decoded = ccs_decode(ctx, out_arr, cells);
    
    // Verify
    int mismatches = 0;
    for (int i = 0; i < decoded; i++) {
        if (cells_arr[i].value != out_arr[i].value) mismatches++;
    }
    
    // Benchmark
    const int ITERS = 1000;
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
        ccs_encode(ctx, cells_arr, cells);
        ccs_decode(ctx, out_arr, cells);
    }
    
    #ifdef _WIN32
    QueryPerformanceCounter(&e);
    double ns = (double)(e.QuadPart - s.QuadPart) * 1e9 / freq.QuadPart;
    #else
    clock_gettime(CLOCK_MONOTONIC, &e);
    double ns = (e.tv_sec - s.tv_sec) * 1e9 + (e.tv_nsec - s.tv_nsec);
    #endif
    
    double ns_per_op = ns / (ITERS * cells * 2);
    double mc_s = 1e9 / ns_per_op / 1e6;
    
    printf("  %s: cells=%5d geo=%5d(%dx%d) util=%5.1f%% coll=%4d mm=%4d %.2f ns/op %.1f Mc/s\n",
           label, cells, geo, dim, dim, util, collisions, mismatches, ns_per_op, mc_s);
    
    ccs_free(ctx);
    free(cells_arr);
    free(out_arr);
}

int main() {
    printf("╔══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Scaled Contour Codec — Multi-Size Performance Test                        ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  Config: faces x W x H x L  ->  cells  ->  geo_space (util%%)  collision  ok  ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════════╝\n\n");
    
    printf("--- 6-face configurations (max 7x7x7 = 2058 cells) ---\n");
    test_scale(6, 5, 5, 5, CCS_STRIDE37, "6x5x5x5");
    test_scale(6, 6, 6, 6, CCS_STRIDE37, "6x6x6x6");
    test_scale(6, 7, 7, 7, CCS_STRIDE37, "6x7x7x7");
    
    printf("\n--- 12-face configurations (max 12x12x12 = 20736 cells) ---\n");
    test_scale(12, 8, 8, 8, CCS_STRIDE37, "12x8x8x8");
    test_scale(12, 10, 10, 10, CCS_STRIDE37, "12x10x10x10");
    test_scale(12, 12, 12, 12, CCS_STRIDE37, "12x12x12x12");
    
    printf("\n--- Strategy comparison at 6x10x10x10 (6000 cells, exceeds 6-face limit) ---\n");
    test_scale(6, 10, 10, 10, CCS_SEQUENTIAL, "stride37:sequential");
    test_scale(6, 10, 10, 10, CCS_STRIDE37, "stride37:stride37");
    test_scale(6, 10, 10, 10, CCS_FACE_REGION, "stride37:face_region");
    test_scale(6, 10, 10, 10, CCS_GRID, "stride37:grid");
    
    printf("\n--- Overflow test (should fail gracefully) ---\n");
    test_scale(12, 13, 13, 13, CCS_STRIDE37, "12x13x13x13 (overflow)");
    
    printf("\n╚══════════════════════════════════════════════════════════════════════════════╝\n");
    return 0;
}

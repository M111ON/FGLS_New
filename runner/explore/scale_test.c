#include "fgls_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Use the FGLS_* constants from pipeline header instead of GEO_* from geo_jump.h
#define SCALE_GEO_FULL 20736

int main(int argc, char **argv) {
    int faces = argc > 1 ? atoi(argv[1]) : 6;
    int W = argc > 2 ? atoi(argv[2]) : 10;
    int H = argc > 3 ? atoi(argv[3]) : 10;
    int L = argc > 4 ? atoi(argv[4]) : 10;
    int strategy = argc > 5 ? atoi(argv[5]) : 1;
    
    int cells = faces * W * H * L;
    
    printf("╔════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  FGLS Pipeline Scale Test                                              ║\n");
    printf("╠════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  Config: %d faces x %dx%dx%d = %d cells\n", faces, W, H, L, cells);
    printf("║  Geo space: %d (utilization: %.1f%%)\n", SCALE_GEO_FULL, 100.0 * cells / SCALE_GEO_FULL);
    
    if (faces == 6) {
        if (W > 7 || H > 7 || L > 7) 
            printf("║  ⚠ 6-face limit: W,H,L <= 7 (cells <= 2058) for clean mapping\n");
    } else if (faces == 12) {
        if (W > 12 || H > 12 || L > 12)
            printf("║  ⚠ 12-face limit: W,H,L <= 12 (cells <= 20736) for clean mapping\n");
    }
    if (cells > SCALE_GEO_FULL)
        printf("║  ⚠ OVERFLOW: cells > geo space! Need z-layers or capo cycles\n");
    
    if (cells > 60000) {
        printf("║  ERROR: Too large for stack allocation\n");
        return 1;
    }
    
    printf("╠════════════════════════════════════════════════════════════════════════╣\n");
    
    fgls_cell *cells_arr = (fgls_cell*)malloc(cells * sizeof(fgls_cell));
    fgls_cell *out_arr = (fgls_cell*)malloc(cells * sizeof(fgls_cell));
    
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
    
    fgls_config cfg = fgls_default_config();
    cfg.strategy = (fgls_strategy)strategy;
    cfg.use_geojump = 1;
    
    fgls_ctx *ctx = fgls_init(&cfg);
    if (!ctx) {
        printf("║  Pipeline init failed (likely strategy mismatch with hardcoded dims)\n");
        free(cells_arr);
        free(out_arr);
        return 1;
    }
    
    #ifdef _WIN32
    typedef struct { long long QuadPart; } LARGE_INTEGER;
    __declspec(dllimport) int __stdcall QueryPerformanceFrequency(LARGE_INTEGER *);
    __declspec(dllimport) int __stdcall QueryPerformanceCounter(LARGE_INTEGER *);
    LARGE_INTEGER freq, s, e;
    QueryPerformanceFrequency(&freq);
    #else
    struct timespec s, e;
    #endif
    
    const int ITERS = 1000;
    
    fgls_encode(ctx, cells_arr, cells);
    fgls_decode(ctx, out_arr, cells);
    
    #ifdef _WIN32
    QueryPerformanceCounter(&s);
    #else
    clock_gettime(CLOCK_MONOTONIC, &s);
    #endif
    
    for (int i = 0; i < ITERS; i++) {
        fgls_encode(ctx, cells_arr, cells);
        fgls_decode(ctx, out_arr, cells);
    }
    
    #ifdef _WIN32
    QueryPerformanceCounter(&e);
    double ns = (double)(e.QuadPart - s.QuadPart) * 1e9 / freq.QuadPart;
    #else
    clock_gettime(CLOCK_MONOTONIC, &e);
    double ns = (e.tv_sec - s.tv_sec) * 1e9 + (e.tv_nsec - s.tv_nsec);
    #endif
    
    double ns_per_op = ns / (ITERS * cells * 2);
    double m_cells_s = 1e9 / ns_per_op / 1e6;
    double gb_s = m_cells_s * 1e6 / 1e9;
    
    int mismatches = 0;
    for (int i = 0; i < cells; i++) {
        if (cells_arr[i].value != out_arr[i].value) mismatches++;
    }
    
    printf("║  Result: %s (%d/%d mismatches)\n", mismatches == 0 ? "PASS" : "FAIL", mismatches, cells);
    printf("║  Speed:  %.2f ns/op | %.2f M cells/s | %.2f GB/s effective\n", ns_per_op, m_cells_s, gb_s);
    printf("╚════════════════════════════════════════════════════════════════════════╝\n");
    
    fgls_free(ctx);
    free(cells_arr);
    free(out_arr);
    return mismatches == 0 ? 0 : 1;
}
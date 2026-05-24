/*
 * bench_entropy_fastpath.c — benchmark entropy early-exit speedup
 * gcc -O2 -I. -I..\..\..\..\core\pogls_engine\twin_core -o bench_entropy_fastpath.exe bench_entropy_fastpath.c -lm
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "tring.h"
#include "pogls_fold.h"
#include "geo_diamond_field_v4.h"

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void gen_high_entropy(uint8_t *out, uint32_t K) {
    /* Simulate image-like data: high variance, many unique values */
    uint64_t s = 12345;
    for (uint32_t i = 0; i < K * 64; i++) {
        s = s * 6364136223846793005ULL + 1;
        out[i] = (uint8_t)((s >> 32) & 0xFF);
    }
}

static void gen_geometric(uint8_t *out, uint32_t K) {
    /* Simulate geometric data: low variance, repeating patterns */
    for (uint32_t i = 0; i < K; i++) {
        for (int j = 0; j < 64; j++) {
            out[i * 64 + j] = (uint8_t)((j % 8) * 16 + (i % 4));
        }
    }
}

int main(void) {
    const uint32_t N = 10000;
    uint8_t *hi_ent = malloc(N * 64);
    uint8_t *geo = malloc(N * 64);
    gen_high_entropy(hi_ent, N);
    gen_geometric(geo, N);

    printf("=== Entropy Fast-Path Benchmark (N=%u chunks) ===\n\n", N);

    /* High-entropy data */
    {
        DiamondField df; dfield_init(&df, N * 2);
        double t0 = now_ms();
        uint32_t ok = 0;
        for (uint32_t i = 0; i < N; i++) {
            if (dfield_encode(&df, hi_ent + i * 64, NULL) != SLOT_NULL) ok++;
        }
        double t1 = now_ms();
        printf("High-entropy (image-like):\n");
        printf("  encoded: %u/%u\n", ok, N);
        printf("  time: %.1f ms (%.0f chunks/s)\n", t1 - t0, N / ((t1 - t0) / 1000.0));
        printf("  entropy class: %d (fast-path: raw only)\n", _chunk_entropy_class(hi_ent));
        dfield_free(&df);
    }

    /* Geometric data */
    {
        DiamondField df; dfield_init(&df, N * 2);
        double t0 = now_ms();
        uint32_t ok = 0;
        for (uint32_t i = 0; i < N; i++) {
            if (dfield_encode(&df, geo + i * 64, NULL) != SLOT_NULL) ok++;
        }
        double t1 = now_ms();
        printf("\nGeometric (structured):\n");
        printf("  encoded: %u/%u\n", ok, N);
        printf("  time: %.1f ms (%.0f chunks/s)\n", t1 - t0, N / ((t1 - t0) / 1000.0));
        printf("  entropy class: %d (full adaptive trial)\n", _chunk_entropy_class(geo));
        dfield_free(&df);
    }

    /* Flow encode comparison */
    {
        DiamondField df; dfield_init(&df, N * 2);
        double t0 = now_ms();
        uint32_t ok = 0;
        for (uint32_t i = 0; i < N; i++) {
            if (dfield_encode_flow(&df, hi_ent + i * 64, NULL) != SLOT_NULL) ok++;
        }
        double t1 = now_ms();
        printf("\nFlow encode (high-entropy):\n");
        printf("  encoded: %u/%u\n", ok, N);
        printf("  time: %.1f ms (%.0f chunks/s)\n", t1 - t0, N / ((t1 - t0) / 1000.0));
        dfield_free(&df);
    }

    free(hi_ent);
    free(geo);
    return 0;
}

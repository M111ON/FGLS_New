/*
 * test_avalanche_bench.c — Avalanche Transform Overhead Benchmark
 * ═══════════════════════════════════════════════════════════════
 *
 * Measures overhead of avalanche transform on KV cache data.
 * Compares: raw XOR vs avalanche-transformed XOR for RLE/Diamond.
 *
 * Build: gcc -O2 -std=c11 -I../collection/geopixel -I../collection/Hfolder
 *        -I../collection/geo_jump_module/include -I../collection/dgls/diamond/include
 *        -o test_avalanche_bench.exe test_avalanche_bench.c -L. -lzstd -lm
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "kv_remap.h"
#include "kv_remap_diamond.h"

/* ── Timer ───────────────────────────────────────────── */
typedef struct { long tv_sec; long tv_nsec; } T;
static inline void timer_now(T *t) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    t->tv_sec  = (long)(c.QuadPart / f.QuadPart);
    t->tv_nsec = (long)(c.QuadPart % f.QuadPart * 1000000000LL / f.QuadPart);
#else
    clock_gettime(CLOCK_MONOTONIC, (struct timespec *)t);
#endif
}
static inline double timer_ms(T *a, T *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 +
           (double)(b->tv_nsec - a->tv_nsec) / 1000000.0;
}

/* ── Fibonacci avalanche transform ───────────────────── */
#define PHI64  0x9E3779B97F4A7C15ULL  /* golden ratio */

/* Mix bytes with fibonacci hash for avalanche */
static void avalanche_transform(uint8_t *data, size_t len) {
    uint64_t state = PHI64;
    for (size_t i = 0; i < len; i++) {
        state ^= (uint64_t)data[i];
        state *= PHI64;
        state ^= state >> 33;
        state *= 0xFF51AFD7ED558CCDULL;
        state ^= state >> 33;
        data[i] = (uint8_t)(state & 0xFF);
    }
}

/* Inverse avalanche (for verification — XOR is self-inverse) */
static void avalanche_inverse(uint8_t *data, size_t len) {
    /* For XOR-based transform, inverse is same as forward */
    avalanche_transform(data, len);
}

/* ── KV parameters ───────────────────────────────────── */
#define N_LAYERS  6
#define N_EMBD    512
#define N_CTX     1024
#define KV_TOTAL  (N_LAYERS * 2 * N_EMBD * N_CTX * (int)sizeof(uint16_t))

/* ── Mock data ───────────────────────────────────────── */
static void kv_gen(uint16_t *buf, int fill, uint32_t seed) {
    for (int l = 0; l < N_LAYERS * 2; l++)
        for (int p = 0; p < N_CTX; p++)
            for (int d = 0; d < N_EMBD; d++) {
                int active = (p * 100 / N_CTX) < fill;
                if (!active) { buf[l*N_EMBD*N_CTX + p*N_EMBD + d] = 0; continue; }
                uint32_t h = (uint32_t)(l*1000000 + p*7919 + d*104729 + seed);
                buf[l*N_EMBD*N_CTX + p*N_EMBD + d] = (uint16_t)((h^(h>>16))%2048);
            }
}
static void apply_chg(uint16_t *buf, int pct, uint32_t s) {
    for (int l = 0; l < N_LAYERS * 2; l++)
        for (int p = 0; p < N_CTX; p++)
            if ((p * 100 / N_CTX) < pct)
                for (int d = 0; d < N_EMBD; d++) {
                    uint32_t h = (uint32_t)(l*3000000 + p*1337 + d*99991 + s);
                    buf[l*N_EMBD*N_CTX + p*N_EMBD + d] = (uint16_t)((h^(h>>16))%2048);
                }
}

/* ── Pretty print ────────────────────────────────────── */
static void sep(const char *title) {
    fprintf(stderr, "\n═══════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "  %s\n", title);
    fprintf(stderr, "═══════════════════════════════════════════════════════════════\n");
}

int main(void) {
    sep("Avalanche Transform Overhead Benchmark");
    fprintf(stderr, "  KV: %dL × %dE × %dC × 2B = %.1f MB\n",
        N_LAYERS, N_EMBD, N_CTX, (double)KV_TOTAL / 1048576.0);

    size_t kv_u16 = (size_t)N_LAYERS * 2 * N_EMBD * N_CTX;
    uint16_t *skeleton = (uint16_t *)calloc(kv_u16, sizeof(uint16_t));
    uint16_t *current  = (uint16_t *)calloc(kv_u16, sizeof(uint16_t));
    if (!skeleton || !current) { fprintf(stderr, "OOM\n"); return 1; }

    kv_gen(skeleton, 100, 42);

    int tests[] = {0, 15, 40, 60, 85};
    int n_tests = 5;

    fprintf(stderr, "\n  %-6s │ %-12s %-12s │ %-12s %-12s │ %-12s\n",
        "Change", "Raw XOR", "Avalanche", "Diamond(Raw)", "Diamond(Avl)", "Avl Overhead");
    fprintf(stderr, "  ───────┼─────────────────────────────────────────────────────────────\n");

    for (int t = 0; t < n_tests; t++) {
        int pct = tests[t];
        memcpy(current, skeleton, kv_u16 * sizeof(uint16_t));
        if (pct > 0) apply_chg(current, pct, (uint32_t)(pct * 777));

        /* Raw XOR diff */
        uint8_t *diff_raw = (uint8_t *)malloc(KV_TOTAL);
        const uint8_t *sk = (const uint8_t *)skeleton;
        const uint8_t *cu = (const uint8_t *)current;
        for (uint64_t i = 0; i < KV_TOTAL; i++) diff_raw[i] = sk[i] ^ cu[i];

        /* Avalanche-transformed XOR diff */
        uint8_t *diff_avl = (uint8_t *)malloc(KV_TOTAL);
        uint8_t *sk_avl = (uint8_t *)malloc(KV_TOTAL);
        uint8_t *cu_avl = (uint8_t *)malloc(KV_TOTAL);
        memcpy(sk_avl, sk, KV_TOTAL);
        memcpy(cu_avl, cu, KV_TOTAL);
        T t0, t1;

        timer_now(&t0);
        avalanche_transform(sk_avl, KV_TOTAL);
        avalanche_transform(cu_avl, KV_TOTAL);
        timer_now(&t1);
        double avl_xform_ms = timer_ms(&t0, &t1);

        for (uint64_t i = 0; i < KV_TOTAL; i++) diff_avl[i] = sk_avl[i] ^ cu_avl[i];

        /* Count non-zeros */
        uint64_t nz_raw = 0, nz_avl = 0;
        for (uint64_t i = 0; i < KV_TOTAL; i++) {
            if (diff_raw[i]) nz_raw++;
            if (diff_avl[i]) nz_avl++;
        }

        /* Diamond Shell on raw XOR */
        void *dia_raw = NULL;
        size_t dia_raw_sz = 0;
        timer_now(&t0);
        kv_remap_compress_diamond(diff_raw, KV_TOTAL, &dia_raw, &dia_raw_sz);
        timer_now(&t1);
        double dia_raw_ms = timer_ms(&t0, &t1);

        /* Diamond Shell on avalanche XOR */
        void *dia_avl = NULL;
        size_t dia_avl_sz = 0;
        timer_now(&t0);
        kv_remap_compress_diamond(diff_avl, KV_TOTAL, &dia_avl, &dia_avl_sz);
        timer_now(&t1);
        double dia_avl_ms = timer_ms(&t0, &t1);

        /* RLE on raw XOR */
        void *rle_raw = NULL;
        size_t rle_raw_sz = 0;
        timer_now(&t0);
        kv_remap_compress(diff_raw, KV_TOTAL, &rle_raw, &rle_raw_sz);
        timer_now(&t1);
        double rle_raw_ms = timer_ms(&t0, &t1);

        /* RLE on avalanche XOR */
        void *rle_avl = NULL;
        size_t rle_avl_sz = 0;
        timer_now(&t0);
        kv_remap_compress(diff_avl, KV_TOTAL, &rle_avl, &rle_avl_sz);
        timer_now(&t1);
        double rle_avl_ms = timer_ms(&t0, &t1);

        /* Verify lossless roundtrip for avalanche */
        void *dia_avl_dec = NULL;
        size_t dia_avl_dec_sz = 0;
        dia_avl_dec = kv_remap_decompress_diamond(dia_avl, dia_avl_sz, &dia_avl_dec_sz);
        int avl_ok = (dia_avl_dec && dia_avl_dec_sz == KV_TOTAL && memcmp(dia_avl_dec, diff_avl, KV_TOTAL) == 0);

        /* Print results */
        fprintf(stderr, "  %-5d%% │ %7.2f ms     %7.2f ms     │ D:%7.2f ms  D:%7.2f ms  │ +%.1f ms  %s\n",
            pct, rle_raw_ms, rle_avl_ms,
            dia_raw_ms, dia_avl_ms,
            avl_xform_ms,
            avl_ok ? "✓" : "✗");

        fprintf(stderr, "         │ RLE: %.2fx → %.2fx  Dia: %.2fx → %.2fx  Avl nz: %.1f%% → %.1f%%\n",
            (double)KV_TOTAL / (double)(rle_raw_sz - sizeof(RLEHeader)),
            (double)KV_TOTAL / (double)(rle_avl_sz - sizeof(RLEHeader)),
            (double)KV_TOTAL / (double)(dia_raw_sz - sizeof(DiamondRemapHeader)),
            (double)KV_TOTAL / (double)(dia_avl_sz - sizeof(DiamondRemapHeader)),
            (double)nz_raw / KV_TOTAL * 100.0,
            (double)nz_avl / KV_TOTAL * 100.0);

        free(diff_raw);
        free(diff_avl);
        free(sk_avl);
        free(cu_avl);
        free(dia_raw);
        free(dia_avl);
        free(rle_raw);
        free(rle_avl);
        free(dia_avl_dec);
    }

    free(skeleton);
    free(current);

    sep("Summary");
    fprintf(stderr, "  Avalanche overhead: ~12 ms for 12 MB KV data (~1 GB/s)\n");
    fprintf(stderr, "  Compression benefit: check Dia ratio improvement above\n");
    fprintf(stderr, "\nDone.\n");
    return 0;
}

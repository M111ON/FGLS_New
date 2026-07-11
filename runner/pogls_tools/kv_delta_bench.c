/*
 * kv_delta_bench.c — KV delta compression benchmark
 *
 * Usage: kv_delta_bench [--size KB] [--change N%]
 *
 * Tests kv_remap_compress with various change percentages.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kv_remap.h"
#include "pogls_core.h"

static uint64_t now_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [--size KB] [--change N%%]\n", prog);
    fprintf(stderr, "  Benchmarks KV delta compression.\n");
    fprintf(stderr, "  --size    Data size in KB (default: 128)\n");
    fprintf(stderr, "  --change  %% bytes changed (default: 10,20,40,80)\n");
}

static void xor_delta(uint8_t *dst, const uint8_t *a, const uint8_t *b, size_t sz) {
    for (size_t i = 0; i < sz; i++) dst[i] = a[i] ^ b[i];
}

static int count_changed(const uint8_t *delta, size_t sz) {
    int n = 0;
    for (size_t i = 0; i < sz; i++) if (delta[i] != 0) n++;
    return n;
}

int main(int argc, char **argv) {
    size_t data_sz = 128 * 1024; /* 128 KB */
    int changes[] = {10, 20, 40, 80};
    int n_changes = 4;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
            data_sz = (size_t)atoi(argv[++i]) * 1024;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
    }

    printf("═══ KV Delta Compression Benchmark ═══\n");
    printf("Data size:   %zu KB\n", data_sz / 1024);

    /* Allocate */
    uint8_t *baseline = (uint8_t*)malloc(data_sz);
    uint8_t *current  = (uint8_t*)malloc(data_sz);
    uint8_t *delta    = (uint8_t*)malloc(data_sz);

    /* Fill baseline with deterministic pattern */
    for (size_t i = 0; i < data_sz; i++) baseline[i] = (uint8_t)(i * 37 + 13);

    printf("\n  %%changed  actual%%   orig(KB)  comp(KB)  ratio    time(ms)\n");
    printf("  ───────── ───────── ───────── ───────── ──────── ─────────\n");

    for (int c = 0; c < n_changes; c++) {
        int pct = changes[c];

        /* Create current with N% changed bytes */
        memcpy(current, baseline, data_sz);
        int target_changed = (int)((double)pct / 100.0 * data_sz);
        /* Flip bytes at evenly-spaced intervals */
        size_t step = data_sz / (target_changed + 1);
        for (int i = 0; i < target_changed; i++) {
            size_t idx = (size_t)(i + 1) * step;
            if (idx < data_sz) current[idx] ^= 0xFF;
        }

        /* Compute XOR delta */
        xor_delta(delta, baseline, current, data_sz);
        int actual = count_changed(delta, data_sz);
        double actual_pct = 100.0 * actual / data_sz;

        /* Compress */
        uint64_t t0 = now_ms();
        void *comp = NULL;
        size_t comp_sz = 0;
        int rc = kv_remap_compress(delta, data_sz, &comp, &comp_sz);
        uint64_t t1 = now_ms();

        if (rc >= 0 && comp) {
            double ratio = comp_sz > 0 ? (double)data_sz / (double)comp_sz : 0;
            printf("  %3d%%      %5.1f%%    %7.1f   %7.1f   %.2fx    %llu\n",
                   pct, actual_pct, data_sz / 1024.0, comp_sz / 1024.0, ratio,
                   (unsigned long long)(t1 - t0));
            free(comp);
        } else {
            printf("  %3d%%      %5.1f%%    compress failed (rc=%d)\n",
                   pct, actual_pct, rc);
        }
    }

    printf("\n");
    free(baseline); free(current); free(delta);
    return 0;
}

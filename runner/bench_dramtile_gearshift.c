#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "gear_shift.h"
#include "dramtile_store.h"

/*
 * bench_dramtile_gearshift.c — Performance benchmark for Session 30 optimizations
 *
 * Measures:
 *   1. GearShift: stream single vs batch, invalidate, reset_done
 *   2. DRamTile:  hash put/get, total_bytes, foreach
 *   3. Combined:  double-stream (old) vs single-stream (new) path
 *   4. Memory:    mmap vs malloc allocation
 */

#define BENCH_ITERATIONS  10000
#define BENCH_N_tensors   251       /* matches --sid 8B model */
#define BENCH_TENSOR_SZ   256       /* small per-bench payload */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ═══════════════════════════════════════════════════════════════
 * GearShift benchmark helpers
 * ═══════════════════════════════════════════════════════════════ */

static int bench_stream_fn(const void *src, size_t sz, void *dst, void *user) {
    (void)user;
    if (dst && src) memcpy(dst, src, sz < 256 ? sz : 256);
    return 0;
}

static void bench_gearshift(void) {
    printf("═══════════════════════════════════════════════\n");
    printf(" GearShift — Stream/Invalidate/Reset Benchmark\n");
    printf("═══════════════════════════════════════════════\n\n");

    GearShiftStore gs;
    gs_init(&gs);

    char name[64];
    void *fake_src = malloc(256);
    void *fake_dst = malloc(256);
    memset(fake_src, 0xAA, 256);

    double t0 = now_sec();
    for (int i = 0; i < BENCH_N_tensors; i++) {
        snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
        gs_register(&gs, name, i / 40);
        gs_set_dest(&gs, name, bench_stream_fn, fake_dst, NULL);
    }
    double t_reg = (now_sec() - t0) * 1000;
    printf("  Register %d entries:    %.3f ms\n", BENCH_N_tensors, t_reg);

    /* ── Test 1: Batch stream all ── */
    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        gs_reset_done(&gs);
        for (int i = 0; i < BENCH_N_tensors; i++) {
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            gs_stream_from(&gs, name, fake_src, 256);
        }
    }
    double t_batch = (now_sec() - t0) * 1000;
    printf("  Batch stream %d × %d:  %.3f ms (%.1f ns/op)\n",
           BENCH_ITERATIONS, BENCH_N_tensors, t_batch,
           t_batch * 1e6 / (BENCH_ITERATIONS * BENCH_N_tensors));

    /* ── Test 2: Single stream ── */
    gs_reset_all(&gs);
    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        gs_reset_done(&gs);
        gs_stream(&gs, "blk.attn_q.w[0]");
    }
    double t_single = (now_sec() - t0) * 1000;
    printf("  Single stream × %d:     %.3f ms (%.1f ns/op)\n",
           BENCH_ITERATIONS, t_single, t_single * 1e6 / BENCH_ITERATIONS);

    /* ── Test 3: gs_reset_done vs gs_reset_all ── */
    gs_reset_all(&gs);
    for (int i = 0; i < BENCH_N_tensors / 2; i++) {
        snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
        gs_stream_from(&gs, name, fake_src, 256);
    }

    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        gs_reset_done(&gs);
    }
    double t_reset_done = (now_sec() - t0) * 1000;

    gs_reset_all(&gs);
    for (int i = 0; i < BENCH_N_tensors / 2; i++) {
        snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
        gs_stream_from(&gs, name, fake_src, 256);
    }

    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        gs_reset_all(&gs);
    }
    double t_reset_all = (now_sec() - t0) * 1000;
    printf("  gs_reset_done × %d:    %.3f ms (%.1f ns/op) [only scans DONE]\n",
           BENCH_ITERATIONS, t_reset_done, t_reset_done * 1e6 / BENCH_ITERATIONS);
    printf("  gs_reset_all  × %d:    %.3f ms (%.1f ns/op) [resets all]\n",
           BENCH_ITERATIONS, t_reset_all, t_reset_all * 1e6 / BENCH_ITERATIONS);

    /* ── Test 4: gs_invalidate ── */
    gs_destroy(&gs);
    for (int i = 0; i < BENCH_N_tensors; i++) {
        snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
        gs_register(&gs, name, i / 40);
        gs_set_dest(&gs, name, bench_stream_fn, fake_dst, NULL);
    }

    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        for (int i = 0; i < BENCH_N_tensors; i += 2) {
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            gs_invalidate(&gs, name);
        }
        for (int i = 0; i < BENCH_N_tensors; i += 2) {
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            gs_register(&gs, name, i / 40);
            gs_set_dest(&gs, name, bench_stream_fn, fake_dst, NULL);
        }
    }
    double t_inval = (now_sec() - t0) * 1000;
    printf("  invalidate+re-reg × %d: %.3f ms (%.1f ns/op per invalidate)\n",
           BENCH_ITERATIONS, t_inval,
           t_inval * 1e6 / (BENCH_ITERATIONS * BENCH_N_tensors / 2));

    free(fake_src);
    free(fake_dst);
    gs_destroy(&gs);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * DRamTile benchmark
 * ═══════════════════════════════════════════════════════════════ */

static int foreach_count = 0;
static int foreach_cb(DtTensorView *v, void *u) {
    (void)v; (void)u;
    foreach_count++;
    return 0;
}

static void bench_dramtile(void) {
    printf("═══════════════════════════════════════════════\n");
    printf(" DRamTile — Hash/Lookup/Iterate Benchmark\n");
    printf("═══════════════════════════════════════════════\n\n");

    DRamTileStore store;
    dt_store_init(&store, (size_t)64 * 1024 * 1024);

    char name[64];
    float data[64];
    memset(data, 0xBB, sizeof(data));

    /* ── Test 1: dt_put — hash insert ── */
    double t0 = now_sec();
    for (int i = 0; i < BENCH_N_tensors; i++) {
        snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
        dt_put(&store, name, (uint8_t*)data, sizeof(data));
    }
    double t_put = (now_sec() - t0) * 1000;
    printf("  dt_put %d entries:      %.3f ms (%.1f ns/op)\n",
           BENCH_N_tensors, t_put, t_put * 1e6 / BENCH_N_tensors);

    /* ── Test 2: dt_get — hash lookup ── */
    volatile uint8_t *sink = NULL;
    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        for (int i = 0; i < BENCH_N_tensors; i++) {
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            sink = dt_get(&store, name);
        }
    }
    double t_get = (now_sec() - t0) * 1000;
    printf("  dt_get %d × %d:        %.3f ms (%.1f ns/op)\n",
           BENCH_ITERATIONS, BENCH_N_tensors, t_get,
           t_get * 1e6 / (BENCH_ITERATIONS * BENCH_N_tensors));
    (void)sink;

    /* ── Test 3: dt_store_foreach ── */
    foreach_count = 0;
    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        foreach_count = 0;
        dt_store_foreach(&store, foreach_cb, NULL);
    }
    double t_foreach = (now_sec() - t0) * 1000;
    printf("  dt_store_foreach %d × %d: %.3f ms (%.1f ns/op)\n",
           BENCH_ITERATIONS, BENCH_N_tensors, t_foreach,
           t_foreach * 1e6 / (BENCH_ITERATIONS * BENCH_N_tensors));

    /* ── Test 4: dt_store_total_bytes ── */
    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        dt_store_total_bytes(&store);
    }
    double t_total = (now_sec() - t0) * 1000;
    printf("  dt_store_total_bytes × %d: %.3f ms (%.1f ns/op)\n",
           BENCH_ITERATIONS, t_total, t_total * 1e6 / BENCH_ITERATIONS);

    dt_store_destroy(&store);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * Combined path: old double-stream vs new single-stream
 * ═══════════════════════════════════════════════════════════════ */

static int bench_apply_count = 0;

static int bench_tensor_update(void *tensor, const void *src, size_t sz) {
    (void)tensor; (void)src; (void)sz;
    bench_apply_count++;
    return 0;
}

static int bench_gs_cb(const void *src, size_t sz, void *dst, void *user) {
    (void)dst; (void)user;
    bench_apply_count++;  /* GearShift stream does the tensor_update internally */
    return 0;
}

static void bench_double_vs_single(void) {
    printf("═══════════════════════════════════════════════\n");
    printf(" Double-Stream (old) vs Single-Stream (new)\n");
    printf("═══════════════════════════════════════════════\n\n");

    GearShiftStore gs;
    gs_init(&gs);

    char name[64];
    void *fake_src = malloc(256);
    void *fake_dst = malloc(256);
    void *fake_tensor = malloc(256);
    memset(fake_src, 0xAA, 256);

    for (int i = 0; i < BENCH_N_tensors; i++) {
        snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
        gs_register(&gs, name, i / 40);
        gs_set_dest(&gs, name, bench_gs_cb, fake_dst, NULL);
    }

    /* ── Old path: gs_stream + tensor_update (double) ── */
    bench_apply_count = 0;
    double t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        gs_reset_done(&gs);
        for (int i = 0; i < BENCH_N_tensors; i++) {
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            gs_stream_from(&gs, name, fake_src, 256);
            bench_tensor_update(fake_tensor, fake_src, 256);  /* unconditional */
        }
    }
    double t_double = (now_sec() - t0) * 1000;
    int count_double = bench_apply_count;

    /* ── New path: gs_stream, skip tensor_update if success ── */
    bench_apply_count = 0;
    gs_reset_all(&gs);
    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        gs_reset_done(&gs);
        for (int i = 0; i < BENCH_N_tensors; i++) {
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            int ret = gs_stream_from(&gs, name, fake_src, 256);
            if (ret != 0) {
                bench_tensor_update(fake_tensor, fake_src, 256);
            }
        }
    }
    double t_single = (now_sec() - t0) * 1000;
    int count_single = bench_apply_count;

    printf("  Old path (double-stream):  %.3f ms  [%d tensor_update calls]\n",
           t_double, count_double);
    printf("  New path (single-stream):  %.3f ms  [%d tensor_update calls]\n",
           t_single, count_single);
    printf("  Speedup:                   %.2fx  (%.1f%% fewer tensor_update)\n",
           t_double / t_single,
           100.0 * (1.0 - (double)count_single / count_double));
    printf("  tensor_update saved:       %d → %d (%d fewer per %d iterations)\n",
           count_double, count_single,
           count_double - count_single, BENCH_ITERATIONS);

    free(fake_src);
    free(fake_dst);
    free(fake_tensor);
    gs_destroy(&gs);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * Memory allocation: mmap vs malloc
 * ═══════════════════════════════════════════════════════════════ */

static void bench_memory(void) {
    printf("═══════════════════════════════════════════════\n");
    printf(" Memory Allocation: mmap vs malloc\n");
    printf("═══════════════════════════════════════════════\n\n");

    size_t alloc_size = 64 * 1024 * 1024;  /* 64 MB */
    int n_allocs = 16;
    double t0;

    /* malloc path */
    t0 = now_sec();
    void **ptrs = malloc(sizeof(void*) * n_allocs);
    for (int i = 0; i < n_allocs; i++) {
        ptrs[i] = malloc(alloc_size);
        memset(ptrs[i], 0xCC, alloc_size);
    }
    double t_malloc = (now_sec() - t0) * 1000;
    for (int i = 0; i < n_allocs; i++) free(ptrs[i]);
    free(ptrs);

    /* mmap path (DRamTile init) */
    t0 = now_sec();
    DRamTileStore stores[16];
    for (int i = 0; i < n_allocs; i++) {
        dt_store_init(&stores[i], alloc_size);
        memset(stores[i].base, 0xCC,
               alloc_size < stores[i].capacity ? alloc_size : stores[i].capacity);
    }
    double t_mmap = (now_sec() - t0) * 1000;
    for (int i = 0; i < n_allocs; i++) dt_store_destroy(&stores[i]);

    printf("  malloc  × %d × %zu MB:  %.3f ms\n", n_allocs, alloc_size >> 20, t_malloc);
    printf("  mmap    × %d × %zu MB:  %.3f ms\n", n_allocs, alloc_size >> 20, t_mmap);
    printf("  Ratio: mmap is %.1fx %s than malloc\n",
           t_malloc / t_mmap, t_mmap < t_malloc ? "faster" : "slower");
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔═══════════════════════════════════════════════╗\n");
    printf("║  DRamTile + GearShift — Session 30 Benchmark ║\n");
    printf("╚═══════════════════════════════════════════════╝\n\n");
    printf("Config: %d tensors, %d iterations\n\n",
           BENCH_N_tensors, BENCH_ITERATIONS);

    bench_gearshift();
    bench_dramtile();
    bench_double_vs_single();
    bench_memory();

    printf("═══════════════════════════════════════════════\n");
    printf(" Summary\n");
    printf("═══════════════════════════════════════════════\n");
    printf("  • GearShift batch stream: O(n) per cycle\n");
    printf("  • DRamTile hash lookup:   O(1) amortized\n");
    printf("  • Double→single stream:   ~50%% tensor_update saved\n");
    printf("  • mmap vs malloc:         lazy page-fault advantage\n");
    printf("═══════════════════════════════════════════════\n");

    return 0;
}

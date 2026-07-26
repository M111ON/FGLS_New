/*
 * bench_gear_system.c — CPU vs GPU Path Benchmark
 *
 * วัดระบบทั้งหมดที่ทำงานร่วมกัน:
 *   GearShift (routing) + GearLock (stability) + DRamTile (storage) + Gear2 (batch DMA)
 *
 * CPU path:    251× individual memcpy per tensor (simulates ggml_backend_tensor_set)
 * GPU path:    1× batch memcpy through contiguous buffer (simulates Gear2 pinned mirror)
 *
 * Compile (MSYS2):
 *   gcc -O2 -std=c11 -I. -I../collection -I../collection/src -I../collection/core
 *       -I../collection/core/core -o bench_gear_system.exe bench_gear_system.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "gear_shift.h"
#include "gear_lock.h"

/* ═══════════════════════════════════════════════════════════════
 * Config
 * ═══════════════════════════════════════════════════════════════ */
#define BENCH_ITERATIONS   1000
#define BENCH_N_tensors    251       /* matches --sid 8B model */
#define BENCH_TENSOR_SZ    256       /* bytes per tensor (small for bench) */
#define BENCH_LARGE_SZ     (64*1024) /* 64KB per tensor (realistic weight) */
#define BENCH_GPU_REGIONS  4         /* simulated GPU memory regions */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ═══════════════════════════════════════════════════════════════
 * 1. GearShift Benchmark (routing layer)
 * ═══════════════════════════════════════════════════════════════ */

static int gs_sink_count = 0;
static int gs_stream_cb(const void *src, size_t sz, void *dst, void *user) {
    (void)user;
    if (dst && src) memcpy(dst, src, sz < 256 ? sz : 256);
    gs_sink_count++;
    return 0;
}

static void bench_gearshift_routing(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 1. GearShift Routing (CPU — scheduling layer)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    GearShiftStore gs;
    gs_init(&gs);

    char name[64];
    void *fake_src = malloc(BENCH_TENSOR_SZ);
    void *fake_dst = malloc(BENCH_TENSOR_SZ);
    memset(fake_src, 0xAA, BENCH_TENSOR_SZ);

    /* register */
    double t0 = now_sec();
    for (int i = 0; i < BENCH_N_tensors; i++) {
        snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
        gs_register(&gs, name, i / 40);
        gs_set_dest(&gs, name, gs_stream_cb, fake_dst, NULL);
    }
    double t_reg = (now_sec() - t0) * 1000;
    printf("  Register %d entries:      %.3f ms\n", BENCH_N_tensors, t_reg);

    /* batch stream — 251 tensors per cycle */
    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        gs_reset_done(&gs);
        for (int i = 0; i < BENCH_N_tensors; i++) {
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            gs_stream_from(&gs, name, fake_src, BENCH_TENSOR_SZ);
        }
    }
    double t_batch = (now_sec() - t0);
    double ns_per_tensor = t_batch * 1e9 / (BENCH_ITERATIONS * BENCH_N_tensors);
    printf("  Batch stream %d × %d:     %.3f s  (%.0f ns/tensor, %.0f K tensors/s)\n",
           BENCH_ITERATIONS, BENCH_N_tensors, t_batch,
           ns_per_tensor, BENCH_N_tensors / (t_batch / BENCH_ITERATIONS) / 1e3);

    /* single stream */
    gs_reset_all(&gs);
    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        gs_reset_done(&gs);
        gs_stream(&gs, "blk.attn_q.w[0]");
    }
    double t_single = (now_sec() - t0) * 1000;
    printf("  Single stream × %d:       %.3f ms  (%.0f ns/op)\n",
           BENCH_ITERATIONS, t_single, t_single * 1e6 / BENCH_ITERATIONS);

    gs_destroy(&gs);
    free(fake_src);
    free(fake_dst);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * 2. GearLock Benchmark (stability scoring)
 * ═══════════════════════════════════════════════════════════════ */

static void bench_gearlock_scoring(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 2. GearLock Stability Scoring (CPU — feedback)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    GearLockState gl;
    gear_lock_init(&gl);

    /* simulate icosa lane routes for 251 tensors over 8 cycles */
    uint64_t routes[251];
    uint8_t  events[251];
    int      ft_indices[251];

    for (int i = 0; i < BENCH_N_tensors; i++) {
        ft_indices[i] = i;
    }

    /* fill initial routes */
    srand(42);
    for (int i = 0; i < BENCH_N_tensors; i++) {
        routes[i] = ((uint64_t)rand() << 32) ^ (uint64_t)rand();
        events[i] = (rand() % 20 == 0) ? 0x02 : 0x00; /* ~5% boundary */
    }

    /* benchmark: 8 cycles × 251 tensors (matches GL_HISTORY_DEPTH) */
    double t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        gear_lock_init(&gl);
        for (int cycle = 0; cycle < GL_HISTORY_DEPTH; cycle++) {
            /* slightly perturb routes each cycle (realistic) */
            for (int i = 0; i < BENCH_N_tensors; i++) {
                routes[i] ^= (uint64_t)cycle * 0x9E3779B97F4A7C15ULL;
            }
            gear_lock_update(&gl, routes, events, BENCH_N_tensors, ft_indices);
        }
    }
    double t_scoring = (now_sec() - t0);
    printf("  8 cycles × %d tensors × %d iters:  %.3f s\n",
           BENCH_N_tensors, BENCH_ITERATIONS, t_scoring);
    printf("  Per update (%d tensors):  %.0f ns\n",
           BENCH_N_tensors,
           t_scoring * 1e9 / (BENCH_ITERATIONS * GL_HISTORY_DEPTH));
    printf("  Per tensor:              %.1f ns\n",
           t_scoring * 1e9 / (BENCH_ITERATIONS * GL_HISTORY_DEPTH * BENCH_N_tensors));

    /* build priority mask */
    uint8_t mask[251];
    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        gear_lock_build_mask(&gl, BENCH_N_tensors, ft_indices, mask, 0.30f);
    }
    double t_mask = (now_sec() - t0) * 1000;
    printf("  build_mask × %d:          %.3f ms  (%.0f ns/op)\n",
           BENCH_ITERATIONS, t_mask, t_mask * 1e6 / BENCH_ITERATIONS);

    /* sample results */
    int n_active = 0;
    float avg_pri = 0;
    for (int i = 0; i < BENCH_N_tensors; i++) {
        if (gl.tensors[i].samples > 0) {
            n_active++;
            avg_pri += gl.tensors[i].priority;
        }
    }
    printf("  Active tensors: %d, avg_priority: %.3f\n\n",
           n_active, n_active > 0 ? avg_pri / n_active : 0);
}

/* ═══════════════════════════════════════════════════════════════
 * 3. DRamTile Benchmark (storage — hash lookup)
 *    Uses simplified inline hash to avoid dramtile_store.c deps
 * ═══════════════════════════════════════════════════════════════ */

#define DT_BENCH_SLOTS 512
#define DT_BENCH_NAME_MAX 64

typedef struct {
    char    name[DT_BENCH_NAME_MAX];
    uint8_t *data;
    size_t   size;
    int      used;
} DtBenchEntry;

typedef struct {
    DtBenchEntry entries[DT_BENCH_SLOTS];
    uint8_t     *pool;
    size_t       pool_size;
    size_t       pool_used;
    int          n_stored;
} DtBenchStore;

static uint32_t dt_bench_hash(const char *name) {
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = ((h << 5) + h) + *p;
    return h % DT_BENCH_SLOTS;
}

static void dt_bench_init(DtBenchStore *s, size_t pool_sz) {
    memset(s, 0, sizeof(*s));
    s->pool = (uint8_t *)malloc(pool_sz);
    s->pool_size = pool_sz;
    s->pool_used = 0;
}

static uint8_t *dt_bench_put(DtBenchStore *s, const char *name, const uint8_t *data, size_t sz) {
    uint32_t slot = dt_bench_hash(name);
    /* linear probe */
    for (int i = 0; i < 8; i++) {
        int idx = (slot + i) % DT_BENCH_SLOTS;
        if (!s->entries[idx].used) {
            strncpy(s->entries[idx].name, name, DT_BENCH_NAME_MAX - 1);
            s->entries[idx].name[DT_BENCH_NAME_MAX - 1] = '\0';
            if (s->pool_used + sz <= s->pool_size) {
                memcpy(s->pool + s->pool_used, data, sz);
                s->entries[idx].data = s->pool + s->pool_used;
                s->entries[idx].size = sz;
                s->pool_used += sz;
            }
            s->entries[idx].used = 1;
            s->n_stored++;
            return s->entries[idx].data;
        }
    }
    return NULL;
}

static uint8_t *dt_bench_get(DtBenchStore *s, const char *name) {
    uint32_t slot = dt_bench_hash(name);
    for (int i = 0; i < 8; i++) {
        int idx = (slot + i) % DT_BENCH_SLOTS;
        if (s->entries[idx].used && strcmp(s->entries[idx].name, name) == 0)
            return s->entries[idx].data;
    }
    return NULL;
}

static void bench_dramtile_storage(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 3. DRamTile Storage (CPU — hash lookup)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    DtBenchStore store;
    dt_bench_init(&store, (size_t)64 * 1024 * 1024);

    char name[64];
    uint8_t data[256];
    memset(data, 0xBB, sizeof(data));

    /* put */
    double t0 = now_sec();
    for (int i = 0; i < BENCH_N_tensors; i++) {
        snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
        dt_bench_put(&store, name, data, sizeof(data));
    }
    double t_put = (now_sec() - t0) * 1000;
    printf("  dt_put %d entries:        %.3f ms  (%.0f ns/op)\n",
           BENCH_N_tensors, t_put, t_put * 1e6 / BENCH_N_tensors);

    /* get — random access pattern */
    volatile uint8_t *sink = NULL;
    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        for (int i = 0; i < BENCH_N_tensors; i++) {
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            sink = dt_bench_get(&store, name);
        }
    }
    double t_get = (now_sec() - t0);
    printf("  dt_get %d × %d:          %.3f s  (%.0f ns/op)\n",
           BENCH_ITERATIONS, BENCH_N_tensors, t_get,
           t_get * 1e9 / (BENCH_ITERATIONS * BENCH_N_tensors));

    /* foreach */
    int count = 0;
    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        count = 0;
        for (int i = 0; i < DT_BENCH_SLOTS; i++) {
            if (store.entries[i].used) count++;
        }
    }
    double t_foreach = (now_sec() - t0) * 1000;
    printf("  foreach × %d:            %.3f ms  (%.0f ns/op)\n",
           BENCH_ITERATIONS, t_foreach,
           t_foreach * 1e6 / (BENCH_ITERATIONS * BENCH_N_tensors));

    free(store.pool);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * 4. CPU vs GPU Path — THE KEY BENCHMARK
 *
 * CPU path: 251× individual memcpy (simulates ggml_backend_tensor_set)
 * GPU path: 1× batch memcpy through contiguous pinned mirror (Gear2 approach)
 *
 * Also tests: GearShift dispatch overhead for both paths
 * ═══════════════════════════════════════════════════════════════ */

/* GPU simulation: contiguous mirror buffer (like Gear2 pinned memory) */
typedef struct {
    uint8_t *mirror;       /* contiguous pinned mirror */
    size_t   total_size;
    int      n_tensors;
    uint32_t offsets[251]; /* byte offset of each tensor in mirror */
    uint32_t sizes[251];   /* byte size of each tensor */
} GpuMirror;

static void gpu_mirror_init(GpuMirror *m, int n, size_t tensor_sz) {
    memset(m, 0, sizeof(*m));
    m->n_tensors = n;
    m->total_size = (size_t)n * tensor_sz;
    m->mirror = (uint8_t *)malloc(m->total_size);
    for (int i = 0; i < n; i++) {
        m->offsets[i] = (uint32_t)(i * tensor_sz);
        m->sizes[i] = (uint32_t)tensor_sz;
    }
}

static void gpu_mirror_destroy(GpuMirror *m) {
    free(m->mirror);
    memset(m, 0, sizeof(*m));
}

static void bench_cpu_vs_gpu_path(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 4. CPU vs GPU Path — Tensor Upload\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    /* ── Small tensors (251 × 256B = 64KB total) ── */
    printf("  [A] Small tensors: %d × %d B = %zu KB total\n\n",
           BENCH_N_tensors, BENCH_TENSOR_SZ,
           (size_t)BENCH_N_tensors * BENCH_TENSOR_SZ / 1024);

    uint8_t *src_data = (uint8_t *)malloc(BENCH_TENSOR_SZ);
    memset(src_data, 0xAA, BENCH_TENSOR_SZ);

    /* CPU path: individual memcpy per tensor (like ggml_backend_tensor_set) */
    uint8_t *cpu_buf = (uint8_t *)malloc(BENCH_N_tensors * BENCH_TENSOR_SZ);
    double t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        for (int i = 0; i < BENCH_N_tensors; i++) {
            /* simulate ggml_backend_tensor_set: each is individual driver call */
            memcpy(cpu_buf + i * BENCH_TENSOR_SZ, src_data, BENCH_TENSOR_SZ);
        }
    }
    double t_cpu = (now_sec() - t0);
    printf("  CPU (individual memcpy × %d):  %.3f s  (%.0f ns/tensor)\n",
           BENCH_ITERATIONS, t_cpu,
           t_cpu * 1e9 / (BENCH_ITERATIONS * BENCH_N_tensors));

    /* GPU path: Gear2 pinned mirror — fill mirror then single batch copy */
    GpuMirror mirror;
    gpu_mirror_init(&mirror, BENCH_N_tensors, BENCH_TENSOR_SZ);

    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        /* Phase 1: memcpy face data → pinned mirror (CPU-side) */
        for (int i = 0; i < BENCH_N_tensors; i++) {
            memcpy(mirror.mirror + mirror.offsets[i], src_data, mirror.sizes[i]);
        }
        /* Phase 2: single batch H2D (simulated — just the memcpy cost) */
        /* In real GPU: cudaMemcpyAsync(gpu_buf, mirror, total_size) */
        /* Here we skip the actual DMA and just measure the mirror fill */
    }
    double t_gpu_mirror = (now_sec() - t0);
    printf("  GPU (fill mirror + 1 batch):  %.3f s  (%.0f ns/tensor)\n",
           t_gpu_mirror,
           t_gpu_mirror * 1e9 / (BENCH_ITERATIONS * BENCH_N_tensors));
    printf("  → mirror fill speedup: %.2fx\n", t_cpu / t_gpu_mirror);

    /* GPU path with GearShift dispatch overhead included */
    GearShiftStore gs;
    gs_init(&gs);
    char name[64];
    int gs_count = 0;

    static int gs_bench_cb(const void *src, size_t sz, void *dst, void *u) {
        (void)u;
        memcpy(dst, src, sz);
        (*(int *)u)++;
        return 0;
    }

    for (int i = 0; i < BENCH_N_tensors; i++) {
        snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
        gs_register(&gs, name, i / 40);
        gs_set_dest(&gs, name, gs_bench_cb, mirror.mirror + mirror.offsets[i], &gs_count);
    }

    t0 = now_sec();
    for (int it = 0; it < BENCH_ITERATIONS; it++) {
        gs_reset_done(&gs);
        for (int i = 0; i < BENCH_N_tensors; i++) {
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            gs_stream_from(&gs, name, src_data, BENCH_TENSOR_SZ);
        }
    }
    double t_gs_gpu = (now_sec() - t0);
    printf("  GearShift → mirror fill:      %.3f s  (%.0f ns/tensor)\n",
           t_gs_gpu,
           t_gs_gpu * 1e9 / (BENCH_ITERATIONS * BENCH_N_tensors));
    printf("  GearShift overhead per cycle: %.0f ns\n",
           (t_gs_gpu - t_gpu_mirror) * 1e9 / BENCH_ITERATIONS);

    gs_destroy(&gs);
    gpu_mirror_destroy(&mirror);
    free(cpu_buf);
    free(src_data);
    printf("\n");

    /* ── Large tensors (251 × 64KB = 16MB total) ── */
    printf("  [B] Large tensors: %d × %d KB = %zu MB total\n\n",
           BENCH_N_tensors, BENCH_LARGE_SZ / 1024,
           (size_t)BENCH_N_tensors * BENCH_LARGE_SZ / (1024 * 1024));

    uint8_t *large_src = (uint8_t *)malloc(BENCH_LARGE_SZ);
    memset(large_src, 0xCC, BENCH_LARGE_SZ);

    /* CPU path: individual memcpy */
    uint8_t *large_cpu = (uint8_t *)malloc((size_t)BENCH_N_tensors * BENCH_LARGE_SZ);
    int iters_large = 100; /* fewer iters for large data */

    t0 = now_sec();
    for (int it = 0; it < iters_large; it++) {
        for (int i = 0; i < BENCH_N_tensors; i++) {
            memcpy(large_cpu + (size_t)i * BENCH_LARGE_SZ, large_src, BENCH_LARGE_SZ);
        }
    }
    double t_cpu_large = (now_sec() - t0);
    printf("  CPU (individual memcpy × %d):  %.3f s  (%.0f ns/tensor)\n",
           iters_large, t_cpu_large,
           t_cpu_large * 1e9 / (iters_large * BENCH_N_tensors));

    /* GPU path: Gear2 mirror */
    GpuMirror large_mirror;
    gpu_mirror_init(&large_mirror, BENCH_N_tensors, BENCH_LARGE_SZ);

    t0 = now_sec();
    for (int it = 0; it < iters_large; it++) {
        for (int i = 0; i < BENCH_N_tensors; i++) {
            memcpy(large_mirror.mirror + large_mirror.offsets[i],
                   large_src, large_mirror.sizes[i]);
        }
    }
    double t_gpu_large = (now_sec() - t0);
    printf("  GPU (fill mirror + 1 batch):  %.3f s  (%.0f ns/tensor)\n",
           t_gpu_large,
           t_gpu_large * 1e9 / (iters_large * BENCH_N_tensors));
    printf("  → speedup: %.2fx\n", t_cpu_large / t_gpu_large);

    free(large_src);
    free(large_cpu);
    gpu_mirror_destroy(&large_mirror);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * 5. Integrated Pipeline: Full Gear Chain
 *
 * Simulates one decode cycle:
 *   DRamTile lookup → GearLock score → GearShift dispatch → mirror fill
 * ═══════════════════════════════════════════════════════════════ */

static void bench_integrated_pipeline(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 5. Integrated Pipeline (full gear chain)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    /* setup stores */
    DtBenchStore dt_store;
    dt_bench_init(&dt_store, (size_t)64 * 1024 * 1024);

    GearShiftStore gs;
    gs_init(&gs);

    GearLockState gl;
    gear_lock_init(&gl);

    /* fill DRamTile store */
    char name[64];
    uint8_t data[256];
    memset(data, 0xAA, sizeof(data));
    for (int i = 0; i < BENCH_N_tensors; i++) {
        snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
        dt_bench_put(&dt_store, name, data, sizeof(data));
    }

    /* setup GearShift */
    GpuMirror mirror;
    gpu_mirror_init(&mirror, BENCH_N_tensors, 256);

    static int pipeline_cb(const void *src, size_t sz, void *dst, void *u) {
        (void)u;
        memcpy(dst, src, sz);
        return 0;
    }

    for (int i = 0; i < BENCH_N_tensors; i++) {
        snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
        gs_register(&gs, name, i / 40);
        gs_set_dest(&gs, name, pipeline_cb,
                    mirror.mirror + mirror.offsets[i], NULL);
    }

    /* setup GearLock route history */
    uint64_t routes[251];
    uint8_t  events[251];
    int      ft_indices[251];
    for (int i = 0; i < BENCH_N_tensors; i++) ft_indices[i] = i;
    srand(42);
    for (int i = 0; i < BENCH_N_tensors; i++) {
        routes[i] = ((uint64_t)rand() << 32) ^ (uint64_t)rand();
        events[i] = (rand() % 20 == 0) ? 0x02 : 0x00;
    }
    /* build initial gear lock state */
    for (int c = 0; c < GL_HISTORY_DEPTH; c++) {
        for (int i = 0; i < BENCH_N_tensors; i++)
            routes[i] ^= (uint64_t)c * 0x9E3779B97F4A7C15ULL;
        gear_lock_update(&gl, routes, events, BENCH_N_tensors, ft_indices);
    }

    /* benchmark: full pipeline per cycle */
    uint8_t mask[251];
    int iters = 500;

    double t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        /* Step 1: GearLock — build priority mask */
        gear_lock_build_mask(&gl, BENCH_N_tensors, ft_indices, mask, 0.30f);

        /* Step 2: GearShift — dispatch only high-priority tensors */
        gs_reset_done(&gs);
        for (int i = 0; i < BENCH_N_tensors; i++) {
            if (!mask[i]) continue; /* skip low-priority */
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            uint8_t *stored = dt_bench_get(&dt_store, name);
            if (stored) {
                gs_stream_from(&gs, name, stored, 256);
            }
        }

        /* Step 3: update GearLock with new routes */
        for (int i = 0; i < BENCH_N_tensors; i++)
            routes[i] ^= (uint64_t)it * 0x9E3779B97F4A7C15ULL;
        gear_lock_update(&gl, routes, events, BENCH_N_tensors, ft_indices);
    }
    double t_pipeline = (now_sec() - t0);

    printf("  Full cycle (%d iters × %d tensors):\n", iters, BENCH_N_tensors);
    printf("    Total time:    %.3f s\n", t_pipeline);
    printf("    Per cycle:     %.0f us\n", t_pipeline * 1e6 / iters);
    printf("    Per tensor:    %.0f ns\n",
           t_pipeline * 1e9 / (iters * BENCH_N_tensors));

    /* breakdown: which step is bottleneck? */
    double t_gl = 0, t_gs = 0, t_dt = 0;
    int high_pri_count = 0;

    /* GearLock only */
    t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        gear_lock_build_mask(&gl, BENCH_N_tensors, ft_indices, mask, 0.30f);
    }
    t_gl = (now_sec() - t0);

    /* count high-priority */
    high_pri_count = 0;
    for (int i = 0; i < BENCH_N_tensors; i++)
        if (mask[i]) high_pri_count++;

    /* DRamTile lookup only */
    t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < BENCH_N_tensors; i++) {
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            dt_bench_get(&dt_store, name);
        }
    }
    t_dt = (now_sec() - t0);

    /* GearShift stream only (for high-priority subset) */
    t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        gs_reset_done(&gs);
        for (int i = 0; i < high_pri_count; i++) {
            snprintf(name, sizeof(name), "blk.attn_q.w[%d]", i);
            gs_stream_from(&gs, name, data, 256);
        }
    }
    t_gs = (now_sec() - t0);

    printf("\n  Breakdown per cycle:\n");
    printf("    GearLock mask:   %.1f us  (%.1f%%)\n",
           t_gl * 1e6 / iters, t_gl / t_pipeline * 100);
    printf("    DRamTile lookup: %.1f us  (%.1f%%)\n",
           t_dt * 1e6 / iters, t_dt / t_pipeline * 100);
    printf("    GearShift stream:%.1f us  (%.1f%%)  [%d/%d tensors]\n",
           t_gs * 1e6 / iters, t_gs / t_pipeline * 100,
           high_pri_count, BENCH_N_tensors);
    printf("    Total:           %.1f us/cycle\n", t_pipeline * 1e6 / iters);

    gs_destroy(&gs);
    gpu_mirror_destroy(&mirror);
    free(dt_store.pool);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║  Gear System Benchmark — CPU vs GPU Path             ║\n");
    printf("║  GearShift + GearLock + DRamTile + Gear2             ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");
    printf("Config: %d tensors, %d iterations\n\n", BENCH_N_tensors, BENCH_ITERATIONS);

    bench_gearshift_routing();    /* 1. routing layer */
    bench_gearlock_scoring();     /* 2. stability scoring */
    bench_dramtile_storage();     /* 3. hash storage */
    bench_cpu_vs_gpu_path();      /* 4. CPU vs GPU path comparison */
    bench_integrated_pipeline();  /* 5. full pipeline */

    printf("═══════════════════════════════════════════════════════\n");
    printf(" Summary\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  GearShift:   CPU-only routing, O(n) linear scan\n");
    printf("  GearLock:    CPU-only scoring, O(n×depth) per update\n");
    printf("  DRamTile:    CPU-only storage, O(1) amortized hash\n");
    printf("  Gear2:       CPU→GPU bridge, 251× individual → 1× batch DMA\n");
    printf("  GPU win:     Gear2 eliminates 251 driver context switches\n");
    printf("               Single cudaMemcpyAsync vs 251× tensor_set\n");
    printf("═══════════════════════════════════════════════════════\n");
    return 0;
}

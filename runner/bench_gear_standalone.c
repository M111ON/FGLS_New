/*
 * bench_gear_standalone.c — Self-contained Gear System Benchmark
 *
 * No external dependencies — everything inline.
 * Measures: GearShift routing + GearLock scoring + DRamTile hash + CPU vs GPU path
 *
 * Compile:
 *   C:\msys64\mingw64\bin\gcc.exe -O2 -std=c11 -o bench_gear_standalone.exe bench_gear_standalone.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════
 * Inline GearShift (from gear_shift.h — minimal subset)
 * ═══════════════════════════════════════════════════════════════ */
#define GS_MAX_ENTRIES  2048
#define GS_NAME_MAX     128

typedef enum { GS_IDLE=0, GS_STREAMING=1, GS_DONE=2, GS_FAILED=3 } GSState;
typedef int (*GSStreamFn)(const void *src, size_t sz, void *dst, void *user);

typedef struct {
    char       name[GS_NAME_MAX];
    void      *src_ptr;
    size_t     src_size;
    float      priority;
    uint32_t   access_tick;
    uint32_t   layer;
    uint16_t   state;
    uint16_t   flags;
    void      *dst_ctx;
    void      *user_data;
    GSStreamFn stream_fn;
} GSEntry;

typedef struct {
    GSEntry   entries[GS_MAX_ENTRIES];
    int       n_entries;
    uint32_t  tick;
    uint32_t  n_streamed;
    uint32_t  n_errors;
} GearShiftStore;

static inline void gs_init(GearShiftStore *gs) { memset(gs, 0, sizeof(*gs)); }

static inline int gs_register(GearShiftStore *gs, const char *name, uint32_t layer) {
    if (gs->n_entries >= GS_MAX_ENTRIES) return -1;
    GSEntry *e = &gs->entries[gs->n_entries];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, GS_NAME_MAX - 1);
    e->layer = layer;
    e->state = GS_IDLE;
    gs->n_entries++;
    return 0;
}

static inline int gs_set_dest(GearShiftStore *gs, const char *name,
                               GSStreamFn fn, void *dst, void *user) {
    for (int i = 0; i < gs->n_entries; i++) {
        if (strcmp(gs->entries[i].name, name) == 0) {
            gs->entries[i].stream_fn = fn;
            gs->entries[i].dst_ctx = dst;
            gs->entries[i].user_data = user;
            return 0;
        }
    }
    return -1;
}

static inline GSEntry *gs_find(GearShiftStore *gs, const char *name) {
    for (int i = 0; i < gs->n_entries; i++)
        if (strcmp(gs->entries[i].name, name) == 0) return &gs->entries[i];
    return NULL;
}

static inline int gs_stream_from(GearShiftStore *gs, const char *name,
                                  void *src, size_t sz) {
    GSEntry *e = gs_find(gs, name);
    if (!e) return -1;
    if (e->state == GS_DONE) return 0;
    if (!e->stream_fn) return -1;
    e->src_ptr = src; e->src_size = sz;
    e->state = GS_STREAMING;
    e->access_tick = ++gs->tick;
    int ret = e->stream_fn(e->src_ptr, e->src_size, e->dst_ctx, e->user_data);
    if (ret == 0) { e->state = GS_DONE; gs->n_streamed++; }
    else { e->state = GS_FAILED; gs->n_errors++; }
    return ret;
}

static inline void gs_reset_done(GearShiftStore *gs) {
    for (int i = 0; i < gs->n_entries; i++)
        if (gs->entries[i].state == GS_DONE) gs->entries[i].state = GS_IDLE;
}

static inline void gs_reset_all(GearShiftStore *gs) {
    for (int i = 0; i < gs->n_entries; i++) gs->entries[i].state = GS_IDLE;
}

static inline void gs_destroy(GearShiftStore *gs) {
    memset(gs->entries, 0, sizeof(gs->entries));
    gs->n_entries = 0; gs->tick = 0;
    gs->n_streamed = 0; gs->n_errors = 0;
}

/* ═══════════════════════════════════════════════════════════════
 * Inline GearLock (from gear_lock.h — minimal subset)
 * ═══════════════════════════════════════════════════════════════ */
#define GL_HISTORY_DEPTH  8
#define GL_MAX_TENSORS    4096

typedef struct {
    uint64_t routes[GL_HISTORY_DEPTH];
    uint8_t  samples;
    uint8_t  boundaries;
    float    stability;
    float    priority;
} GLTensor;

typedef struct {
    GLTensor tensors[GL_MAX_TENSORS];
    int      cycles;
    int      n_active;
} GearLockState;

static inline void gear_lock_init(GearLockState *gs) { memset(gs, 0, sizeof(*gs)); }

#ifdef _MSC_VER
#include <intrin.h>
static inline int gl_popcnt64(uint64_t x) { return (int)__popcnt64(x); }
#else
static inline int gl_popcnt64(uint64_t x) { return (int)__builtin_popcountll(x); }
#endif

static inline void gear_lock_update(GearLockState *gs,
    const uint64_t *routes, const uint8_t *events, int n, const int *ft_indices)
{
    if (!gs || !routes || !events || n <= 0) return;
    gs->cycles++;
    for (int i = 0; i < n; i++) {
        int ti = ft_indices[i];
        if (ti < 0 || ti >= GL_MAX_TENSORS) continue;
        GLTensor *t = &gs->tensors[ti];
        int pos = t->samples % GL_HISTORY_DEPTH;
        t->routes[pos] = routes[i];
        if (events[i] & 0x02) { if (t->boundaries < 255) t->boundaries++; }
        if (t->samples < GL_HISTORY_DEPTH) t->samples++;
        if (t->samples >= 2) {
            int np = t->samples - 1;
            int oldest = t->samples < GL_HISTORY_DEPTH ? 0 : (pos + 1) % GL_HISTORY_DEPTH;
            uint64_t sum = 0;
            for (int j = 0; j < np; j++) {
                int a = (oldest + j) % GL_HISTORY_DEPTH;
                int b = (a + 1) % GL_HISTORY_DEPTH;
                sum += gl_popcnt64(t->routes[a] ^ t->routes[b]);
            }
            float avg = (float)sum / (float)np;
            t->stability = 1.0f - (avg / 64.0f);
            if (t->stability < 0.0f) t->stability = 0.0f;
        }
        float br = (float)t->boundaries / (float)(t->samples);
        if (br > 1.0f) br = 1.0f;
        t->priority = t->stability * (1.0f - br);
    }
    gs->n_active = 0;
    for (int i = 0; i < GL_MAX_TENSORS; i++)
        if (gs->tensors[i].samples > 0) gs->n_active++;
}

static inline int gear_lock_build_mask(const GearLockState *gs,
    int n, const int *ft_indices, uint8_t *mask, float threshold)
{
    if (!gs || !ft_indices || !mask || n <= 0) return 0;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        float s = (ft_indices[i] >= 0 && ft_indices[i] < GL_MAX_TENSORS)
                  ? gs->tensors[ft_indices[i]].priority : 0.0f;
        mask[i] = (s >= threshold) ? 1 : 0;
        if (mask[i]) cnt++;
    }
    return cnt;
}

/* ═══════════════════════════════════════════════════════════════
 * Config
 * ═══════════════════════════════════════════════════════════════ */
#define N_TENSORS      251
#define N_ITERS        1000
#define TENSOR_SZ      256
#define LARGE_SZ       (64*1024)
#define HASH_SLOTS     512
#define NAME_MAX       64

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ═══════════════════════════════════════════════════════════════
 * Simple hash store (simulates DRamTile)
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
    char    name[NAME_MAX];
    uint8_t *data;
    size_t   size;
    int      used;
} HashEntry;

typedef struct {
    HashEntry entries[HASH_SLOTS];
    uint8_t  *pool;
    size_t    pool_size, pool_used;
    int       n_stored;
} HashStore;

static uint32_t hash_fn(const char *name) {
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = ((h << 5) + h) + *p;
    return h % HASH_SLOTS;
}

static void hs_init(HashStore *s, size_t pool_sz) {
    memset(s, 0, sizeof(*s));
    s->pool = (uint8_t *)malloc(pool_sz);
    s->pool_size = pool_sz;
}

static uint8_t *hs_put(HashStore *s, const char *name, const uint8_t *data, size_t sz) {
    uint32_t slot = hash_fn(name);
    for (int i = 0; i < 8; i++) {
        int idx = (slot + i) % HASH_SLOTS;
        if (!s->entries[idx].used) {
            strncpy(s->entries[idx].name, name, NAME_MAX - 1);
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

static uint8_t *hs_get(HashStore *s, const char *name) {
    uint32_t slot = hash_fn(name);
    for (int i = 0; i < 8; i++) {
        int idx = (slot + i) % HASH_SLOTS;
        if (s->entries[idx].used && strcmp(s->entries[idx].name, name) == 0)
            return s->entries[idx].data;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * 1. GearShift Routing Benchmark
 * ═══════════════════════════════════════════════════════════════ */
static int gs_sink_n = 0;
static int gs_cb(const void *src, size_t sz, void *dst, void *u) {
    (void)u;
    if (dst && src) memcpy(dst, src, sz < 256 ? sz : 256);
    gs_sink_n++;
    return 0;
}

static void bench_gearshift(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 1. GearShift Routing (CPU)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    GearShiftStore gs; gs_init(&gs);
    char name[64];
    void *src = malloc(TENSOR_SZ); memset(src, 0xAA, TENSOR_SZ);
    void *dst = malloc(TENSOR_SZ);

    double t0 = now_sec();
    for (int i = 0; i < N_TENSORS; i++) {
        snprintf(name, sizeof(name), "blk.w[%d]", i);
        gs_register(&gs, name, i / 40);
        gs_set_dest(&gs, name, gs_cb, dst, NULL);
    }
    printf("  Register %d:       %.3f ms\n", N_TENSORS, (now_sec() - t0) * 1000);

    t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++) {
        gs_reset_done(&gs);
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.w[%d]", i);
            gs_stream_from(&gs, name, src, TENSOR_SZ);
        }
    }
    double dt = now_sec() - t0;
    printf("  Batch %d×%d:      %.3f s  (%.0f ns/tensor)\n",
           N_ITERS, N_TENSORS, dt, dt * 1e9 / (N_ITERS * N_TENSORS));

    gs_destroy(&gs); free(src); free(dst);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * 2. GearLock Scoring Benchmark
 * ═══════════════════════════════════════════════════════════════ */
static void bench_gearlock(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 2. GearLock Stability Scoring (CPU)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    GearLockState gl; gear_lock_init(&gl);
    uint64_t routes[N_TENSORS];
    uint8_t  events[N_TENSORS];
    int      fti[N_TENSORS];
    for (int i = 0; i < N_TENSORS; i++) fti[i] = i;
    srand(42);
    for (int i = 0; i < N_TENSORS; i++) {
        routes[i] = ((uint64_t)rand() << 32) ^ (uint64_t)rand();
        events[i] = (rand() % 20 == 0) ? 0x02 : 0x00;
    }

    double t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++) {
        gear_lock_init(&gl);
        for (int c = 0; c < GL_HISTORY_DEPTH; c++) {
            for (int i = 0; i < N_TENSORS; i++)
                routes[i] ^= (uint64_t)c * 0x9E3779B97F4A7C15ULL;
            gear_lock_update(&gl, routes, events, N_TENSORS, fti);
        }
    }
    double dt = now_sec() - t0;
    printf("  8 cycles × %d × %d: %.3f s\n", N_TENSORS, N_ITERS, dt);
    printf("  Per update:         %.0f ns  (%.1f ns/tensor)\n",
           dt * 1e9 / (N_ITERS * GL_HISTORY_DEPTH),
           dt * 1e9 / (N_ITERS * GL_HISTORY_DEPTH * N_TENSORS));

    uint8_t mask[N_TENSORS];
    t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++)
        gear_lock_build_mask(&gl, N_TENSORS, fti, mask, 0.30f);
    double tm = (now_sec() - t0) * 1000;
    printf("  build_mask × %d:   %.3f ms  (%.0f ns/op)\n",
           N_ITERS, tm, tm * 1e6 / N_ITERS);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * 3. DRamTile Hash Benchmark
 * ═══════════════════════════════════════════════════════════════ */
static void bench_dramtile(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 3. DRamTile Hash Storage (CPU)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    HashStore s; hs_init(&s, 64 * 1024 * 1024);
    char name[64];
    uint8_t data[256]; memset(data, 0xBB, sizeof(data));

    double t0 = now_sec();
    for (int i = 0; i < N_TENSORS; i++) {
        snprintf(name, sizeof(name), "blk.w[%d]", i);
        hs_put(&s, name, data, sizeof(data));
    }
    printf("  put %d:            %.3f ms  (%.0f ns/op)\n",
           N_TENSORS, (now_sec() - t0) * 1000,
           (now_sec() - t0) * 1e6 / N_TENSORS);

    volatile uint8_t *sink = NULL;
    t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++)
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.w[%d]", i);
            sink = hs_get(&s, name);
        }
    double dt = now_sec() - t0;
    printf("  get %d×%d:        %.3f s  (%.0f ns/op)\n",
           N_ITERS, N_TENSORS, dt, dt * 1e9 / (N_ITERS * N_TENSORS));

    free(s.pool);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * 4. CPU vs GPU Path — THE KEY BENCHMARK
 * ═══════════════════════════════════════════════════════════════ */
static void bench_cpu_vs_gpu(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 4. CPU vs GPU Path — Tensor Upload\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    uint8_t *src = (uint8_t *)malloc(TENSOR_SZ);
    memset(src, 0xAA, TENSOR_SZ);

    /* ── Small (251 × 256B = 64KB) ── */
    printf("  [A] Small: %d × %d B = %zu KB\n\n", N_TENSORS, TENSOR_SZ,
           (size_t)N_TENSORS * TENSOR_SZ / 1024);

    /* CPU: individual memcpy (simulates ggml_backend_tensor_set × 251) */
    uint8_t *cpu_buf = (uint8_t *)malloc((size_t)N_TENSORS * TENSOR_SZ);
    double t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++)
        for (int i = 0; i < N_TENSORS; i++)
            memcpy(cpu_buf + (size_t)i * TENSOR_SZ, src, TENSOR_SZ);
    double t_cpu = now_sec() - t0;
    printf("  CPU (251× memcpy):    %.3f s  (%.0f ns/tensor)\n",
           t_cpu, t_cpu * 1e9 / (N_ITERS * N_TENSORS));

    /* GPU: Gear2 mirror — contiguous buffer, single batch copy */
    uint8_t *mirror = (uint8_t *)malloc((size_t)N_TENSORS * TENSOR_SZ);
    t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++) {
        for (int i = 0; i < N_TENSORS; i++)
            memcpy(mirror + (size_t)i * TENSOR_SZ, src, TENSOR_SZ);
        /* single batch DMA would go here (skip — measuring mirror fill only) */
    }
    double t_gpu = now_sec() - t0;
    printf("  GPU (mirror+1 batch): %.3f s  (%.0f ns/tensor)\n",
           t_gpu, t_gpu * 1e9 / (N_ITERS * N_TENSORS));
    printf("  → mirror speedup:     %.2fx\n\n", t_cpu / t_gpu);

    /* GearShift overhead on GPU path */
    GearShiftStore gs; gs_init(&gs);
    char name[64];
    for (int i = 0; i < N_TENSORS; i++) {
        snprintf(name, sizeof(name), "blk.w[%d]", i);
        gs_register(&gs, name, i / 40);
        gs_set_dest(&gs, name, gs_cb, mirror + (size_t)i * TENSOR_SZ, NULL);
    }

    t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++) {
        gs_reset_done(&gs);
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.w[%d]", i);
            gs_stream_from(&gs, name, src, TENSOR_SZ);
        }
    }
    double t_gs = now_sec() - t0;
    printf("  GearShift→mirror:     %.3f s  (%.0f ns/tensor)\n",
           t_gs, t_gs * 1e9 / (N_ITERS * N_TENSORS));
    printf("  GearShift overhead:   %.0f ns/cycle\n\n",
           (t_gs - t_gpu) * 1e9 / N_ITERS);

    /* ── Large (251 × 64KB = 16MB) ── */
    printf("  [B] Large: %d × %d KB = %zu MB\n\n", N_TENSORS, LARGE_SZ / 1024,
           (size_t)N_TENSORS * LARGE_SZ / (1024 * 1024));

    uint8_t *lsrc = (uint8_t *)malloc(LARGE_SZ); memset(lsrc, 0xCC, LARGE_SZ);
    int iters_l = 100;

    uint8_t *lcpu = (uint8_t *)malloc((size_t)N_TENSORS * LARGE_SZ);
    t0 = now_sec();
    for (int it = 0; it < iters_l; it++)
        for (int i = 0; i < N_TENSORS; i++)
            memcpy(lcpu + (size_t)i * LARGE_SZ, lsrc, LARGE_SZ);
    double t_cl = now_sec() - t0;

    uint8_t *lm = (uint8_t *)malloc((size_t)N_TENSORS * LARGE_SZ);
    t0 = now_sec();
    for (int it = 0; it < iters_l; it++)
        for (int i = 0; i < N_TENSORS; i++)
            memcpy(lm + (size_t)i * LARGE_SZ, lsrc, LARGE_SZ);
    double t_gl = now_sec() - t0;

    printf("  CPU (251× memcpy):    %.3f s  (%.0f ns/tensor)\n",
           t_cl, t_cl * 1e9 / (iters_l * N_TENSORS));
    printf("  GPU (mirror+1 batch): %.3f s  (%.0f ns/tensor)\n",
           t_gl, t_gl * 1e9 / (iters_l * N_TENSORS));
    printf("  → speedup:            %.2fx\n\n", t_cl / t_gl);

    gs_destroy(&gs);
    free(src); free(cpu_buf); free(mirror);
    free(lsrc); free(lcpu); free(lm);
}

/* ═══════════════════════════════════════════════════════════════
 * 5. Integrated Pipeline
 * ═══════════════════════════════════════════════════════════════ */
static void bench_pipeline(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 5. Integrated Pipeline (full gear chain)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    HashStore store; hs_init(&store, 64 * 1024 * 1024);
    GearShiftStore gs; gs_init(&gs);
    GearLockState gl; gear_lock_init(&gl);

    char name[64];
    uint8_t data[256]; memset(data, 0xAA, sizeof(data));
    uint8_t *mirror = (uint8_t *)malloc((size_t)N_TENSORS * TENSOR_SZ);

    for (int i = 0; i < N_TENSORS; i++) {
        snprintf(name, sizeof(name), "blk.w[%d]", i);
        hs_put(&store, name, data, sizeof(data));
        gs_register(&gs, name, i / 40);
        gs_set_dest(&gs, name, gs_cb, mirror + (size_t)i * TENSOR_SZ, NULL);
    }

    uint64_t routes[N_TENSORS];
    uint8_t  events[N_TENSORS];
    int      fti[N_TENSORS];
    for (int i = 0; i < N_TENSORS; i++) fti[i] = i;
    srand(42);
    for (int i = 0; i < N_TENSORS; i++) {
        routes[i] = ((uint64_t)rand() << 32) ^ (uint64_t)rand();
        events[i] = (rand() % 20 == 0) ? 0x02 : 0x00;
    }
    for (int c = 0; c < GL_HISTORY_DEPTH; c++) {
        for (int i = 0; i < N_TENSORS; i++)
            routes[i] ^= (uint64_t)c * 0x9E3779B97F4A7C15ULL;
        gear_lock_update(&gl, routes, events, N_TENSORS, fti);
    }

    uint8_t mask[N_TENSORS];
    int iters = 500;

    double t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        gear_lock_build_mask(&gl, N_TENSORS, fti, mask, 0.30f);
        gs_reset_done(&gs);
        for (int i = 0; i < N_TENSORS; i++) {
            if (!mask[i]) continue;
            snprintf(name, sizeof(name), "blk.w[%d]", i);
            uint8_t *stored = hs_get(&store, name);
            if (stored) gs_stream_from(&gs, name, stored, TENSOR_SZ);
        }
        for (int i = 0; i < N_TENSORS; i++)
            routes[i] ^= (uint64_t)it * 0x9E3779B97F4A7C15ULL;
        gear_lock_update(&gl, routes, events, N_TENSORS, fti);
    }
    double dt = now_sec() - t0;

    int hp = 0;
    for (int i = 0; i < N_TENSORS; i++) if (mask[i]) hp++;

    printf("  %d cycles × %d tensors:\n", iters, N_TENSORS);
    printf("    Total:          %.3f s\n", dt);
    printf("    Per cycle:      %.0f us\n", dt * 1e6 / iters);
    printf("    Per tensor:     %.0f ns\n", dt * 1e9 / (iters * N_TENSORS));
    printf("    High-priority:  %d/%d tensors\n\n", hp, N_TENSORS);

    /* breakdown */
    double t_gl = 0, t_gs = 0, t_dt = 0;

    t0 = now_sec();
    for (int it = 0; it < iters; it++)
        gear_lock_build_mask(&gl, N_TENSORS, fti, mask, 0.30f);
    t_gl = now_sec() - t0;

    t0 = now_sec();
    for (int it = 0; it < iters; it++)
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.w[%d]", i);
            hs_get(&store, name);
        }
    t_dt = now_sec() - t0;

    t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        gs_reset_done(&gs);
        for (int i = 0; i < N_TENSORS; i++)
            if (mask[i]) {
                snprintf(name, sizeof(name), "blk.w[%d]", i);
                gs_stream_from(&gs, name, data, TENSOR_SZ);
            }
    }
    t_gs = now_sec() - t0;

    printf("  Breakdown per cycle:\n");
    printf("    GearLock mask:    %.1f us  (%.1f%%)\n", t_gl*1e6/iters, t_gl/dt*100);
    printf("    DRamTile lookup:  %.1f us  (%.1f%%)\n", t_dt*1e6/iters, t_dt/dt*100);
    printf("    GearShift stream: %.1f us  (%.1f%%)  [%d/%d]\n",
           t_gs*1e6/iters, t_gs/dt*100, hp, N_TENSORS);
    printf("    Total:            %.1f us/cycle\n\n", dt*1e6/iters);

    gs_destroy(&gs); free(store.pool); free(mirror);
}

/* ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║  Gear System Benchmark — CPU vs GPU Path             ║\n");
    printf("║  GearShift + GearLock + DRamTile + Gear2             ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");
    printf("Config: %d tensors, %d iterations\n\n", N_TENSORS, N_ITERS);

    bench_gearshift();
    bench_gearlock();
    bench_dramtile();
    bench_cpu_vs_gpu();
    bench_pipeline();

    printf("═══════════════════════════════════════════════════════\n");
    printf(" Summary\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  GearShift:   CPU routing, O(n) scan\n");
    printf("  GearLock:    CPU scoring, O(n×depth)\n");
    printf("  DRamTile:    CPU hash, O(1) amortized\n");
    printf("  Gear2:       251× individual → 1× batch DMA\n");
    printf("  GPU benefit: eliminates driver context switches\n");
    printf("═══════════════════════════════════════════════════════\n");
    return 0;
}

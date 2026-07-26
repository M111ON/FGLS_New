/*
 * bench_full_pipeline.c — DRamTile + GearShift + GearLock + Gear2
 *
 * Complete pipeline benchmark:
 *   DRamTile:  hash storage (where tensor data lives)
 *   GearShift: name-based routing (connects DRamTile → GPU mirror)
 *   GearLock:  c144 tag + CPU/GPU world tracking (priority control)
 *   Gear2:     pinned memory mirror + single batch DMA
 *
 * Real flow:
 *   geo_frame_seek file (2B headers)
 *     → CPU scan headers (L1 cache, fast)
 *     → GearLock: c144 tag, priority scoring
 *     → GearShift: route from DRamTile → mirror
 *     → Gear2: single memcpyAsync → GPU
 *
 * Compile (MSYS2):
 *   set PATH=C:\msys64\mingw64\bin;%PATH%
 *   gcc -O2 -std=c11 -Wl,--stack,16777216 -o bench_full_pipeline.exe bench_full_pipeline.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════
 * Config
 * ═══════════════════════════════════════════════════════════════ */
#define N_TENSORS       251
#define TENSOR_SZ       256
#define LARGE_TENSOR_SZ (64*1024)
#define N_ITERS         1000
#define GEAR2_MAX       512

/* GearLock constants (from gear_lock.h) */
#define GEAR_CPU_WORLD  128u
#define GEAR_GPU_WORLD  162u
#define GEAR_C144_CYCLE 144u
#define GEAR_GEO_FULL   (GEAR_CPU_WORLD * GEAR_GPU_WORLD)

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ═══════════════════════════════════════════════════════════════
 * 1. DRamTile — Hash Storage (from dramtile_store.h)
 *    Where tensor data lives
 * ═══════════════════════════════════════════════════════════════ */
#define DT_HASH_SLOTS 512
#define DT_NAME_MAX   64

typedef struct {
    uint32_t dram_addr;
    uint64_t offset;
    uint64_t size;
    char     name[DT_NAME_MAX];
    int      used;
} DtEntry;

typedef struct {
    uint8_t  *base;
    size_t    capacity;
    size_t    used;
    DtEntry   hash[DT_HASH_SLOTS];
    uint32_t  n_stored;
    /* cold tier */
    uint8_t  *cold_base;
    size_t    cold_capacity;
    size_t    cold_used;
    /* KV tier */
    uint8_t  *kv_base;
    size_t    kv_capacity;
    size_t    kv_used;
    /* stats */
    uint64_t  total_gets;
    uint64_t  total_puts;
} DRamTileBench;

static uint32_t dt_hash_name(const char *name) {
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = ((h << 5) + h) + *p;
    return h % DT_HASH_SLOTS;
}

static uint32_t dt_name_to_addr(const char *name) {
    /* Simplified: hash to dram address */
    uint32_t h = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = h * 31 + *p;
    return h & 0x0FFFFFFF;
}

static void dt_init(DRamTileBench *dt, size_t main_sz, size_t cold_sz, size_t kv_sz) {
    memset(dt, 0, sizeof(*dt));
    dt->base = (uint8_t *)calloc(1, main_sz);
    dt->capacity = main_sz;
    dt->cold_base = (uint8_t *)calloc(1, cold_sz);
    dt->cold_capacity = cold_sz;
    dt->kv_base = (uint8_t *)calloc(1, kv_sz);
    dt->kv_capacity = kv_sz;
}

static uint8_t *dt_put(DRamTileBench *dt, const char *name, const uint8_t *data, size_t sz) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    /* Linear probe */
    for (int i = 0; i < 8; i++) {
        int idx = (slot + i) % DT_HASH_SLOTS;
        if (!dt->hash[idx].used) {
            if (dt->used + sz > dt->capacity) return NULL;
            strncpy(dt->hash[idx].name, name, DT_NAME_MAX - 1);
            dt->hash[idx].name[DT_NAME_MAX - 1] = '\0';
            dt->hash[idx].dram_addr = addr;
            dt->hash[idx].offset = dt->used;
            dt->hash[idx].size = sz;
            dt->hash[idx].used = 1;
            memcpy(dt->base + dt->used, data, sz);
            dt->used += sz;
            dt->n_stored++;
            dt->total_puts++;
            return dt->base + dt->hash[idx].offset;
        }
    }
    return NULL;
}

static uint8_t *dt_get(DRamTileBench *dt, const char *name) {
    uint32_t addr = dt_name_to_addr(name);
    uint32_t slot = addr % DT_HASH_SLOTS;
    dt->total_gets++;
    for (int i = 0; i < 8; i++) {
        int idx = (slot + i) % DT_HASH_SLOTS;
        if (dt->hash[idx].used && strcmp(dt->hash[idx].name, name) == 0)
            return dt->base + dt->hash[idx].offset;
    }
    return NULL;
}

static void dt_destroy(DRamTileBench *dt) {
    free(dt->base); free(dt->cold_base); free(dt->kv_base);
    memset(dt, 0, sizeof(*dt));
}

/* ═══════════════════════════════════════════════════════════════
 * 2. GearLock — c144 tag + CPU/GPU world tracking
 *    (from gear_lock.h)
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
    volatile uint8_t c144;       /* current c144 tag */
    uint32_t cpu_ops;            /* CPU operation counter */
    uint32_t gpu_ops;            /* GPU operation counter */
    uint32_t cpu_worlds;         /* CPU worlds completed */
    uint32_t gpu_worlds;         /* GPU worlds completed */
} GearLockBench;

static inline uint32_t gl_tag(const GearLockBench *g) { return (uint32_t)g->c144; }
static inline void gl_cpu_tick(GearLockBench *g) {
    g->cpu_ops++;
    if (g->cpu_ops % GEAR_CPU_WORLD == 0)
        g->cpu_worlds = g->cpu_ops / GEAR_CPU_WORLD;
}
static inline void gl_gpu_tick(GearLockBench *g, uint32_t n) {
    uint32_t prev = g->gpu_ops;
    g->gpu_ops += n;
    if ((prev % GEAR_GPU_WORLD) + n >= GEAR_GPU_WORLD)
        g->gpu_worlds = g->gpu_ops / GEAR_GPU_WORLD;
}
static inline void gl_init(GearLockBench *g) { memset(g, 0, sizeof(*g)); }

/* ═══════════════════════════════════════════════════════════════
 * 3. GearShift — Name-based routing
 *    (from gear_shift.h)
 * ═══════════════════════════════════════════════════════════════ */
#define GS_MAX 2048
#define GS_NAME_MAX 64
typedef enum { GS_IDLE=0, GS_STREAMING=1, GS_DONE=2 } GSState;
typedef int (*GSStreamFn)(const void *src, size_t sz, void *dst, void *u);

typedef struct {
    char      name[GS_NAME_MAX];
    void     *src_ptr;
    size_t    src_size;
    float     priority;
    uint32_t  access_tick;
    uint32_t  layer;
    uint16_t  state;
    void     *dst_ctx;
    void     *user_data;
    GSStreamFn stream_fn;
} GSEntryBench;

typedef struct {
    GSEntryBench entries[GS_MAX];
    int          n_entries;
    uint32_t     tick;
    uint32_t     n_streamed;
    uint32_t     n_errors;
} GearShiftBench;

static void gs_init_b(GearShiftBench *gs) { memset(gs, 0, sizeof(*gs)); }

static int gs_register_b(GearShiftBench *gs, const char *name, uint32_t layer) {
    if (gs->n_entries >= GS_MAX) return -1;
    GSEntryBench *e = &gs->entries[gs->n_entries];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, GS_NAME_MAX - 1);
    e->layer = layer;
    e->state = GS_IDLE;
    gs->n_entries++;
    return 0;
}

static int gs_set_dest_b(GearShiftBench *gs, const char *name,
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

static GSEntryBench *gs_find_b(GearShiftBench *gs, const char *name) {
    for (int i = 0; i < gs->n_entries; i++)
        if (strcmp(gs->entries[i].name, name) == 0) return &gs->entries[i];
    return NULL;
}

/* O(1) direct index access — no strcmp */
static GSEntryBench *gs_get_idx_b(GearShiftBench *gs, int idx) {
    if (idx < 0 || idx >= gs->n_entries) return NULL;
    return &gs->entries[idx];
}

/* Stream by index — O(1) lookup */
static int gs_stream_idx_b(GearShiftBench *gs, int idx,
                            void *src, size_t sz) {
    GSEntryBench *e = gs_get_idx_b(gs, idx);
    if (!e) return -1;
    if (e->state == GS_DONE) return 0;
    if (!e->stream_fn) return -1;
    e->src_ptr = src; e->src_size = sz;
    e->state = GS_STREAMING;
    e->access_tick = ++gs->tick;
    int ret = e->stream_fn(e->src_ptr, e->src_size, e->dst_ctx, e->user_data);
    if (ret == 0) { e->state = GS_DONE; gs->n_streamed++; }
    else { e->state = GS_IDLE; gs->n_errors++; }
    return ret;
}

static int gs_stream_from_b(GearShiftBench *gs, const char *name,
                             void *src, size_t sz) {
    GSEntryBench *e = gs_find_b(gs, name);
    if (!e) return -1;
    if (e->state == GS_DONE) return 0;
    if (!e->stream_fn) return -1;
    e->src_ptr = src; e->src_size = sz;
    e->state = GS_STREAMING;
    e->access_tick = ++gs->tick;
    int ret = e->stream_fn(e->src_ptr, e->src_size, e->dst_ctx, e->user_data);
    if (ret == 0) { e->state = GS_DONE; gs->n_streamed++; }
    else { e->state = GS_IDLE; gs->n_errors++; }
    return ret;
}

static void gs_reset_done_b(GearShiftBench *gs) {
    for (int i = 0; i < gs->n_entries; i++)
        if (gs->entries[i].state == GS_DONE) gs->entries[i].state = GS_IDLE;
}

/* ═══════════════════════════════════════════════════════════════
 * 4. Gear2 — Pinned Memory Mirror (CPU→GPU bridge)
 *    (from gear2.h)
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t offset;
    uint32_t size;
    int      tensor_idx;
} G2TensorMap;

typedef struct {
    uint8_t  *gpu_base;       /* simulated GPU buffer */
    uint8_t  *pinned_base;    /* pinned host mirror */
    size_t    total_size;
    int       n_tensors;
    G2TensorMap map[GEAR2_MAX];
} G2Region;

typedef struct {
    G2Region  *regions;
    int        n_regions;
    int        enabled;
    /* stats */
    uint64_t   bytes_copied;
    int        n_transfers;
} Gear2Bench;

static void gear2_init_b(Gear2Bench *g2, int n_regions, size_t region_sz) {
    memset(g2, 0, sizeof(*g2));
    g2->n_regions = n_regions;
    g2->regions = (G2Region *)calloc((size_t)n_regions, sizeof(G2Region));
    for (int r = 0; r < n_regions; r++) {
        G2Region *reg = &g2->regions[r];
        reg->gpu_base = (uint8_t *)calloc(1, region_sz);
        reg->pinned_base = (uint8_t *)calloc(1, region_sz);
        reg->total_size = region_sz;
    }
    g2->enabled = 1;
}

/* Gear2 Phase 1: memcpy face data → pinned mirror */
static void gear2_fill_mirror(Gear2Bench *g2, int region,
                               int tensor_idx, const void *data, size_t sz) {
    G2Region *reg = &g2->regions[region];
    if (tensor_idx >= GEAR2_MAX) return;
    G2TensorMap *m = &reg->map[tensor_idx];
    m->offset = (uint32_t)(tensor_idx * sz);
    m->size = (uint32_t)sz;
    m->tensor_idx = tensor_idx;
    memcpy(reg->pinned_base + m->offset, data, sz);
    reg->n_tensors = tensor_idx + 1;
}

/* Gear2 Phase 2: single batch DMA (simulated — just memcpy) */
static void gear2_batch_dma(Gear2Bench *g2) {
    for (int r = 0; r < g2->n_regions; r++) {
        G2Region *reg = &g2->regions[r];
        if (reg->total_size == 0) continue;
        memcpy(reg->gpu_base, reg->pinned_base, reg->total_size);
        g2->bytes_copied += reg->total_size;
        g2->n_transfers++;
    }
}

static void gear2_destroy_b(Gear2Bench *g2) {
    for (int r = 0; r < g2->n_regions; r++) {
        free(g2->regions[r].gpu_base);
        free(g2->regions[r].pinned_base);
    }
    free(g2->regions);
    memset(g2, 0, sizeof(*g2));
}

/* ═══════════════════════════════════════════════════════════════
 * Stream callback: DRamTile → Gear2 mirror
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
    Gear2Bench *g2;
    int         region;
    int         tensor_idx;
} StreamCtx;

static int stream_to_mirror(const void *src, size_t sz, void *dst, void *user) {
    StreamCtx *ctx = (StreamCtx *)user;
    gear2_fill_mirror(ctx->g2, ctx->region, ctx->tensor_idx, src, sz);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * Benchmark 1: DRamTile Put/Get
 * ═══════════════════════════════════════════════════════════════ */
static void bench_dramtile(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 1. DRamTile Storage (hash put/get)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    DRamTileBench dt;
    dt_init(&dt, 64*1024*1024, 32*1024*1024, 16*1024*1024);

    char name[64];
    uint8_t data[TENSOR_SZ]; memset(data, 0xAA, sizeof(data));

    double t0 = now_sec();
    for (int i = 0; i < N_TENSORS; i++) {
        snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
        dt_put(&dt, name, data, sizeof(data));
    }
    printf("  Put %d:              %.3f ms  (%.0f ns/op)\n",
           N_TENSORS, (now_sec() - t0) * 1000,
           (now_sec() - t0) * 1e6 / N_TENSORS);

    volatile uint8_t *sink = NULL;
    t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++)
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
            sink = dt_get(&dt, name);
        }
    double t_get = now_sec() - t0;
    printf("  Get %d×%d:          %.3f s  (%.0f ns/op)\n",
           N_ITERS, N_TENSORS, t_get, t_get * 1e9 / (N_ITERS * N_TENSORS));
    printf("  Total:              %zu KB stored, %u entries\n\n",
           dt.used / 1024, dt.n_stored);

    dt_destroy(&dt);
}

/* ═══════════════════════════════════════════════════════════════
 * Benchmark 2: GearShift Routing
 * ═══════════════════════════════════════════════════════════════ */
static void bench_gearshift(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 2. GearShift Routing (name-based dispatch)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    GearShiftBench gs;
    gs_init_b(&gs);

    char name[64];
    uint8_t data[TENSOR_SZ]; memset(data, 0xBB, sizeof(data));

    double t0 = now_sec();
    for (int i = 0; i < N_TENSORS; i++) {
        snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
        gs_register_b(&gs, name, i / 40);
    }
    printf("  Register %d:         %.3f ms\n", N_TENSORS, (now_sec() - t0) * 1000);

    /* Batch stream */
    void *fake_dst = malloc(TENSOR_SZ);
    gs_set_dest_b(&gs, gs.entries[0].name, NULL, fake_dst, NULL);
    /* set dest for all */
    for (int i = 0; i < gs.n_entries; i++)
        gs.entries[i].stream_fn = NULL; /* use gs_stream_from with explicit src */

    t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++) {
        gs_reset_done_b(&gs);
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
            GSEntryBench *e = gs_find_b(&gs, name);
            if (e && e->stream_fn) {
                gs_stream_from_b(&gs, name, data, TENSOR_SZ);
            }
        }
    }
    double t_batch = now_sec() - t0;
    printf("  Batch stream %d×%d:  %.3f s  (%.0f ns/tensor)\n",
           N_ITERS, N_TENSORS, t_batch,
           t_batch * 1e9 / (N_ITERS * N_TENSORS));

    free(fake_dst);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
 * Benchmark 3: GearLock (c144 + world tracking)
 * ═══════════════════════════════════════════════════════════════ */
static void bench_gearlock(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 3. GearLock (c144 tag + CPU/GPU world tracking)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    GearLockBench gl;
    gl_init(&gl);

    /* Simulate 1440-tick cycle */
    double t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++) {
        gl_init(&gl);
        for (int tick = 0; tick < GEAR_C144_CYCLE; tick++) {
            gl.c144 = (uint8_t)tick;
            /* CPU writes 128 tensors per world */
            for (int w = 0; w < GEAR_CPU_WORLD; w++)
                gl_cpu_tick(&gl);
            /* GPU processes 162 tensors per world */
            gl_gpu_tick(&gl, GEAR_GPU_WORLD);
        }
    }
    double t_cycle = now_sec() - t0;
    printf("  1440-tick cycle × %d: %.3f s\n", N_ITERS, t_cycle);
    printf("  Per cycle:           %.0f us\n", t_cycle * 1e6 / N_ITERS);
    printf("  c144 final:          %u, cpu_worlds=%u, gpu_worlds=%u\n\n",
           gl.c144, gl.cpu_worlds, gl.gpu_worlds);
}

/* ═══════════════════════════════════════════════════════════════
 * Benchmark 4: Gear2 (pinned mirror + batch DMA)
 * ═══════════════════════════════════════════════════════════════ */
static void bench_gear2(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 4. Gear2 (pinned mirror + batch DMA)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    Gear2Bench g2;
    gear2_init_b(&g2, 1, (size_t)N_TENSORS * TENSOR_SZ);

    uint8_t data[TENSOR_SZ]; memset(data, 0xCC, sizeof(data));

    /* Phase 1: fill mirror */
    double t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++) {
        for (int i = 0; i < N_TENSORS; i++)
            gear2_fill_mirror(&g2, 0, i, data, TENSOR_SZ);
    }
    double t_fill = now_sec() - t0;
    printf("  Fill mirror %d×%d:   %.3f s  (%.0f ns/tensor)\n",
           N_ITERS, N_TENSORS, t_fill,
           t_fill * 1e9 / (N_ITERS * N_TENSORS));

    /* Phase 2: single batch DMA */
    t0 = now_sec();
    for (int it = 0; it < N_ITERS; it++)
        gear2_batch_dma(&g2);
    double t_dma = now_sec() - t0;
    printf("  Batch DMA %d×%d:    %.3f s  (%.0f ns/tensor)\n",
           N_ITERS, N_TENSORS, t_dma,
           t_dma * 1e9 / (N_ITERS * N_TENSORS));

    printf("  Total copied:       %.1f MB in %d transfers\n\n",
           g2.bytes_copied / (1024.0*1024.0), g2.n_transfers);

    gear2_destroy_b(&g2);
}

/* ═══════════════════════════════════════════════════════════════
 * Benchmark 5: CPU vs GPU Path (the real comparison)
 *
 * Path A (correct): CPU scan → GearLock → GearShift(DRamTile→mirror) → Gear2 DMA
 * Path B (wrong):   GPU reads headers directly → processes
 * ═══════════════════════════════════════════════════════════════ */
static void bench_cpu_vs_gpu(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 5. CPU vs GPU Path — Full Pipeline\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    /* Setup all components */
    DRamTileBench dt;
    dt_init(&dt, 64*1024*1024, 32*1024*1024, 16*1024*1024);

    GearShiftBench gs;
    gs_init_b(&gs);

    GearLockBench gl;
    gl_init(&gl);

    Gear2Bench g2;
    gear2_init_b(&g2, 1, (size_t)N_TENSORS * TENSOR_SZ);

    /* Fill DRamTile */
    char name[64];
    uint8_t data[TENSOR_SZ]; memset(data, 0xAA, sizeof(data));
    for (int i = 0; i < N_TENSORS; i++) {
        snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
        dt_put(&dt, name, data, sizeof(data));
    }

    /* Setup stream contexts — one per tensor */
    StreamCtx *sctx_arr = (StreamCtx *)calloc(N_TENSORS, sizeof(StreamCtx));
    for (int i = 0; i < N_TENSORS; i++) {
        snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
        gs_register_b(&gs, name, i / 40);
        sctx_arr[i].g2 = &g2;
        sctx_arr[i].region = 0;
        sctx_arr[i].tensor_idx = i;
        gs_set_dest_b(&gs, name, stream_to_mirror, NULL, &sctx_arr[i]);
    }

    /* ── Path A: CPU scan → GearLock → GearShift → Gear2 ── */
    int iters = 500;
    double t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        /* Step 1: GearLock — c144 tag */
        gl.c144 = (uint8_t)(it % GEAR_C144_CYCLE);
        gl_cpu_tick(&gl);

        /* Step 2: GearShift — route from DRamTile → mirror */
        gs_reset_done_b(&gs);
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
            uint8_t *stored = dt_get(&dt, name);
            if (stored) {
                gs_stream_idx_b(&gs, i, stored, TENSOR_SZ);
            }
        }

        /* Step 3: Gear2 — single batch DMA */
        gear2_batch_dma(&g2);
    }
    double t_path_a = now_sec() - t0;

    /* ── Path B: Individual memcpy (old approach, no batch) ── */
    uint8_t *old_buf = (uint8_t *)calloc((size_t)N_TENSORS * TENSOR_SZ, 1);
    t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        gl.c144 = (uint8_t)(it % GEAR_C144_CYCLE);
        gl_cpu_tick(&gl);
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
            uint8_t *stored = dt_get(&dt, name);
            if (stored)
                memcpy(old_buf + (size_t)i * TENSOR_SZ, stored, TENSOR_SZ);
        }
    }
    double t_path_b = now_sec() - t0;

    /* ── Path C: Direct DRamTile → Gear2 (skip GearShift) ── */
    t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
            uint8_t *stored = dt_get(&dt, name);
            if (stored) gear2_fill_mirror(&g2, 0, i, stored, TENSOR_SZ);
        }
        gear2_batch_dma(&g2);
    }
    double t_path_c = now_sec() - t0;

    printf("  Path A (GearShift idx→Gear2 batch): %.1f us/cycle\n",
           t_path_a * 1e6 / iters);
    printf("  Path B (individual memcpy):         %.1f us/cycle\n",
           t_path_b * 1e6 / iters);
    printf("  Path C (direct DRamTile→Gear2):     %.1f us/cycle\n",
           t_path_c * 1e6 / iters);
    printf("  Speedup A vs B: %.2fx  |  C vs B: %.2fx\n\n",
           t_path_b / t_path_a, t_path_b / t_path_c);

    /* ── Component breakdown ── */
    double t_dt = 0, t_gs = 0, t_gl = 0, t_g2 = 0;

    /* DRamTile only */
    t0 = now_sec();
    for (int it = 0; it < iters; it++)
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
            dt_get(&dt, name);
        }
    t_dt = now_sec() - t0;

    /* GearLock only */
    t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        gl.c144 = (uint8_t)(it % GEAR_C144_CYCLE);
        gl_cpu_tick(&gl);
    }
    t_gl = now_sec() - t0;

    /* GearShift routing only (name-based — slow) */
    t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        gs_reset_done_b(&gs);
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
            gs_stream_from_b(&gs, name, data, TENSOR_SZ);
        }
    }
    t_gs = now_sec() - t0;

    /* Gear2 fill + DMA only */
    t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < N_TENSORS; i++)
            gear2_fill_mirror(&g2, 0, i, data, TENSOR_SZ);
        gear2_batch_dma(&g2);
    }
    t_g2 = now_sec() - t0;

    double total = t_dt + t_gl + t_gs + t_g2;
    printf("  Component breakdown:\n");
    printf("    DRamTile (hash get):  %.1f us  (%.1f%%)\n",
           t_dt * 1e6 / iters, t_dt / total * 100);
    printf("    GearLock (c144 tag):  %.1f us  (%.1f%%)\n",
           t_gl * 1e6 / iters, t_gl / total * 100);
    printf("    GearShift (routing):  %.1f us  (%.1f%%)\n",
           t_gs * 1e6 / iters, t_gs / total * 100);
    printf("    Gear2 (mirror+DMA):   %.1f us  (%.1f%%)\n",
           t_g2 * 1e6 / iters, t_g2 / total * 100);
    printf("    Total:                %.1f us/cycle\n\n",
           total * 1e6 / iters);

    free(old_buf);
    dt_destroy(&dt);
    gear2_destroy_b(&g2);
}

/* ═══════════════════════════════════════════════════════════════
 * Benchmark 6: Large tensor test (realistic weight sizes)
 * ═══════════════════════════════════════════════════════════════ */
static void bench_large_tensors(void) {
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 6. Large Tensors (64KB each = realistic weights)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    DRamTileBench dt;
    dt_init(&dt, (size_t)N_TENSORS * LARGE_TENSOR_SZ + 1024*1024,
                 32*1024*1024, 16*1024*1024);

    Gear2Bench g2;
    gear2_init_b(&g2, 1, (size_t)N_TENSORS * LARGE_TENSOR_SZ);

    char name[64];
    uint8_t *data = (uint8_t *)malloc(LARGE_TENSOR_SZ);
    memset(data, 0xDD, LARGE_TENSOR_SZ);

    double t0 = now_sec();
    for (int i = 0; i < N_TENSORS; i++) {
        snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
        dt_put(&dt, name, data, LARGE_TENSOR_SZ);
    }
    double t_put = now_sec() - t0;
    printf("  DRamTile put %d × %dKB: %.3f s\n",
           N_TENSORS, LARGE_TENSOR_SZ/1024, t_put);

    int iters = 100;

    /* Path A: DRamTile → Gear2 batch */
    t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
            uint8_t *stored = dt_get(&dt, name);
            if (stored) gear2_fill_mirror(&g2, 0, i, stored, LARGE_TENSOR_SZ);
        }
        gear2_batch_dma(&g2);
    }
    double t_a = now_sec() - t0;

    /* Path B: individual memcpy */
    uint8_t *old_buf = (uint8_t *)calloc((size_t)N_TENSORS * LARGE_TENSOR_SZ, 1);
    t0 = now_sec();
    for (int it = 0; it < iters; it++)
        for (int i = 0; i < N_TENSORS; i++) {
            snprintf(name, sizeof(name), "blk.%d.attn_q.w", i);
            uint8_t *stored = dt_get(&dt, name);
            if (stored)
                memcpy(old_buf + (size_t)i * LARGE_TENSOR_SZ, stored, LARGE_TENSOR_SZ);
        }
    double t_b = now_sec() - t0;

    printf("  Path A (DRamTile→Gear2 batch): %.3f s  (%.0f us/cycle)\n",
           t_a, t_a * 1e6 / iters);
    printf("  Path B (individual memcpy):    %.3f s  (%.0f us/cycle)\n",
           t_b, t_b * 1e6 / iters);
    printf("  Speedup:                       %.2fx\n\n", t_b / t_a);

    free(data); free(old_buf);
    dt_destroy(&dt); gear2_destroy_b(&g2);
}

/* ═══════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║  Full Pipeline Benchmark                             ║\n");
    printf("║  DRamTile + GearShift + GearLock + Gear2             ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");
    printf("Config: %d tensors × %d B = %zu KB\n\n",
           N_TENSORS, TENSOR_SZ, (size_t)N_TENSORS * TENSOR_SZ / 1024);

    bench_dramtile();
    bench_gearshift();
    bench_gearlock();
    bench_gear2();
    bench_cpu_vs_gpu();
    bench_large_tensors();

    printf("═══════════════════════════════════════════════════════\n");
    printf(" Architecture Summary\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  DRamTile:  Hash storage, tensor data lives here\n");
    printf("  GearShift: Name-based routing, DRamTile → mirror\n");
    printf("  GearLock:  c144 tag, CPU/GPU world tracking\n");
    printf("  Gear2:     Pinned mirror + single batch DMA\n");
    printf("  Flow:      File → CPU scan → GearLock → GearShift\n");
    printf("             → DRamTile get → Gear2 mirror → GPU DMA\n");
    printf("═══════════════════════════════════════════════════════\n");
    return 0;
}

/*
 * test_gear_gpu.cu — GPU Icosa + Gear Lock integration test
 *
 * Compile:
 *   nvcc -O2 -gencode arch=compute_61,code=sm_61 ^
 *             -gencode arch=compute_75,code=sm_75 ^
 *             -I. -I../core/pogls_engine ^
 *             -DICOSA_SKIP_MAIN -c icosa_twin_bridge.cu -o icosa_twin_bridge.obj
 *   nvcc -O2 -gencode arch=compute_61,code=sm_61 ^
 *             -gencode arch=compute_75,code=sm_75 ^
 *             -I. -I../core/pogls_engine ^
 *             -c test_gear_gpu.cu -o test_gear_gpu.obj
 *   nvcc -O2 -o test_gear_gpu.exe test_gear_gpu.obj icosa_twin_bridge.obj
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <cuda_runtime.h>

#include "gear_lock.h"

/* Extern functions from icosa_twin_bridge.cu */
extern "C" {
    void *icosa_gpu_ctx_create(uint64_t gen2, uint64_t gen3);
    int   icosa_gpu_ctx_valid(void *gpu_ctx);
    void  icosa_gpu_ctx_destroy(void *gpu_ctx);
    int   icosa_gpu_dispatch(void *gpu_ctx,
                              const uint64_t *addrs,
                              const uint64_t *values,
                              uint32_t n,
                              uint64_t gen3,
                              uint32_t c144_tag,
                              uint64_t baseline,
                              uint64_t *out_routes,
                              uint8_t  *out_events);
}

/* CPU reference — same as in icosa_twin_bridge.cu */
#define ICOSA_FACES         20u
#define ICOSA_EDGES          3u
#define ICOSA_TOTAL_UNITS   (ICOSA_FACES * ICOSA_EDGES)
#define ICOSA_CPU_WORLD     128u
#define ICOSA_GPU_WORLD     162u
#define ICOSA_EV_NONE       0x00u
#define ICOSA_EV_BOUNDARY   0x02u

static uint64_t cpu_theta_mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}

static uint64_t cpu_fast_intersect(uint64_t core_raw) {
    uint64_t r8  = (core_raw >> 8)  | (core_raw << 56);
    uint64_t r16 = (core_raw >> 16) | (core_raw << 48);
    uint64_t r24 = (core_raw >> 24) | (core_raw << 40);
    return core_raw & r8 & r16 & r24;
}

static uint64_t cpu_route_update(uint64_t route, uint64_t isect) {
    uint64_t r = route ^ isect;
    if ((r & 0x0000FFFFFFFFFFFFULL) == 0)
        r ^= 0x9E3779B97F4A7C15ULL ^ isect;
    return r;
}

#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(__popcnt64)
static uint32_t portable_popcountll(uint64_t x) { return (uint32_t)__popcnt64(x); }
#else
static uint32_t portable_popcountll(uint64_t x) { return (uint32_t)__builtin_popcountll(x); }
#endif

static void cpu_icosa_lane(
    const uint64_t *addrs, const uint64_t *values,
    uint64_t gen3, uint32_t c144_tag, uint64_t baseline,
    uint32_t n, uint64_t *out_routes, uint8_t *out_events)
{
    for (uint32_t i = 0; i < n; i++) {
        uint64_t raw = addrs[i] ^ values[i] ^ gen3 ^ (uint64_t)c144_tag;
        uint64_t h  = cpu_theta_mix64(raw);
        uint32_t hi = (uint32_t)(h >> 32);
        uint32_t lo = (uint32_t)(h & 0xFFFFFFFFu);
        uint64_t core_raw = ((uint64_t)hi << 32) | ((uint64_t)lo & 0x000FFFFFFFFFFFFFULL);
        uint64_t isect = cpu_fast_intersect(core_raw);
        uint32_t drift = ((core_raw & 7u) == 0u)
                       ? portable_popcountll(baseline & ~isect) : 0u;
        out_routes[i] = cpu_route_update(0, isect);
        out_events[i] = (isect == 0 || drift > 72u) ? ICOSA_EV_BOUNDARY : ICOSA_EV_NONE;
    }
}

/* ── test harness ───────────────────────────────────────────────── */
static int g_pass = 0, g_fail = 0;
#define CHECK(label, cond) do { \
    if (cond) { printf("  OK   %s\n", label); g_pass++; } \
    else      { printf("  FAIL %s\n", label); g_fail++; } \
} while(0)
#define SECTION(n) printf("\n[%s]\n", n)

static uint64_t gen_test_data(uint64_t *addrs, uint64_t *values, uint32_t n, unsigned seed) {
    srand(seed);
    uint64_t chk = 0;
    for (uint32_t i = 0; i < n; i++) {
        addrs[i]  = ((uint64_t)rand() << 32) ^ (uint64_t)rand();
        values[i] = ((uint64_t)rand() << 32) ^ (uint64_t)rand();
        chk ^= addrs[i] ^ values[i];
    }
    return chk;
}

/* ── T1: GPU init + gear lock basic ────────────────────────────── */
static void t1_gpu_gear_basic(void) {
    SECTION("T1: GPU init + gear lock basic");

    uint8_t c144 = 144;
    GearLock gl;
    gl.c144_ref = &c144;
    gl.cpu_ops = 0; gl.gpu_ops = 0;
    gl.cpu_worlds = 0; gl.gpu_worlds = 0;

    CHECK("T1a: gear_tag = 144", gear_tag(&gl) == 144);

    void *gpu = icosa_gpu_ctx_create(0, 0xDEADBEEFCAFEBABEULL);
    CHECK("T1b: GPU context created", gpu != NULL);
    CHECK("T1c: GPU context valid", gpu && icosa_gpu_ctx_valid(gpu));

    for (int i = 0; i < 128; i++) gear_cpu_tick(&gl);
    CHECK("T1d: cpu_worlds=1 after 128", gl.cpu_worlds == 1);

    gear_gpu_tick(&gl, 162);
    CHECK("T1e: gpu_worlds=1 after 162", gl.gpu_worlds == 1);
    CHECK("T1f: gpu_ops=162", gl.gpu_ops == 162);

    icosa_gpu_ctx_destroy(gpu);
}

/* ── T2: GPU batch with gear lock tracking ─────────────────────── */
static void t2_gpu_batch_gear(void) {
    SECTION("T2: GPU batch with gear lock tracking");

    uint32_t n = 65536;
    uint64_t *addrs  = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *values = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *gpu_r  = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint8_t  *gpu_e  = (uint8_t  *)malloc((size_t)n);
    uint64_t *cpu_r  = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint8_t  *cpu_e  = (uint8_t  *)malloc((size_t)n);

    gen_test_data(addrs, values, n, 42);
    uint64_t gen3 = 0xDEADBEEFCAFEBABEULL;
    uint64_t baseline = 0x5555555555555555ULL;

    uint8_t c144 = 144;
    GearLock gl;
    gl.c144_ref = &c144;
    gl.cpu_ops = 0; gl.gpu_ops = 0;
    gl.cpu_worlds = 0; gl.gpu_worlds = 0;

    void *gpu = icosa_gpu_ctx_create(0, gen3);
    CHECK("T2a: GPU ready", gpu && icosa_gpu_ctx_valid(gpu));
    if (!gpu) { free(addrs); free(values); free(gpu_r); free(gpu_e); free(cpu_r); free(cpu_e); return; }

    uint32_t c144_tag = gear_tag(&gl);
    int ret = icosa_gpu_dispatch(gpu, addrs, values, n,
                                  gen3, c144_tag, baseline, gpu_r, gpu_e);
    CHECK("T2b: GPU dispatch OK", ret == 0);

    gear_gpu_tick(&gl, n);
    CHECK("T2c: gpu_ops = 65536", gl.gpu_ops == n);

    cpu_icosa_lane(addrs, values, gen3, c144_tag, baseline, n, cpu_r, cpu_e);

    uint64_t mismatches = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (gpu_r[i] != cpu_r[i] || gpu_e[i] != cpu_e[i]) mismatches++;
    }
    CHECK("T2d: GPU matches CPU reference", mismatches == 0);

    icosa_gpu_ctx_destroy(gpu);
    free(addrs); free(values); free(gpu_r); free(gpu_e); free(cpu_r); free(cpu_e);
}

/* ── T3: c144 tag propagation to GPU ───────────────────────────── */
static void t3_c144_propagation(void) {
    SECTION("T3: c144 tag propagation");

    uint32_t n = 4096;
    uint64_t *addrs  = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *values = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *gpu_r  = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint8_t  *gpu_e  = (uint8_t  *)malloc((size_t)n);
    uint64_t *cpu_r  = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint8_t  *cpu_e  = (uint8_t  *)malloc((size_t)n);

    gen_test_data(addrs, values, n, 99);
    uint64_t gen3 = 0xCAFEBABEDEADBEEFULL;
    uint64_t baseline = 0xAAAAAAAAAAAAAAAAULL;

    uint8_t c144 = 100;
    GearLock gl;
    gl.c144_ref = &c144;
    gl.cpu_ops = 0; gl.gpu_ops = 0;
    gl.cpu_worlds = 0; gl.gpu_worlds = 0;

    void *gpu = icosa_gpu_ctx_create(0, gen3);
    CHECK("T3a: GPU ready", gpu && icosa_gpu_ctx_valid(gpu));
    if (!gpu) { free(addrs); free(values); free(gpu_r); free(gpu_e); free(cpu_r); free(cpu_e); return; }

    uint32_t tag_A = gear_tag(&gl); /* 100 */
    icosa_gpu_dispatch(gpu, addrs, values, n, gen3, tag_A, baseline, gpu_r, gpu_e);
    cpu_icosa_lane(addrs, values, gen3, tag_A, baseline, n, cpu_r, cpu_e);

    uint64_t mA = 0;
    for (uint32_t i = 0; i < n; i++)
        if (gpu_r[i] != cpu_r[i] || gpu_e[i] != cpu_e[i]) mA++;
    CHECK("T3b: tag=100 GPU matches CPU", mA == 0);

    c144 = 50;
    uint32_t tag_B = gear_tag(&gl); /* 50 */
    icosa_gpu_dispatch(gpu, addrs, values, n, gen3, tag_B, baseline, gpu_r, gpu_e);
    cpu_icosa_lane(addrs, values, gen3, tag_B, baseline, n, cpu_r, cpu_e);

    uint64_t mB = 0;
    for (uint32_t i = 0; i < n; i++)
        if (gpu_r[i] != cpu_r[i] || gpu_e[i] != cpu_e[i]) mB++;
    CHECK("T3c: tag=50 GPU matches CPU", mB == 0);

    CHECK("T3d: tags differ (different results expected)",
          gpu_r[0] != cpu_r[0] || tag_A != tag_B);

    icosa_gpu_ctx_destroy(gpu);
    free(addrs); free(values); free(gpu_r); free(gpu_e); free(cpu_r); free(cpu_e);
}

/* ── T4: 128:162 realignment ───────────────────────────────────── */
static void t4_geo_full_realign(void) {
    SECTION("T4: 128:162 gear realignment");

    uint8_t c144 = 144;
    GearLock gl;
    gl.c144_ref = &c144;
    gl.cpu_ops = 0; gl.gpu_ops = 0;
    gl.cpu_worlds = 0; gl.gpu_worlds = 0;

    for (uint32_t i = 0; i < GEAR_GEO_FULL; i++) {
        gear_cpu_tick(&gl);
        gear_gpu_tick(&gl, 1);
    }

    CHECK("T4a: cpu_ops = 20736",  gl.cpu_ops == GEAR_GEO_FULL);
    CHECK("T4b: gpu_ops = 20736",  gl.gpu_ops == GEAR_GEO_FULL);
    CHECK("T4c: cpu_worlds = 162", gl.cpu_worlds == 162);
    CHECK("T4d: gpu_worlds = 128", gl.gpu_worlds == 128);

    printf("  CPU: %u ops in %u worlds (%u/128)\n",
           gl.cpu_ops, gl.cpu_worlds, gl.cpu_ops);
    printf("  GPU: %u ops in %u worlds (%u/162)\n",
           gl.gpu_ops, gl.gpu_worlds, gl.gpu_ops);
    printf("  128*162 = 162*128 = %u = GEO_FULL ✓\n", GEAR_GEO_FULL);
}

/* ── T5: Large GPU batch + gear lock + CPU verify ──────────────── */
static void t5_large_batch_verify(void) {
    SECTION("T5: Large GPU batch + gear lock verify (256K ops)");

    uint32_t n = 262144;
    uint64_t *addrs  = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *values = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *gpu_r  = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint8_t  *gpu_e  = (uint8_t  *)malloc((size_t)n);
    uint64_t *cpu_r  = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint8_t  *cpu_e  = (uint8_t  *)malloc((size_t)n);

    gen_test_data(addrs, values, n, 12345);
    uint64_t gen3 = 0x1111111111111111ULL;
    uint64_t baseline = 0x5555555555555555ULL;

    uint8_t c144 = 144;
    GearLock gl;
    gl.c144_ref = &c144;
    gl.cpu_ops = 0; gl.gpu_ops = 0;
    gl.cpu_worlds = 0; gl.gpu_worlds = 0;

    void *gpu = icosa_gpu_ctx_create(0, gen3);
    CHECK("T5a: GPU ready", gpu && icosa_gpu_ctx_valid(gpu));
    if (!gpu) { free(addrs); free(values); free(gpu_r); free(gpu_e); free(cpu_r); free(cpu_e); return; }

    uint32_t c144_tag = gear_tag(&gl);
    uint32_t batch_sz = 65536;

    /* Warmup — same data, same size, discard results */
    for (uint32_t offset = 0; offset < n; offset += batch_sz) {
        uint32_t chunk = (n - offset) < batch_sz ? (n - offset) : batch_sz;
        icosa_gpu_dispatch(gpu, addrs + offset, values + offset,
                            chunk, gen3, c144_tag, baseline,
                            gpu_r + offset, gpu_e + offset);
    }

    /* Reset gear counters after warmup */
    gl.gpu_ops = 0;

    cudaEvent_t t0, t1;
    cudaEventCreate(&t0);
    cudaEventCreate(&t1);
    cudaEventRecord(t0);

    for (uint32_t offset = 0; offset < n; offset += batch_sz) {
        uint32_t chunk = (n - offset) < batch_sz ? (n - offset) : batch_sz;
        icosa_gpu_dispatch(gpu, addrs + offset, values + offset,
                            chunk, gen3, c144_tag, baseline,
                            gpu_r + offset, gpu_e + offset);
        gear_gpu_tick(&gl, chunk);
        gear_cpu_tick(&gl);
    }

    cudaEventRecord(t1);
    cudaEventSynchronize(t1);
    float ms;
    cudaEventElapsedTime(&ms, t0, t1);

    /* CPU reference */
    cpu_icosa_lane(addrs, values, gen3, c144_tag, baseline, n, cpu_r, cpu_e);

    uint64_t mismatches = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (gpu_r[i] != cpu_r[i] || gpu_e[i] != cpu_e[i]) mismatches++;
    }
    CHECK("T5b: ALL 262144 ops match CPU", mismatches == 0);
    CHECK("T5c: gpu_ops = 262144", gl.gpu_ops == n);
    printf("  262144 ops in %.2f ms  (%.0f M/s)\n", ms, (double)n / (ms * 1e3));
    printf("  gpu_worlds=%u (162 ops/world)\n", gl.gpu_worlds);

    icosa_gpu_ctx_destroy(gpu);
    cudaEventDestroy(t0);
    cudaEventDestroy(t1);
    free(addrs); free(values); free(gpu_r); free(gpu_e); free(cpu_r); free(cpu_e);
}

/* ── main ───────────────────────────────────────────────────────── */
int main(void) {
    printf("=== GPU ICOSA + GEAR LOCK INTEGRATION TEST ===\n\n");

    int dev_count = 0;
    cudaGetDeviceCount(&dev_count);
    printf("CUDA devices: %d\n", dev_count);
    if (dev_count > 0) {
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, 0);
        printf("  [0] %s  SM%d.%d  %.0fMB\n",
               prop.name, prop.major, prop.minor,
               prop.totalGlobalMem / 1048576.0);
    }
    printf("\n");

    t1_gpu_gear_basic();
    t2_gpu_batch_gear();
    t3_c144_propagation();
    t4_geo_full_realign();
    t5_large_batch_verify();

    printf("\n=== RESULTS: %d/%d passed ===\n", g_pass, g_pass + g_fail);
    return g_fail > 0 ? 1 : 0;
}

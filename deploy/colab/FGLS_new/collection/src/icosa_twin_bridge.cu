/*
 * icosa_twin_bridge.cu — Icosa GPU Lane + Twin Bridge (CUDA)
 *
 * Dodeca (CPU) ↔ Icosa (GPU) dual polyhedra.
 * Icosa = 20 triangular faces, 3 edges/face = 60 total edge units.
 * Dodeca = 12 pentagon faces, 5 edges/face = 60 total edge units.
 *
 * The GPU kernel processes the icosa lane in parallel with the CPU
 * dodeca lane. Results are zero-copy by dual polyhedron nature.
 *
 * Compile (GTX 1050 Ti, sm_61):
 *   nvcc -O2 -arch=sm_61 -o icosa_twin_bridge icosa_twin_bridge.cu
 *
 * Compile (Colab T4, sm_75):
 *   nvcc -O2 -arch=sm_75 -o icosa_twin_bridge icosa_twin_bridge.cu
 *
 * Compile (both architectures):
 *   nvcc -O2 -gencode arch=compute_61,code=sm_61 ^
 *             -gencode arch=compute_75,code=sm_75 ^
 *             -o icosa_twin_bridge icosa_twin_bridge.cu
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cuda_runtime.h>
#include <cuda.h>

#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(__popcnt64)
static inline uint32_t portable_popcountll(uint64_t x) {
    return (uint32_t)__popcnt64(x);
}
#else
static inline uint32_t portable_popcountll(uint64_t x) {
    return (uint32_t)__builtin_popcountll(x);
}
#endif

#ifndef ICOSA_TWIN_BRIDGE_IMPL
#define ICOSA_TWIN_BRIDGE_IMPL
#endif

#define ICOSA_FACES         20u
#define ICOSA_EDGES          3u
#define ICOSA_GPU_TPB      256u
#define ICOSA_GPU_BATCH   65536u
#define ICOSA_CPU_WORLD     128u
#define ICOSA_GPU_WORLD     162u

#define ICOSA_EV_NONE      0x00u
#define ICOSA_EV_FLUSH     0x01u
#define ICOSA_EV_BOUNDARY  0x02u

/* ═══════════════════════════════════════════════════════════════════
 * DEVICE FUNCTIONS — Icosa Lane Compute
 * ═══════════════════════════════════════════════════════════════════ */

__device__ __forceinline__
uint64_t d_theta_mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

__device__ __forceinline__
uint64_t d_fast_intersect(uint64_t core_raw) {
    uint64_t r8  = (core_raw >> 8)  | (core_raw << 56);
    uint64_t r16 = (core_raw >> 16) | (core_raw << 48);
    uint64_t r24 = (core_raw >> 24) | (core_raw << 40);
    return core_raw & r8 & r16 & r24;
}

__device__ __forceinline__
uint64_t d_route_update(uint64_t route, uint64_t isect) {
    uint64_t r = route ^ isect;
    if ((r & 0x0000FFFFFFFFFFFFULL) == 0)
        r ^= 0x9E3779B97F4A7C15ULL ^ isect;
    return r;
}

/* ═══════════════════════════════════════════════════════════════════
 * ICOSA LANE KERNEL
 *
 * Each thread processes one (addr, value) pair through the icosa
 * geometry lane: theta_mix64 → face%20, edge%3 → fast_intersect
 * → route_update → boundary check.
 *
 * Input:  pairs[i] = {addr, value}
 * Output: out_route[i] = route_addr after this op
 *         out_event[i] = ICOSA_EV_BOUNDARY if flow flushed
 *
 * gen3, c144_tag, baseline are uniform across the batch.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t addr;
    uint64_t value;
} IcosaPair;

__global__ void icosa_lane_kernel(
    const IcosaPair * __restrict__ pairs,
    uint64_t         * __restrict__ out_route,
    uint8_t          * __restrict__ out_event,
    uint64_t          gen3,
    uint32_t          c144_tag,
    uint64_t          baseline,
    uint32_t          n)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;

    uint64_t raw   = pairs[i].addr ^ pairs[i].value ^ gen3 ^ (uint64_t)c144_tag;

    uint64_t h     = d_theta_mix64(raw);
    uint32_t hi    = (uint32_t)(h >> 32);
    uint32_t lo    = (uint32_t)(h & 0xFFFFFFFFu);

    uint64_t core_raw = ((uint64_t)hi << 32)
                      | ((uint64_t)lo & 0x000FFFFFFFFFFFFFULL);
    uint64_t isect = d_fast_intersect(core_raw);

    uint32_t drift = ((core_raw & 7u) == 0u)
                   ? (uint32_t)__popcll(baseline & ~isect)
                   : 0u;

    out_route[i] = d_route_update(0, isect);

    int at_end = (isect == 0) || (drift > 72u);
    out_event[i] = at_end ? ICOSA_EV_BOUNDARY : ICOSA_EV_NONE;
}

/* ═══════════════════════════════════════════════════════════════════
 * GPU CONTEXT
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t  gen2;
    uint64_t  gen3;
    int       valid;

    IcosaPair *d_pairs;
    uint64_t  *d_route;
    uint8_t   *d_event;
    IcosaPair *h_pairs;
    uint64_t  *h_route;
    uint8_t   *h_event;

    uint32_t   capacity;
    uint32_t   count;
    cudaStream_t stream;
    CUcontext  cu_ctx;
} IcosaGpuCtx;

#ifdef _WIN32
  #ifdef ICOSA_BUILD_DLL
    #define ICOSA_API __declspec(dllexport)
  #else
    #define ICOSA_API
  #endif
#else
  #ifdef ICOSA_BUILD_DLL
    #define ICOSA_API __attribute__((visibility("default")))
  #else
    #define ICOSA_API
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

ICOSA_API void icosa_gpu_ctx_destroy(void *gpu_ctx);

ICOSA_API void *icosa_gpu_ctx_create(uint64_t gen2, uint64_t gen3) {
    IcosaGpuCtx *ctx = (IcosaGpuCtx *)calloc(1, sizeof(IcosaGpuCtx));
    if (!ctx) return NULL;

    ctx->gen2 = gen2;
    ctx->gen3 = gen3;
    ctx->capacity = ICOSA_GPU_BATCH;
    ctx->count = 0;
    ctx->valid = 0;

    int dev_count = 0;
    if (cudaGetDeviceCount(&dev_count) != cudaSuccess || dev_count == 0) {
        printf("[icosa] No CUDA device found\n");
        free(ctx);
        return NULL;
    }

    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("[icosa] GPU: %s  SM%d.%d  %.0fMB\n",
           prop.name, prop.major, prop.minor,
           prop.totalGlobalMem / 1048576.0);

    size_t pair_sz  = (size_t)ctx->capacity * sizeof(IcosaPair);
    size_t route_sz = (size_t)ctx->capacity * sizeof(uint64_t);
    size_t ev_sz    = (size_t)ctx->capacity * sizeof(uint8_t);

    cudaError_t e;

    e = cudaMallocHost(&ctx->h_pairs, pair_sz);
    if (e != cudaSuccess) { printf("[icosa] h_pairs alloc fail\n"); goto fail; }

    e = cudaMallocHost(&ctx->h_route, route_sz);
    if (e != cudaSuccess) { printf("[icosa] h_route alloc fail\n"); goto fail; }

    e = cudaMallocHost(&ctx->h_event, ev_sz);
    if (e != cudaSuccess) { printf("[icosa] h_event alloc fail\n"); goto fail; }

    e = cudaMalloc(&ctx->d_pairs, pair_sz);
    if (e != cudaSuccess) { printf("[icosa] d_pairs alloc fail\n"); goto fail; }

    e = cudaMalloc(&ctx->d_route, route_sz);
    if (e != cudaSuccess) { printf("[icosa] d_route alloc fail\n"); goto fail; }

    e = cudaMalloc(&ctx->d_event, ev_sz);
    if (e != cudaSuccess) { printf("[icosa] d_event alloc fail\n"); goto fail; }

    e = cudaStreamCreate(&ctx->stream);
    if (e != cudaSuccess) { printf("[icosa] stream create fail\n"); goto fail; }

    /* Save CUDA context for push/pop around dispatch */
    cuCtxGetCurrent(&ctx->cu_ctx);

    ctx->valid = 1;
    printf("[icosa] GPU context ready  capacity=%u\n", ctx->capacity);
    return (void *)ctx;

fail:
    icosa_gpu_ctx_destroy((void *)ctx);
    return NULL;
}

ICOSA_API int icosa_gpu_ctx_valid(void *gpu_ctx) {
    if (!gpu_ctx) return 0;
    return ((IcosaGpuCtx *)gpu_ctx)->valid;
}

void icosa_gpu_ctx_destroy(void *gpu_ctx) {
    if (!gpu_ctx) return;
    IcosaGpuCtx *ctx = (IcosaGpuCtx *)gpu_ctx;

    if (ctx->d_pairs) cudaFree(ctx->d_pairs);
    if (ctx->d_route) cudaFree(ctx->d_route);
    if (ctx->d_event) cudaFree(ctx->d_event);
    if (ctx->h_pairs) cudaFreeHost(ctx->h_pairs);
    if (ctx->h_route) cudaFreeHost(ctx->h_route);
    if (ctx->h_event) cudaFreeHost(ctx->h_event);
    if (ctx->stream)  cudaStreamDestroy(ctx->stream);

    free(ctx);
}

/* ═══════════════════════════════════════════════════════════════════
 * GPU DISPATCH — process a batch through icosa lane
 *
 * Returns 0 on success, non-zero on error.
 * out_routes and out_events must be pre-allocated (n elements each).
 * ═══════════════════════════════════════════════════════════════════ */

static int _dispatch_chunk(
    IcosaGpuCtx    *ctx,
    const uint64_t *addrs,
    const uint64_t *values,
    uint32_t        n,
    uint64_t        gen3,
    uint32_t        c144_tag,
    uint64_t        baseline,
    uint64_t       *out_routes,
    uint8_t        *out_events)
{
    /* Save previous context and switch to ours (ggml-cuda.dll may have changed it) */
    CUcontext prev_ctx = NULL;
    cuCtxGetCurrent(&prev_ctx);
    if (prev_ctx != ctx->cu_ctx) cuCtxSetCurrent(ctx->cu_ctx);
    int restore_ctx = (prev_ctx != ctx->cu_ctx && prev_ctx != NULL);

    int ret = 0;
    cudaError_t e;
    dim3 grid((n + ICOSA_GPU_TPB - 1) / ICOSA_GPU_TPB, 1, 1);
    dim3 block(ICOSA_GPU_TPB, 1, 1);

    for (uint32_t i = 0; i < n; i++) {
        ctx->h_pairs[i].addr  = addrs[i];
        ctx->h_pairs[i].value = values[i];
    }

    e = cudaMemcpyAsync(ctx->d_pairs, ctx->h_pairs,
                        (size_t)n * sizeof(IcosaPair),
                        cudaMemcpyHostToDevice, ctx->stream);
    if (e != cudaSuccess) { ret = -3; goto done; }

    /* Clear any pending CUDA errors before kernel launch */
    cudaGetLastError();

    icosa_lane_kernel<<<grid, block, 0, ctx->stream>>>(
        ctx->d_pairs, ctx->d_route, ctx->d_event,
        gen3, c144_tag, baseline, n);

    e = cudaGetLastError();
    if (e != cudaSuccess) {
        printf("[icosa] kernel launch error: %d (%s)\n", e, cudaGetErrorString(e));
        ret = -4; goto done;
    }

    e = cudaMemcpyAsync(ctx->h_route, ctx->d_route,
                        (size_t)n * sizeof(uint64_t),
                        cudaMemcpyDeviceToHost, ctx->stream);
    if (e != cudaSuccess) { ret = -5; goto done; }

    e = cudaMemcpyAsync(ctx->h_event, ctx->d_event,
                        (size_t)n * sizeof(uint8_t),
                        cudaMemcpyDeviceToHost, ctx->stream);
    if (e != cudaSuccess) { ret = -6; goto done; }

    e = cudaStreamSynchronize(ctx->stream);
    if (e != cudaSuccess) { ret = -7; goto done; }

    memcpy(out_routes, ctx->h_route, (size_t)n * sizeof(uint64_t));
    memcpy(out_events, ctx->h_event, (size_t)n * sizeof(uint8_t));

done:
    if (restore_ctx) cuCtxSetCurrent(prev_ctx);
    return ret;
}

ICOSA_API int icosa_gpu_dispatch(
    void            *gpu_ctx,
    const uint64_t  *addrs,
    const uint64_t  *values,
    uint32_t         n,
    uint64_t         gen3,
    uint32_t         c144_tag,
    uint64_t         baseline,
    uint64_t        *out_routes,
    uint8_t         *out_events)
{
    if (!gpu_ctx || !addrs || !values || n == 0) return -1;
    IcosaGpuCtx *ctx = (IcosaGpuCtx *)gpu_ctx;
    if (!ctx->valid) return -2;

    uint32_t cap = ctx->capacity;
    uint32_t done = 0;
    while (done < n) {
        uint32_t chunk = (n - done) < cap ? (n - done) : cap;
        int ret = _dispatch_chunk(ctx, addrs + done, values + done,
                                   chunk, gen3, c144_tag, baseline,
                                   out_routes + done, out_events + done);
        if (ret != 0) return ret;
        done += chunk;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

/* ═══════════════════════════════════════════════════════════════════
 * CPU REFERENCE — same computation for verification
 * ═══════════════════════════════════════════════════════════════════ */

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

static void cpu_icosa_lane(
    const uint64_t *addrs,
    const uint64_t *values,
    uint64_t        gen3,
    uint32_t        c144_tag,
    uint64_t        baseline,
    uint32_t        n,
    uint64_t       *out_routes,
    uint8_t        *out_events)
{
    for (uint32_t i = 0; i < n; i++) {
        uint64_t raw = addrs[i] ^ values[i] ^ gen3 ^ (uint64_t)c144_tag;

        uint64_t h  = cpu_theta_mix64(raw);
        uint32_t hi = (uint32_t)(h >> 32);
        uint32_t lo = (uint32_t)(h & 0xFFFFFFFFu);

        uint64_t core_raw = ((uint64_t)hi << 32)
                          | ((uint64_t)lo & 0x000FFFFFFFFFFFFFULL);
        uint64_t isect = cpu_fast_intersect(core_raw);

        uint32_t drift = ((core_raw & 7u) == 0u)
                       ? portable_popcountll(baseline & ~isect)
                       : 0u;

        out_routes[i] = cpu_route_update(0, isect);

        int at_end = (isect == 0) || (drift > 72u);
        out_events[i] = at_end ? ICOSA_EV_BOUNDARY : ICOSA_EV_NONE;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * TEST main()
 * ═══════════════════════════════════════════════════════════════════ */

#define TEST_N  (1024 * 1024)

#ifndef ICOSA_SKIP_MAIN
int main(void) {
    printf("=== Icosa Twin Bridge — GPU Lane Test ===\n\n");

    /* Query GPU */
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

    /* Generate test data */
    uint32_t n = TEST_N;
    uint64_t *addrs   = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *values  = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *gpu_r   = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint8_t  *gpu_e   = (uint8_t  *)malloc((size_t)n);
    uint64_t *cpu_r   = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint8_t  *cpu_e   = (uint8_t  *)malloc((size_t)n);

    if (!addrs || !values || !gpu_r || !gpu_e || !cpu_r || !cpu_e) {
        printf("malloc fail\n");
        return 1;
    }

    /* Deterministic test data */
    uint64_t gen3     = 0xDEADBEEFCAFEBABEULL;
    uint32_t c144_tag = 42;
    uint64_t baseline = 0x5555555555555555ULL;

    srand(12345);
    for (uint32_t i = 0; i < n; i++) {
        addrs[i]  = ((uint64_t)rand() << 32) ^ (uint64_t)rand();
        values[i] = ((uint64_t)rand() << 32) ^ (uint64_t)rand();
    }

    /* CPU reference */
    printf("\nCPU reference...\n");
    cpu_icosa_lane(addrs, values, gen3, c144_tag, baseline, n, cpu_r, cpu_e);

    uint64_t cpu_boundaries = 0;
    for (uint32_t i = 0; i < n; i++)
        if (cpu_e[i] & ICOSA_EV_BOUNDARY) cpu_boundaries++;
    printf("  %u ops, %llu boundaries\n", n, (unsigned long long)cpu_boundaries);

    /* GPU test */
    printf("\nGPU dispatch...\n");
    void *gpu_ctx = icosa_gpu_ctx_create(0, gen3);
    if (!gpu_ctx) {
        printf("  GPU init failed (no CUDA?)\n");
        printf("\n=== RESULT: GPU unavailable, CPU-only ===\n");
        free(addrs); free(values); free(gpu_r); free(gpu_e);
        free(cpu_r); free(cpu_e);
        return 0;
    }

    cudaEvent_t t0, t1;
    cudaEventCreate(&t0);
    cudaEventCreate(&t1);

    /* Warmup */
    icosa_gpu_dispatch(gpu_ctx, addrs, values, 4096,
                        gen3, c144_tag, baseline, gpu_r, gpu_e);

    /* Benchmark */
    int reps = 10;
    cudaEventRecord(t0);
    for (int r = 0; r < reps; r++) {
        icosa_gpu_dispatch(gpu_ctx, addrs, values, n,
                            gen3, c144_tag, baseline, gpu_r, gpu_e);
    }
    cudaEventRecord(t1);
    cudaEventSynchronize(t1);
    float ms;
    cudaEventElapsedTime(&ms, t0, t1);
    ms /= (float)reps;

    double ops_per_sec = (double)n / (ms * 1e-3);
    printf("  %u ops in %.3f ms  =  %.0f M/s\n",
           n, ms, ops_per_sec / 1e6);

    uint64_t gpu_boundaries = 0;
    for (uint32_t i = 0; i < n; i++)
        if (gpu_e[i] & ICOSA_EV_BOUNDARY) gpu_boundaries++;
    printf("  %llu boundaries\n", (unsigned long long)gpu_boundaries);

    /* Verify against CPU reference */
    uint64_t mismatches = 0;
    uint64_t max_show = 10;
    for (uint32_t i = 0; i < n; i++) {
        if (gpu_r[i] != cpu_r[i]) {
            if (mismatches < max_show)
                printf("  MISMATCH [%u]: GPU=0x%016llx CPU=0x%016llx\n",
                       i, (unsigned long long)gpu_r[i],
                       (unsigned long long)cpu_r[i]);
            mismatches++;
        }
        if (gpu_e[i] != cpu_e[i]) {
            if (mismatches < max_show)
                printf("  EVENT MISMATCH [%u]: GPU=%u CPU=%u\n",
                       i, (unsigned)gpu_e[i], (unsigned)cpu_e[i]);
            mismatches++;
        }
    }

    printf("\n=== VERDICT ===\n");
    if (mismatches == 0) {
        printf("  ✓ ALL %u ops MATCH CPU reference\n", n);
    } else {
        printf("  ✗ %llu / %u mismatches\n",
               (unsigned long long)mismatches, n);
    }

    /* Benchmark: quad-core zen4 vs GPU */
    printf("\n  Icosa GPU: %.0f M/s  |  %u ops in %.3f ms\n",
           ops_per_sec / 1e6, n, ms);

    /* Cleanup */
    icosa_gpu_ctx_destroy(gpu_ctx);
    cudaEventDestroy(t0);
    cudaEventDestroy(t1);
    free(addrs); free(values);
    free(gpu_r); free(gpu_e);
    free(cpu_r); free(cpu_e);

    return mismatches > 0 ? 1 : 0;
}
#endif /* ICOSA_SKIP_MAIN */

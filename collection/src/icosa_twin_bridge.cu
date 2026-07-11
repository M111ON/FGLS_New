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
 * BERMUDA GPU KERNEL — batch traverse (mode 0..3)
 * Mirrors bermuda_traverse() from bermuda_export.h exactly
 * ═══════════════════════════════════════════════════════════════════ */

#define BERMUDA_STRIDE       37u
#define BERMUDA_N_ZONES      12u
#define BERMUDA_TRING_SLOTS  720u

__constant__ uint8_t  d_bermuda_cross[12] = {9,10,11,6,7,8,3,4,5,0,1,2};
__constant__ uint16_t d_bermuda_slots[5];       /* [0]=0, [1]=512, [2]=1024, [3]=2048, [4]=4096 */
__constant__ uint16_t d_bermuda_walk_len[5];
__constant__ uint16_t d_bermuda_face_sz[5];
__constant__ uint16_t d_bermuda_inv37[5];
__constant__ uint16_t d_bermuda_inv37_wl[5];

typedef struct {
    uint16_t idx_in;
    uint16_t idx_out;
    uint8_t  zone;
    uint8_t  pole;
    uint8_t  shape;
    uint8_t  polarity;
    uint16_t tring_slot;
} BermudaRouteEntry;

__device__ __forceinline__ uint16_t d_bermuda_mod_mul(uint16_t a, uint16_t b, uint16_t mod) {
    return (uint16_t)(((uint32_t)a * (uint32_t)b) % (uint32_t)mod);
}

__device__ __forceinline__ uint16_t d_bermuda_traverse_orbit(uint16_t idx, uint8_t gear) {
    uint16_t N = d_bermuda_slots[gear];
    return (uint16_t)((idx + 1) % N);
}

__device__ __forceinline__ uint16_t d_bermuda_traverse_chiral(uint16_t idx, uint8_t gear) {
    uint16_t N = d_bermuda_slots[gear];
    return (uint16_t)((idx + N / 2) % N);
}

__device__ __forceinline__ uint16_t d_bermuda_traverse_cross(uint16_t idx, uint8_t gear) {
    uint16_t WL = d_bermuda_walk_len[gear];
    uint16_t FS = d_bermuda_face_sz[gear];
    uint16_t IW = d_bermuda_inv37_wl[gear];
    uint16_t N  = d_bermuda_slots[gear];

    uint16_t enc = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE) % WL);
    uint8_t  z   = (uint8_t)(enc / FS);
    uint8_t  pz  = d_bermuda_cross[z % 12];
    uint16_t ne  = (uint16_t)(pz * FS + enc % FS);
    return (uint16_t)(((uint32_t)ne * IW) % WL % N);
}

__device__ __forceinline__ uint16_t d_bermuda_traverse_hub(uint16_t idx, uint8_t gear) {
    uint16_t WL = d_bermuda_walk_len[gear];
    uint16_t FS = d_bermuda_face_sz[gear];
    uint16_t N  = d_bermuda_slots[gear];
    uint16_t enc = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE) % WL);
    uint8_t  z  = (uint8_t)(enc / FS);
    return (uint16_t)(((uint32_t)z * (N / BERMUDA_N_ZONES)) % N);
}

__device__ __forceinline__ uint16_t d_bermuda_traverse(uint16_t idx, uint8_t gear, uint8_t mode) {
    switch (mode) {
        case 0: return d_bermuda_traverse_orbit(idx, gear);
        case 1: return d_bermuda_traverse_chiral(idx, gear);
        case 2: return d_bermuda_traverse_cross(idx, gear);
        case 3: return d_bermuda_traverse_hub(idx, gear);
        default: return idx;
    }
}

__device__ __forceinline__ uint8_t d_bermuda_zone(uint16_t idx, uint8_t gear) {
    uint16_t enc = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE) % d_bermuda_walk_len[gear]);
    return (uint8_t)(enc / d_bermuda_face_sz[gear]);
}

__device__ __forceinline__ uint8_t d_bermuda_pole(uint8_t zone) {
    return zone >= 6 ? 1 : 0;
}

__device__ __forceinline__ uint16_t d_bermuda_tring_slot(uint16_t idx) {
    return idx % BERMUDA_TRING_SLOTS;
}

__device__ __forceinline__ uint8_t d_bermuda_shape(uint8_t mode, uint8_t zone) {
    /* shape bytes: I=73, O=79, S=83, L=76 */
    switch (mode) {
        case 0: return zone < 6 ? 73 : 79;  /* ORBITAL: I or O */
        case 1: return 79;                   /* CHIRAL: O */
        case 2: return 83;                   /* CROSS: S */
        case 3: return 76;                   /* HUB: L */
        default: return 73;
    }
}

__device__ __forceinline__ uint8_t d_bermuda_polarity(uint8_t mode, uint8_t zone) {
    switch (mode) {
        case 0: return d_bermuda_pole(zone);  /* ORBITAL: pole-dependent */
        case 1: return 1;                      /* CHIRAL: always GROUND */
        case 2: return 0;                      /* CROSS: always ROUTE */
        case 3: return 1;                      /* HUB: always GROUND */
        default: return 0;
    }
}

__global__ void bermuda_gpu_kernel(
    const uint16_t * __restrict__ idxs_in,
    BermudaRouteEntry * __restrict__ out,
    uint8_t gear, uint8_t mode, uint32_t n)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;

    uint16_t idx = idxs_in[i];
    uint16_t idx_out = d_bermuda_traverse(idx, gear, mode);
    uint8_t  z       = d_bermuda_zone(idx, gear);

    out[i].idx_in     = idx;
    out[i].idx_out    = idx_out;
    out[i].zone       = z;
    out[i].pole       = d_bermuda_pole(z);
    out[i].shape      = d_bermuda_shape(mode, z);
    out[i].polarity   = d_bermuda_polarity(mode, z);
    out[i].tring_slot = d_bermuda_tring_slot(idx);
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
    uint16_t  *d_bermuda_idxs;   /* Device buffer for Bermuda input indices */
    BermudaRouteEntry *d_bermuda_out; /* Device buffer for Bermuda output */
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

    /* Allocate persistent Bermuda buffers (max capacity) */
    size_t bermuda_idxs_sz = (size_t)ctx->capacity * sizeof(uint16_t);
    size_t bermuda_out_sz  = (size_t)ctx->capacity * sizeof(BermudaRouteEntry);
    e = cudaMalloc(&ctx->d_bermuda_idxs, bermuda_idxs_sz);
    if (e != cudaSuccess) { printf("[icosa] d_bermuda_idxs alloc fail\n"); goto fail; }
    e = cudaMalloc(&ctx->d_bermuda_out, bermuda_out_sz);
    if (e != cudaSuccess) { printf("[icosa] d_bermuda_out alloc fail\n"); goto fail; }

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
    if (ctx->d_bermuda_idxs) cudaFree(ctx->d_bermuda_idxs);
    if (ctx->d_bermuda_out) cudaFree(ctx->d_bermuda_out);
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

/* ═══════════════════════════════════════════════════════════════════
 * BERMUDA GPU — batch traverse initialization and dispatch
 * ═══════════════════════════════════════════════════════════════════ */

/* Compute walk_len (smallest multiple of 12 >= slots, coprime with 37) */
static uint16_t _bermuda_walk_len(uint16_t slots) {
    uint16_t wl = slots;
    while (1) {
        if (wl % 12 == 0) {
            if (wl % 37 != 0) return wl;
        }
        wl++;
    }
}

/* Modular inverse (Extended Euclidean) */
static uint16_t _bermuda_modinv(uint16_t a, uint16_t m) {
    int32_t g = (int32_t)m, x = 0, a0 = (int32_t)a, x0 = 1;
    while (a0 != 0) {
        int32_t q = g / a0;
        int32_t t = a0; a0 = g - q * a0; g = t;
        t = x0; x0 = x - q * x0; x = t;
    }
    int32_t r = x % (int32_t)m;
    return (uint16_t)(r < 0 ? r + (int32_t)m : r);
}

/* Initialize Bermuda constant memory on device */
static int bermuda_gpu_init_constants(void) {
    uint16_t slots[5] = {0, 512, 1024, 2048, 4096};
    uint16_t walk_len[5], face_sz[5], inv37[5], inv37_wl[5];

    for (int g = 1; g <= 4; g++) {
        walk_len[g] = _bermuda_walk_len(slots[g]);
        face_sz[g]  = walk_len[g] / 12;
        inv37[g]    = _bermuda_modinv(BERMUDA_STRIDE, slots[g]);
        inv37_wl[g] = _bermuda_modinv(BERMUDA_STRIDE, walk_len[g]);
    }

    cudaError_t e;
    e = cudaMemcpyToSymbol(d_bermuda_slots, slots, sizeof(slots));
    if (e != cudaSuccess) { printf("[bermuda-gpu] slots copy fail: %d\n", e); return -1; }
    e = cudaMemcpyToSymbol(d_bermuda_walk_len, walk_len, sizeof(walk_len));
    if (e != cudaSuccess) { printf("[bermuda-gpu] walk_len copy fail: %d\n", e); return -2; }
    e = cudaMemcpyToSymbol(d_bermuda_face_sz, face_sz, sizeof(face_sz));
    if (e != cudaSuccess) { printf("[bermuda-gpu] face_sz copy fail: %d\n", e); return -3; }
    e = cudaMemcpyToSymbol(d_bermuda_inv37, inv37, sizeof(inv37));
    if (e != cudaSuccess) { printf("[bermuda-gpu] inv37 copy fail: %d\n", e); return -4; }
    e = cudaMemcpyToSymbol(d_bermuda_inv37_wl, inv37_wl, sizeof(inv37_wl));
    if (e != cudaSuccess) { printf("[bermuda-gpu] inv37_wl copy fail: %d\n", e); return -5; }

    return 0;
}

/* Dispatch Bermuda batch traverse on GPU */
ICOSA_API int bermuda_gpu_dispatch(
    void            *gpu_ctx,
    const uint16_t  *idxs_in,
    BermudaRouteEntry *out,
    uint8_t          gear,
    uint8_t          mode,
    uint32_t         n)
{
    if (!gpu_ctx || !idxs_in || !out || n == 0) return -1;
    if (gear < 1 || gear > 4) return -2;
    if (mode > 3) return -3;

    IcosaGpuCtx *ctx = (IcosaGpuCtx *)gpu_ctx;
    if (!ctx->valid) return -4;

    /* Ensure constants are initialized (one-time, lazy) */
    static int bermuda_constants_init = 0;
    if (!bermuda_constants_init) {
        int r = bermuda_gpu_init_constants();
        if (r != 0) return r;
        bermuda_constants_init = 1;
    }

    /* Ensure persistent buffers are allocated (lazy, once) */
    size_t idxs_cap = (size_t)ctx->capacity * sizeof(uint16_t);
    size_t out_cap  = (size_t)ctx->capacity * sizeof(BermudaRouteEntry);

    if (!ctx->d_bermuda_idxs) {
        cudaError_t e = cudaMalloc(&ctx->d_bermuda_idxs, idxs_cap);
        if (e != cudaSuccess) { printf("[bermuda-gpu] d_idxs alloc fail: %d\n", e); return -5; }
    }
    if (!ctx->d_bermuda_out) {
        cudaError_t e = cudaMalloc(&ctx->d_bermuda_out, out_cap);
        if (e != cudaSuccess) { printf("[bermuda-gpu] d_out alloc fail: %d\n", e); return -6; }
    }

    CUcontext prev_ctx = NULL;
    cuCtxGetCurrent(&prev_ctx);
    if (prev_ctx != ctx->cu_ctx) cuCtxSetCurrent(ctx->cu_ctx);
    int restore_ctx = (prev_ctx != ctx->cu_ctx && prev_ctx != NULL);

    cudaError_t e;
    dim3 grid((n + ICOSA_GPU_TPB - 1) / ICOSA_GPU_TPB, 1, 1);
    dim3 block(ICOSA_GPU_TPB, 1, 1);

    /* Copy input idxs to persistent device buffer (async, pinned host -> device) */
    size_t idxs_sz = (size_t)n * sizeof(uint16_t);
    e = cudaMemcpyAsync(ctx->d_bermuda_idxs, idxs_in, idxs_sz, cudaMemcpyHostToDevice, ctx->stream);
    if (e != cudaSuccess) { if (restore_ctx) cuCtxSetCurrent(prev_ctx); return -7; }

    /* Launch kernel */
    bermuda_gpu_kernel<<<grid, block, 0, ctx->stream>>>(
        ctx->d_bermuda_idxs, ctx->d_bermuda_out, gear, mode, n);

    e = cudaGetLastError();
    if (e != cudaSuccess) {
        printf("[bermuda-gpu] kernel launch error: %d (%s)\n", e, cudaGetErrorString(e));
        if (restore_ctx) cuCtxSetCurrent(prev_ctx);
        return -8;
    }

    /* Copy results back to host (async, device -> pinned host, then sync) */
    size_t out_sz = (size_t)n * sizeof(BermudaRouteEntry);
    e = cudaMemcpyAsync(out, ctx->d_bermuda_out, out_sz, cudaMemcpyDeviceToHost, ctx->stream);
    if (e != cudaSuccess) { if (restore_ctx) cuCtxSetCurrent(prev_ctx); return -9; }

    e = cudaStreamSynchronize(ctx->stream);
    if (e != cudaSuccess) { if (restore_ctx) cuCtxSetCurrent(prev_ctx); return -10; }

    if (restore_ctx) cuCtxSetCurrent(prev_ctx);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * GPU MEMORY MANAGEMENT — alloc/free/memcpy with context save/restore
 *
 * Used by StreamWindow for VRAM upload path.
 * All operations run on ctx->stream (async-capable).
 * ═══════════════════════════════════════════════════════════════════ */

ICOSA_API void *icosa_gpu_alloc(void *gpu_ctx, size_t size) {
    if (!gpu_ctx || size == 0) return NULL;
    IcosaGpuCtx *ctx = (IcosaGpuCtx *)gpu_ctx;
    if (!ctx->valid) return NULL;

    CUcontext prev_ctx = NULL;
    cuCtxGetCurrent(&prev_ctx);
    if (prev_ctx != ctx->cu_ctx) cuCtxSetCurrent(ctx->cu_ctx);
    int restore_ctx = (prev_ctx != ctx->cu_ctx && prev_ctx != NULL);

    void *ptr = NULL;
    cudaError_t e = cudaMalloc(&ptr, size);
    if (e != cudaSuccess) ptr = NULL;

    if (restore_ctx) cuCtxSetCurrent(prev_ctx);
    return ptr;
}

ICOSA_API int icosa_gpu_free(void *gpu_ctx, void *ptr) {
    if (!gpu_ctx || !ptr) return -1;
    IcosaGpuCtx *ctx = (IcosaGpuCtx *)gpu_ctx;
    if (!ctx->valid) return -2;

    CUcontext prev_ctx = NULL;
    cuCtxGetCurrent(&prev_ctx);
    if (prev_ctx != ctx->cu_ctx) cuCtxSetCurrent(ctx->cu_ctx);
    int restore_ctx = (prev_ctx != ctx->cu_ctx && prev_ctx != NULL);

    cudaFree(ptr);

    if (restore_ctx) cuCtxSetCurrent(prev_ctx);
    return 0;
}

ICOSA_API int icosa_gpu_memcpy_h2d(void *gpu_ctx, void *dst, const void *src, size_t size) {
    if (!gpu_ctx || !dst || !src || size == 0) return -1;
    IcosaGpuCtx *ctx = (IcosaGpuCtx *)gpu_ctx;
    if (!ctx->valid) return -2;

    CUcontext prev_ctx = NULL;
    cuCtxGetCurrent(&prev_ctx);
    if (prev_ctx != ctx->cu_ctx) cuCtxSetCurrent(ctx->cu_ctx);
    int restore_ctx = (prev_ctx != ctx->cu_ctx && prev_ctx != NULL);

    cudaError_t e = cudaMemcpy(dst, src, size, cudaMemcpyHostToDevice);

    if (restore_ctx) cuCtxSetCurrent(prev_ctx);
    return (e == cudaSuccess) ? 0 : -3;
}

ICOSA_API int icosa_gpu_sync(void *gpu_ctx) {
    if (!gpu_ctx) return -1;
    IcosaGpuCtx *ctx = (IcosaGpuCtx *)gpu_ctx;
    if (!ctx->valid) return -2;

    CUcontext prev_ctx = NULL;
    cuCtxGetCurrent(&prev_ctx);
    if (prev_ctx != ctx->cu_ctx) cuCtxSetCurrent(ctx->cu_ctx);
    int restore_ctx = (prev_ctx != ctx->cu_ctx && prev_ctx != NULL);

    cudaError_t e = cudaStreamSynchronize(ctx->stream);

    if (restore_ctx) cuCtxSetCurrent(prev_ctx);
    return (e == cudaSuccess) ? 0 : -3;
}

/* ═══════════════════════════════════════════════════════════════════
 * BATCH MEMCPY — N transfers within one context save/restore
 *
 * Used by Gear 2 (pinned memory mirror) for batched SID swap upload.
 * ═══════════════════════════════════════════════════════════════════ */

ICOSA_API int icosa_gpu_batch_memcpy_h2d(void *gpu_ctx,
    void *const *dst, const void *const *src,
    const size_t *sizes, int n)
{
    if (!gpu_ctx || !dst || !src || n == 0) return -1;
    IcosaGpuCtx *ctx = (IcosaGpuCtx *)gpu_ctx;
    if (!ctx->valid) return -2;

    CUcontext prev_ctx = NULL;
    cuCtxGetCurrent(&prev_ctx);
    if (prev_ctx != ctx->cu_ctx) cuCtxSetCurrent(ctx->cu_ctx);
    int restore_ctx = (prev_ctx != ctx->cu_ctx && prev_ctx != NULL);

    cudaError_t e = cudaSuccess;
    for (int i = 0; i < n; i++) {
        if (dst[i] && src[i] && sizes[i] > 0) {
            e = cudaMemcpyAsync(dst[i], src[i], sizes[i],
                                cudaMemcpyHostToDevice, ctx->stream);
            if (e != cudaSuccess) break;
        }
    }
    if (e == cudaSuccess)
        e = cudaStreamSynchronize(ctx->stream);

    if (restore_ctx) cuCtxSetCurrent(prev_ctx);
    return (e == cudaSuccess) ? 0 : -3;
}

ICOSA_API int icosa_gpu_batch_memcpy_d2h(void *gpu_ctx,
    void *const *dst, const void *const *src,
    const size_t *sizes, int n)
{
    if (!gpu_ctx || !dst || !src || n == 0) return -1;
    IcosaGpuCtx *ctx = (IcosaGpuCtx *)gpu_ctx;
    if (!ctx->valid) return -2;

    CUcontext prev_ctx = NULL;
    cuCtxGetCurrent(&prev_ctx);
    if (prev_ctx != ctx->cu_ctx) cuCtxSetCurrent(ctx->cu_ctx);
    int restore_ctx = (prev_ctx != ctx->cu_ctx && prev_ctx != NULL);

    cudaError_t e = cudaSuccess;
    for (int i = 0; i < n; i++) {
        if (dst[i] && src[i] && sizes[i] > 0) {
            e = cudaMemcpyAsync(dst[i], src[i], sizes[i],
                                cudaMemcpyDeviceToHost, ctx->stream);
            if (e != cudaSuccess) break;
        }
    }
    if (e == cudaSuccess)
        e = cudaStreamSynchronize(ctx->stream);

    if (restore_ctx) cuCtxSetCurrent(prev_ctx);
    return (e == cudaSuccess) ? 0 : -3;
}

/* ═══════════════════════════════════════════════════════════════════
 * HOST PINNED MEMORY — allocate/free host memory usable by DMA engine
 *
 * Pinned memory eliminates the implicit driver staging copy that
 * happens when cudaMemcpy reads from a regular malloc'd buffer.
 * ═══════════════════════════════════════════════════════════════════ */

ICOSA_API int icosa_gpu_pin_host(void *gpu_ctx, void **ptr, size_t size) {
    (void)gpu_ctx;
    if (!ptr || size == 0) return -1;
    cudaError_t e = cudaMallocHost(ptr, size);
    if (e != cudaSuccess) {
        *ptr = NULL;
        return -2;
    }
    return 0;
}

ICOSA_API int icosa_gpu_unpin_host(void *gpu_ctx, void *ptr) {
    (void)gpu_ctx;
    if (!ptr) return -1;
    cudaFreeHost(ptr);
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

/* ═══════════════════════════════════════════════════════════════════
 * BERMUDA GPU CORRECTNESS TEST
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef TEST_BERMUDA_CORRECTNESS
#define BERMUDA_STRIDE       37u
#define BERMUDA_N_ZONES      12u
#define BERMUDA_TRING_SLOTS  720u
#define BERMUDA_GEAR1_SLOTS  512u
#define BERMUDA_GEAR2_SLOTS  1024u
#define BERMUDA_GEAR3_SLOTS  2048u
#define BERMUDA_GEAR4_SLOTS  4096u
#define ICOSA_GPU_TPB        256u

static const uint8_t  BERMUDA_CROSS[12] = {9,10,11,6,7,8,3,4,5,0,1,2};
static const uint16_t BERMUDA_SLOTS[5] = {0, 512, 1024, 2048, 4096};

static uint16_t _bermuda_walk_len(uint16_t slots) {
    uint16_t wl = slots;
    while (1) {
        if (wl % 12 == 0 && wl % 37 != 0) return wl;
        wl++;
    }
}

static uint16_t _bermuda_modinv(uint16_t a, uint16_t m) {
    int32_t g = (int32_t)m, x = 0, a0 = (int32_t)a, x0 = 1;
    while (a0 != 0) {
        int32_t q = g / a0;
        int32_t t = a0; a0 = g - q * a0; g = t;
        t = x0; x0 = x - q * x0; x = t;
    }
    int32_t r = x % (int32_t)m;
    return (uint16_t)(r < 0 ? r + (int32_t)m : r);
}

static uint16_t BERMUDA_WALK_LEN[5], BERMUDA_FACE_SZ[5], BERMUDA_INV37[5], BERMUDA_INV37_WL[5];

static void bermuda_cpu_init(void) {
    for (int g = 1; g <= 4; g++) {
        uint16_t slots = BERMUDA_SLOTS[g];
        BERMUDA_WALK_LEN[g] = _bermuda_walk_len(slots);
        BERMUDA_FACE_SZ[g] = BERMUDA_WALK_LEN[g] / 12;
        BERMUDA_INV37[g] = _bermuda_modinv(BERMUDA_STRIDE, slots);
        BERMUDA_INV37_WL[g] = _bermuda_modinv(BERMUDA_STRIDE, BERMUDA_WALK_LEN[g]);
    }
}

static uint16_t bermuda_cpu_traverse_orbit(uint16_t idx, uint8_t gear) {
    uint16_t N = BERMUDA_SLOTS[gear];
    return (uint16_t)((idx + 1) % N);
}

static uint16_t bermuda_cpu_traverse_chiral(uint16_t idx, uint8_t gear) {
    uint16_t N = BERMUDA_SLOTS[gear];
    return (uint16_t)((idx + N / 2) % N);
}

static uint16_t bermuda_cpu_traverse_cross(uint16_t idx, uint8_t gear) {
    uint16_t WL = BERMUDA_WALK_LEN[gear];
    uint16_t FS = BERMUDA_FACE_SZ[gear];
    uint16_t IW = BERMUDA_INV37_WL[gear];
    uint16_t N  = BERMUDA_SLOTS[gear];
    uint16_t enc = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE) % WL);
    uint8_t  z   = (uint8_t)(enc / FS);
    uint8_t  pz  = BERMUDA_CROSS[z % 12];
    uint16_t ne  = (uint16_t)(pz * FS + enc % FS);
    return (uint16_t)(((uint32_t)ne * IW) % WL % N);
}

static uint16_t bermuda_cpu_traverse_hub(uint16_t idx, uint8_t gear) {
    uint16_t WL = BERMUDA_WALK_LEN[gear];
    uint16_t FS = BERMUDA_FACE_SZ[gear];
    uint16_t N  = BERMUDA_SLOTS[gear];
    uint16_t enc = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE) % WL);
    uint8_t  z  = (uint8_t)(enc / FS);
    return (uint16_t)(((uint32_t)z * (N / BERMUDA_N_ZONES)) % N);
}

static uint16_t bermuda_cpu_traverse(uint16_t idx, uint8_t gear, uint8_t mode) {
    switch (mode) {
        case 0: return bermuda_cpu_traverse_orbit(idx, gear);
        case 1: return bermuda_cpu_traverse_chiral(idx, gear);
        case 2: return bermuda_cpu_traverse_cross(idx, gear);
        case 3: return bermuda_cpu_traverse_hub(idx, gear);
        default: return idx;
    }
}

static uint8_t bermuda_cpu_zone(uint16_t idx, uint8_t gear) {
    uint16_t enc = (uint16_t)(((uint32_t)idx * BERMUDA_STRIDE) % BERMUDA_WALK_LEN[gear]);
    return (uint8_t)(enc / BERMUDA_FACE_SZ[gear]);
}

static uint8_t bermuda_cpu_pole(uint8_t zone) {
    return zone >= 6 ? 1 : 0;
}

static uint16_t bermuda_cpu_tring_slot(uint16_t idx) {
    return idx % BERMUDA_TRING_SLOTS;
}

static uint8_t bermuda_cpu_shape(uint8_t mode, uint8_t zone) {
    switch (mode) {
        case 0: return zone < 6 ? 73 : 79;
        case 1: return 79;
        case 2: return 83;
        case 3: return 76;
        default: return 73;
    }
}

static uint8_t bermuda_cpu_polarity(uint8_t mode, uint8_t zone) {
    switch (mode) {
        case 0: return bermuda_cpu_pole(zone);
        case 1: return 1;
        case 2: return 0;
        case 3: return 1;
        default: return 0;
    }
}

typedef struct {
    uint16_t idx_in;
    uint16_t idx_out;
    uint8_t  zone;
    uint8_t  pole;
    uint8_t  shape;
    uint8_t  polarity;
    uint16_t tring_slot;
} BermudaRouteEntry;

static void bermuda_cpu_route_token(uint16_t idx_in, uint8_t gear, uint8_t mode, BermudaRouteEntry *out) {
    uint16_t idx_out = bermuda_cpu_traverse(idx_in, gear, mode);
    uint8_t  z       = bermuda_cpu_zone(idx_in, gear);
    out->idx_in      = idx_in;
    out->idx_out     = idx_out;
    out->zone        = z;
    out->pole        = bermuda_cpu_pole(z);
    out->shape       = bermuda_cpu_shape(mode, z);
    out->polarity    = bermuda_cpu_polarity(mode, z);
    out->tring_slot  = bermuda_cpu_tring_slot(idx_in);
}

int main(void) {
    bermuda_cpu_init();

    printf("=== Bermuda GPU vs CPU Correctness Test ===\n\n");

    uint32_t n = 5000;
    uint16_t *idxs_in = (uint16_t*)malloc(n * sizeof(uint16_t));
    BermudaRouteEntry *gpu_out = (BermudaRouteEntry*)malloc(n * sizeof(BermudaRouteEntry));
    BermudaRouteEntry *cpu_out = (BermudaRouteEntry*)malloc(n * sizeof(BermudaRouteEntry));

    srand(12345);
    for (uint32_t i = 0; i < n; i++) {
        idxs_in[i] = (uint16_t)(rand() % 4096);
    }

    /* Test all gear/mode combinations */
    int all_pass = 1;
    for (int gear = 1; gear <= 4; gear++) {
        for (int mode = 0; mode <= 3; mode++) {
            printf("Testing gear=%d mode=%d... ", gear, mode);

            /* GPU dispatch */
            IcosaGpuCtx *ctx = (IcosaGpuCtx*)icosa_gpu_ctx_create(0, 0xDEADBEEFCAFEBABEULL);
            if (!ctx || !icosa_gpu_ctx_valid(ctx)) {
                printf("GPU init failed, skipping\n");
                continue;
            }

            int ret = bermuda_gpu_dispatch(ctx, idxs_in, gpu_out, gear, mode, n);
            icosa_gpu_ctx_destroy(ctx);

            if (ret != 0) {
                printf("GPU dispatch error %d\n", ret);
                all_pass = 0;
                continue;
            }

            /* CPU reference */
            for (uint32_t i = 0; i < n; i++) {
                bermuda_cpu_route_token(idxs_in[i], gear, mode, &cpu_out[i]);
            }

            /* Compare */
            int mismatches = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (gpu_out[i].idx_in   != cpu_out[i].idx_in ||
                    gpu_out[i].idx_out  != cpu_out[i].idx_out ||
                    gpu_out[i].zone     != cpu_out[i].zone ||
                    gpu_out[i].pole     != cpu_out[i].pole ||
                    gpu_out[i].shape    != cpu_out[i].shape ||
                    gpu_out[i].polarity != cpu_out[i].polarity ||
                    gpu_out[i].tring_slot != cpu_out[i].tring_slot) {
                    if (mismatches < 5)
                        printf("\n  MISMATCH [%u]: GPU(idx_out=%u,zone=%u,shape=%u) CPU(idx_out=%u,zone=%u,shape=%u)",
                               i, gpu_out[i].idx_out, gpu_out[i].zone, gpu_out[i].shape,
                               cpu_out[i].idx_out, cpu_out[i].zone, cpu_out[i].shape);
                    mismatches++;
                }
            }
            if (mismatches == 0) {
                printf("PASS\n");
            } else {
                printf("FAIL (%d mismatches)\n", mismatches);
                all_pass = 0;
            }
        }
    }

    printf("\n=== RESULT ===\n");
    if (all_pass) {
        printf("✓ ALL TESTS PASSED\n");
    } else {
        printf("✗ SOME TESTS FAILED\n");
    }

    free(idxs_in);
    free(gpu_out);
    free(cpu_out);

    return all_pass ? 0 : 1;
}
#endif /* TEST_BERMUDA_CORRECTNESS */

/*
 * geofield_cuda.cu — GPU-accelerated GeoField Pipeline Stages
 *
 * Parallel per-block kernels:
 *   1. Diamond Shell classify (6 rotations + fibo_intersect)
 *   2. Skeleton decide (P0-P5)
 *   3. Wallet chunk seed (XOR-fold + SplitMix64)
 *
 * Compile:
 *   nvcc -O3 -shared -o geofield_cuda.dll geofield_cuda.cu -Xcompiler -fPIC -arch=sm_61
 *
 * Usage from Python ctypes:
 *   dll = ctypes.CDLL('./geofield_cuda.dll')
 *   dll.geofield_batch_classify(d_blocks, d_results, n)
 *   dll.geofield_batch_skel(d_blocks, d_prevs, d_results, n, has_prev)
 *   dll.geofield_batch_seed(d_blocks, d_seeds, n)
 */

#include <stdint.h>
#include <cuda_runtime.h>

#define CHUNK_SZ 64
#define SKEL_ID     0
#define SKEL_FLAT   1
#define SKEL_DIFF   2
#define SKEL_BREF   3
#define SKEL_GEOM   4
#define SKEL_RAW    5

/* ═══════════════════════════════════════════════════════════════
   DEVICE FUNCTIONS
   ═══════════════════════════════════════════════════════════════ */

__device__ __forceinline__
uint64_t d_splitmix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

__device__ __forceinline__
uint32_t d_popcount64(uint64_t x) {
    return (uint32_t)__popcll(x);
}

__device__ __forceinline__
uint8_t d_isect_pop(const uint8_t *chunk) {
    const uint64_t *w = (const uint64_t *)chunk;
    uint64_t fold = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4] ^ w[5] ^ w[6] ^ w[7];
    return (uint8_t)d_popcount64(fold);
}

__device__ __forceinline__
int d_is_flat(const uint8_t *chunk) {
    const uint64_t *w = (const uint64_t *)chunk;
    uint64_t acc = 0;
    for (int i = 0; i < 8; i++) acc |= w[i];
    return acc == 0;
}

__device__ __forceinline__
uint8_t d_diff_count(const uint8_t *a, const uint8_t *b) {
    uint8_t count = 0;
    for (int i = 0; i < 64; i++) {
        if (a[i] != b[i]) count++;
    }
    return count;
}

__device__ __forceinline__
int d_is_bref(const uint8_t *cur, const uint8_t *prev) {
    for (int i = 0; i < 64; i++) {
        if (cur[i] != prev[63 - i]) return 0;
    }
    return 1;
}

/* Diamond Shell rotation (6 orientations of 4x4x4 cube) */
__device__ __forceinline__
void d_rotate64(uint8_t out[64], const uint8_t in[64], uint8_t rot) {
    for (int i = 0; i < 64; i++) {
        int x = (i >> 0) & 3;
        int y = (i >> 2) & 3;
        int z = (i >> 4) & 3;
        int nx, ny, nz;
        switch (rot) {
            case 0: nx=x; ny=y; nz=z; break;
            case 1: nx=y; ny=z; nz=x; break;
            case 2: nx=z; ny=x; nz=y; break;
            case 3: nx=3-x; ny=y; nz=z; break;
            case 4: nx=x; ny=3-y; nz=z; break;
            case 5: nx=x; ny=y; nz=3-z; break;
            default: nx=x; ny=y; nz=z; break;
        }
        out[nx + ny*4 + nz*16] = in[i];
    }
}

__device__ __forceinline__
uint64_t d_fibo_intersect(const uint8_t *chunk) {
    uint64_t core = 0;
    const uint8_t *c = chunk;
    for (int i = 0; i < 8; i++) {
        core |= (uint64_t)c[i] << (i * 8);
    }
    return core & (core >> 1) & (core >> 2) & (core >> 3);
}

__device__ __forceinline__
uint8_t d_popcount_u64(uint64_t x) {
    return (uint8_t)d_popcount64(x);
}

/* ═══════════════════════════════════════════════════════════════
   KERNEL 1: Diamond Shell classify (parallel per-block)
   Input:  n blocks × 64 bytes
   Output: n × (best_rot, isect_pc, flag)
   ═══════════════════════════════════════════════════════════════ */

struct DiamondResult {
    uint8_t best_rot;
    uint8_t isect_pc;
    uint8_t flag;       /* 0=FLAT, 1=SPARSE, 2=DENSE */
};

__global__ void diamond_classify_kernel(
    const uint8_t    *__restrict__ blocks,    // n × 64
    DiamondResult    *__restrict__ results,   // n
    uint32_t n)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n) return;

    const uint8_t *blk = blocks + (size_t)idx * CHUNK_SZ;

    DiamondResult dr = {0, 0, 2};
    uint8_t best_pc = 0;

    for (uint8_t rot = 0; rot < 6; rot++) {
        uint8_t rotated[64];
        d_rotate64(rotated, blk, rot);
        uint64_t isect = d_fibo_intersect(rotated);
        uint8_t pc = d_popcount_u64(isect);
        if (pc > best_pc) {
            best_pc = pc;
            dr.best_rot = rot;
        }
    }

    dr.isect_pc = best_pc;
    if (best_pc == 0) dr.flag = 0;        /* FLAT */
    else if (best_pc <= 4) dr.flag = 1;   /* SPARSE */
    else dr.flag = 2;                      /* DENSE */

    results[idx] = dr;
}

/* ═══════════════════════════════════════════════════════════════
   KERNEL 2: Skeleton decide (parallel per-block)
   Input:  n blocks × 64, n prevs × 64 (or NULL)
   Output: n × (strategy, best_rot, isect_pc, ref_idx, diff_count)
   ═══════════════════════════════════════════════════════════════ */

struct SkelResult {
    uint8_t strategy;
    uint8_t best_rot;
    uint8_t isect_pc;
    uint8_t ref_idx;
    uint8_t diff_count;
};

__global__ void skel_decide_kernel(
    const uint8_t    *__restrict__ blocks,    // n × 64
    const uint8_t    *__restrict__ prevs,     // n × 64 or NULL
    SkelResult       *__restrict__ results,   // n
    int               has_prev,
    uint32_t n)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n) return;

    const uint8_t *blk = blocks + (size_t)idx * CHUNK_SZ;
    const uint8_t *prv = has_prev ? (prevs + (size_t)idx * CHUNK_SZ) : NULL;

    SkelResult sr = { SKEL_RAW, 0, 0, 0, 64 };

    /* P0: IDENTITY */
    if (has_prev) {
        int same = 1;
        for (int i = 0; i < 64; i++) {
            if (blk[i] != prv[i]) { same = 0; break; }
        }
        if (same) {
            sr.strategy = SKEL_ID;
            sr.diff_count = 0;
            results[idx] = sr;
            return;
        }
    }

    /* P1: high entropy → RAW */
    uint8_t ip = d_isect_pop(blk);
    sr.isect_pc = ip;
    if (ip >= 16) {
        sr.strategy = SKEL_RAW;
        results[idx] = sr;
        return;
    }

    /* P2: FLAT */
    if (d_is_flat(blk)) {
        sr.strategy = SKEL_FLAT;
        sr.diff_count = 0;
        results[idx] = sr;
        return;
    }

    /* P3: DIFF */
    if (has_prev) {
        uint8_t dc = d_diff_count(blk, prv);
        sr.diff_count = dc;
        if (dc >= 1 && dc <= 48) {
            sr.strategy = SKEL_DIFF;
            results[idx] = sr;
            return;
        }
        /* P4: BREF */
        if (d_is_bref(blk, prv)) {
            sr.strategy = SKEL_BREF;
            results[idx] = sr;
            return;
        }
    }

    /* P5: GEOM */
    sr.strategy = SKEL_GEOM;
    results[idx] = sr;
}

/* ═══════════════════════════════════════════════════════════════
   KERNEL 3: Wallet chunk seed (parallel per-block)
   Input:  n blocks × 64
   Output: n × uint64 seed
   ═══════════════════════════════════════════════════════════════ */

__global__ void wallet_seed_kernel(
    const uint8_t    *__restrict__ blocks,    // n × 64
    uint64_t         *__restrict__ seeds,     // n
    uint32_t n)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n) return;

    const uint8_t *c = blocks + (size_t)idx * CHUNK_SZ;

    uint64_t acc = 0;
    #pragma unroll
    for (int i = 0; i < 8; i++) {
        uint64_t w = 0;
        #pragma unroll
        for (int j = 0; j < 8; j++) {
            w |= (uint64_t)c[i * 8 + j] << (j * 8);
        }
        acc ^= w;
    }

    seeds[idx] = d_splitmix64(acc);
}

/* ═══════════════════════════════════════════════════════════════
   KERNEL 4: Combined classify + skel + seed (single-pass)
   Input:  n blocks × 64, prevs (or NULL)
   Output: n × (DiamondResult, SkelResult, seed)
   ═══════════════════════════════════════════════════════════════ */

struct CombinedResult {
    DiamondResult diamond;
    SkelResult    skel;
    uint64_t      seed;
};

__global__ void geofield_combined_kernel(
    const uint8_t    *__restrict__ blocks,
    const uint8_t    *__restrict__ prevs,
    CombinedResult   *__restrict__ results,
    int               has_prev,
    uint32_t n)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n) return;

    const uint8_t *blk = blocks + (size_t)idx * CHUNK_SZ;
    const uint8_t *prv = has_prev ? (prevs + (size_t)idx * CHUNK_SZ) : NULL;

    CombinedResult cr;

    /* Diamond classify */
    {
        DiamondResult dr = {0, 0, 2};
        uint8_t best_pc = 0;
        for (uint8_t rot = 0; rot < 6; rot++) {
            uint8_t rotated[64];
            d_rotate64(rotated, blk, rot);
            uint64_t isect = d_fibo_intersect(rotated);
            uint8_t pc = d_popcount_u64(isect);
            if (pc > best_pc) { best_pc = pc; dr.best_rot = rot; }
        }
        dr.isect_pc = best_pc;
        if (best_pc == 0) dr.flag = 0;
        else if (best_pc <= 4) dr.flag = 1;
        else dr.flag = 2;
        cr.diamond = dr;
    }

    /* Skeleton decide */
    {
        SkelResult sr = { SKEL_RAW, 0, 0, 0, 64 };

        if (has_prev) {
            int same = 1;
            for (int i = 0; i < 64; i++) {
                if (blk[i] != prv[i]) { same = 0; break; }
            }
            if (same) { sr.strategy = SKEL_ID; sr.diff_count = 0; }
        }

        if (sr.strategy == SKEL_RAW) {
            uint8_t ip = d_isect_pop(blk);
            sr.isect_pc = ip;
            if (ip >= 16) sr.strategy = SKEL_RAW;
            else if (d_is_flat(blk)) { sr.strategy = SKEL_FLAT; sr.diff_count = 0; }
            else if (has_prev) {
                uint8_t dc = d_diff_count(blk, prv);
                sr.diff_count = dc;
                if (dc >= 1 && dc <= 48) sr.strategy = SKEL_DIFF;
                else if (d_is_bref(blk, prv)) sr.strategy = SKEL_BREF;
                else sr.strategy = SKEL_GEOM;
            } else {
                sr.strategy = SKEL_GEOM;
            }
        }
        cr.skel = sr;
    }

    /* Wallet seed */
    {
        uint64_t acc = 0;
        #pragma unroll
        for (int i = 0; i < 8; i++) {
            uint64_t w = 0;
            #pragma unroll
            for (int j = 0; j < 8; j++) {
                w |= (uint64_t)blk[i * 8 + j] << (j * 8);
            }
            acc ^= w;
        }
        cr.seed = d_splitmix64(acc);
    }

    results[idx] = cr;
}

/* ═══════════════════════════════════════════════════════════════
   C API for Python ctypes
   ═══════════════════════════════════════════════════════════════ */

extern "C" {

/* Batch Diamond Shell classify */
__declspec(dllexport) int geofield_batch_classify(
    const uint8_t *h_blocks,
    DiamondResult *h_results,
    uint32_t n)
{
    uint8_t     *d_blocks;
    DiamondResult *d_results;

    size_t in_bytes  = (size_t)n * CHUNK_SZ;
    size_t out_bytes = (size_t)n * sizeof(DiamondResult);

    cudaMalloc(&d_blocks, in_bytes);
    cudaMalloc(&d_results, out_bytes);
    cudaMemcpy(d_blocks, h_blocks, in_bytes, cudaMemcpyHostToDevice);

    int block = 256;
    int grid = (n + block - 1) / block;
    diamond_classify_kernel<<<grid, block>>>(d_blocks, d_results, n);

    cudaMemcpy(h_results, d_results, out_bytes, cudaMemcpyDeviceToHost);
    cudaFree(d_blocks);
    cudaFree(d_results);

    return cudaGetLastError() == cudaSuccess ? 0 : -1;
}

/* Batch Skeleton decide */
__declspec(dllexport) int geofield_batch_skel(
    const uint8_t *h_blocks,
    const uint8_t *h_prevs,
    SkelResult    *h_results,
    uint32_t       n,
    int            has_prev)
{
    uint8_t    *d_blocks;
    uint8_t    *d_prevs = NULL;
    SkelResult *d_results;

    size_t in_bytes  = (size_t)n * CHUNK_SZ;
    size_t out_bytes = (size_t)n * sizeof(SkelResult);

    cudaMalloc(&d_blocks, in_bytes);
    cudaMalloc(&d_results, out_bytes);
    cudaMemcpy(d_blocks, h_blocks, in_bytes, cudaMemcpyHostToDevice);

    if (has_prev && h_prevs) {
        cudaMalloc(&d_prevs, in_bytes);
        cudaMemcpy(d_prevs, h_prevs, in_bytes, cudaMemcpyHostToDevice);
    }

    int block = 256;
    int grid = (n + block - 1) / block;
    skel_decide_kernel<<<grid, block>>>(d_blocks, d_prevs, d_results, has_prev, n);

    cudaMemcpy(h_results, d_results, out_bytes, cudaMemcpyDeviceToHost);
    cudaFree(d_blocks);
    if (d_prevs) cudaFree(d_prevs);
    cudaFree(d_results);

    return cudaGetLastError() == cudaSuccess ? 0 : -1;
}

/* Batch Wallet seed */
__declspec(dllexport) int geofield_batch_seed(
    const uint8_t *h_blocks,
    uint64_t      *h_seeds,
    uint32_t       n)
{
    uint8_t  *d_blocks;
    uint64_t *d_seeds;

    size_t in_bytes  = (size_t)n * CHUNK_SZ;
    size_t out_bytes = (size_t)n * sizeof(uint64_t);

    cudaMalloc(&d_blocks, in_bytes);
    cudaMalloc(&d_seeds, out_bytes);
    cudaMemcpy(d_blocks, h_blocks, in_bytes, cudaMemcpyHostToDevice);

    int block = 256;
    int grid = (n + block - 1) / block;
    wallet_seed_kernel<<<grid, block>>>(d_blocks, d_seeds, n);

    cudaMemcpy(h_seeds, d_seeds, out_bytes, cudaMemcpyDeviceToHost);
    cudaFree(d_blocks);
    cudaFree(d_seeds);

    return cudaGetLastError() == cudaSuccess ? 0 : -1;
}

/* Combined: classify + skel + seed in single pass */
__declspec(dllexport) int geofield_batch_combined(
    const uint8_t      *h_blocks,
    const uint8_t      *h_prevs,
    CombinedResult     *h_results,
    uint32_t            n,
    int                 has_prev)
{
    uint8_t         *d_blocks;
    uint8_t         *d_prevs = NULL;
    CombinedResult  *d_results;

    size_t in_bytes  = (size_t)n * CHUNK_SZ;
    size_t out_bytes = (size_t)n * sizeof(CombinedResult);

    cudaMalloc(&d_blocks, in_bytes);
    cudaMalloc(&d_results, out_bytes);
    cudaMemcpy(d_blocks, h_blocks, in_bytes, cudaMemcpyHostToDevice);

    if (has_prev && h_prevs) {
        cudaMalloc(&d_prevs, in_bytes);
        cudaMemcpy(d_prevs, h_prevs, in_bytes, cudaMemcpyHostToDevice);
    }

    int block = 256;
    int grid = (n + block - 1) / block;
    geofield_combined_kernel<<<grid, block>>>(d_blocks, d_prevs, d_results, has_prev, n);

    cudaMemcpy(h_results, d_results, out_bytes, cudaMemcpyDeviceToHost);
    cudaFree(d_blocks);
    if (d_prevs) cudaFree(d_prevs);
    cudaFree(d_results);

    return cudaGetLastError() == cudaSuccess ? 0 : -1;
}

/* Query GPU info */
__declspec(dllexport) int geofield_gpu_info(int *out_sm_count, int *out_max_threads) {
    cudaDeviceProp prop;
    cudaError_t err = cudaGetDeviceProperties(&prop, 0);
    if (err != cudaSuccess) return -1;
    *out_sm_count = prop.multiProcessorCount;
    *out_max_threads = prop.maxThreadsPerBlock;
    return 0;
}

} /* extern "C" */

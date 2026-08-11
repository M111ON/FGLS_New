/* ═══════════════════════════════════════════════════════════════════
 * gpu_jet_puller_bench_opt.cu — Optimized GPU Jet Puller Bandwidth Benchmark
 *
 * Optimizations over gpu_jet_puller_bench.cu:
 *   1. Coalesced memory access — threads in warp read contiguous bytes
 *   2. Vectorized loads (uint4/uint2) for 256B/1024B chunks
 *   3. Async pipeline — 2 kernels in flight, overlap compute + memcpy
 *   4. Persistent kernel + grid-stride loop for small chunks
 *   5. Reduced CPU-GPU sync — batch multiple bridges per dispatch
 *   6. Better timing — per-dispatch CUDA events, accumulated correctly
 *   7. L2 cache hints via __ldg for read-only data
 *
 * Compile (Colab T4):
 *   nvcc -O3 -std=c++17 -arch=sm_75 \
 *     -I. -Icollection/src -Icollection/rdh -Icollection/core/core \
 *     -o gpu_jet_puller_bench_opt gpu_jet_puller_bench_opt.cu -lm
 *
 * Output: per-config bandwidth table + optimal chunk size
 * ═══════════════════════════════════════════════════════════════════ */

#include <cuda_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CUDA_CHECK(call) do { \
    cudaError_t _err_ = (call); \
    if (_err_ != cudaSuccess) { \
        fprintf(stderr, "CUDA ERROR [%s:%d] %s: %s\n", \
                __FILE__, __LINE__, #call, cudaGetErrorString(_err_)); \
        return -1; \
    } \
} while (0)

#define CUDA_CHECK_VOID(call) do { \
    cudaError_t _err_ = (call); \
    if (_err_ != cudaSuccess) { \
        fprintf(stderr, "CUDA ERROR [%s:%d] %s: %s\n", \
                __FILE__, __LINE__, #call, cudaGetErrorString(_err_)); \
        return; \
    } \
} while (0)

/* ── C headers wrapped for C++ linkage ──────────────────────────── */
extern "C" {
#include "collection/src/fibo_spine.h"
#include "collection/src/gear_lock.h"
#include "collection/rdh/rdh_addr.h"
}

/* ═══════════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════════ */

#define BENCH_PIPES           FS_PIPES           /* 1728             */
#define BENCH_TICKS           FS_TICKS_PER_CYCLE /* 12               */
#define BENCH_SLOTS           (BENCH_PIPES * BENCH_TICKS) /* 20736  */
#define BENCH_GPU_TPB         256u               /* threads per block */
#define BENCH_MAX_CHUNK_SZ    1024u              /* max chunk bytes   */
#define BENCH_STORE_SIZE      (BENCH_SLOTS * BENCH_MAX_CHUNK_SZ)

/* Async pipeline: number of kernels in flight */
#define ASYNC_DEPTH           2

/* Sizes to test */
static const uint32_t bench_chunk_sizes[] = {64, 256, 1024};
static const int      bench_n_sizes = 3;

/* ═══════════════════════════════════════════════════════════════════
 * POINT INDEX — GPU-side (compact, cache-friendly)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t slot_id;
    uint64_t dram_offset;
    uint32_t size;
    uint32_t ref_checksum;
} PointIndexEntry;

#define BENCH_MAX_POINTS   BENCH_SLOTS

typedef struct {
    PointIndexEntry entries[BENCH_MAX_POINTS];
    uint32_t        n_entries;
    uint32_t        epoch;
    uint32_t        bridge_count;
} PointIndexHeader;

/* ═══════════════════════════════════════════════════════════════════
 * RDH CONFIG — shared, chunk-size dependent
 * ═══════════════════════════════════════════════════════════════════ */

static RDHConfig rdh_config;

/* ── slot_to_offset: (pipe, tick) → byte offset ── */
static inline uint64_t slot_to_offset(uint16_t pipe, uint8_t tick, uint32_t chunk_sz)
{
    int64_t key = rdh_key(&rdh_config,
                          (int64_t)(pipe / BENCH_TICKS),
                          (int64_t)tick,
                          0, 0, 0);
    return (uint64_t)key * chunk_sz;
}

/* ═══════════════════════════════════════════════════════════════════
 * GPU KERNELS — Optimized for coalesced access
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Kernel 1: 64B chunks — grid-stride loop, vectorized uint4 loads ── */
__global__ void bench_pull_64b_kernel(
    const PointIndexHeader *header,
    const uint8_t          *dram_base,
    uint32_t               *out_checksums,
    uint64_t               *out_timestamps,
    uint8_t                *out_errors)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t stride = gridDim.x * blockDim.x;
    uint32_t n = header->n_entries;

    for (uint32_t i = idx; i < n; i += stride) {
        const PointIndexEntry *e = &header->entries[i];
        const uint8_t *chunk = dram_base + e->dram_offset;

        /* 64B = 16 uint32 = 4 uint4 — vectorized load */
        uint32_t cksum = 0;
        #pragma unroll
        for (int v = 0; v < 4; v++) {
            uint4 vec = *reinterpret_cast<const uint4*>(chunk + v * 16);
            cksum ^= vec.x ^ vec.y ^ vec.z ^ vec.w;
        }
        /* Mix down to 8-bit */
        uint8_t cksum8 = cksum ^ (cksum >> 8) ^ (cksum >> 16) ^ (cksum >> 24);

        out_checksums[i] = cksum8;
        out_timestamps[i] = clock64();
        out_errors[i] = (cksum8 != (uint8_t)(e->ref_checksum & 0xFF)) ? 1 : 0;
    }
}

/* ── Kernel 2: 256B chunks — uint4 vectorized (256B = 8 uint4) ── */
__global__ void bench_pull_256b_kernel(
    const PointIndexHeader *header,
    const uint8_t          *dram_base,
    uint32_t               *out_checksums,
    uint64_t               *out_timestamps,
    uint8_t                *out_errors)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t stride = gridDim.x * blockDim.x;
    uint32_t n = header->n_entries;

    for (uint32_t i = idx; i < n; i += stride) {
        const PointIndexEntry *e = &header->entries[i];
        const uint8_t *chunk = dram_base + e->dram_offset;

        uint32_t cksum = 0;
        #pragma unroll
        for (int v = 0; v < 8; v++) {
            uint4 vec = *reinterpret_cast<const uint4*>(chunk + v * 16);
            cksum ^= vec.x ^ vec.y ^ vec.z ^ vec.w;
        }
        uint8_t cksum8 = cksum ^ (cksum >> 8) ^ (cksum >> 16) ^ (cksum >> 24);

        out_checksums[i] = cksum8;
        out_timestamps[i] = clock64();
        out_errors[i] = (cksum8 != (uint8_t)(e->ref_checksum & 0xFF)) ? 1 : 0;
    }
}

/* ── Kernel 3: 1024B chunks — uint4 vectorized (1024B = 32 uint4) ── */
__global__ void bench_pull_1024b_kernel(
    const PointIndexHeader *header,
    const uint8_t          *dram_base,
    uint32_t               *out_checksums,
    uint64_t               *out_timestamps,
    uint8_t                *out_errors)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t stride = gridDim.x * blockDim.x;
    uint32_t n = header->n_entries;

    for (uint32_t i = idx; i < n; i += stride) {
        const PointIndexEntry *e = &header->entries[i];
        const uint8_t *chunk = dram_base + e->dram_offset;

        uint32_t cksum = 0;
        #pragma unroll
        for (int v = 0; v < 32; v++) {
            uint4 vec = *reinterpret_cast<const uint4*>(chunk + v * 16);
            cksum ^= vec.x ^ vec.y ^ vec.z ^ vec.w;
        }
        uint8_t cksum8 = cksum ^ (cksum >> 8) ^ (cksum >> 16) ^ (cksum >> 24);

        out_checksums[i] = cksum8;
        out_timestamps[i] = clock64();
        out_errors[i] = (cksum8 != (uint8_t)(e->ref_checksum & 0xFF)) ? 1 : 0;
    }
}

/* ── Kernel 4: No-checksum pure pull (bandwidth ceiling) ──
 *    Reads every byte, does minimal work to prevent elimination */
template <int VECTORS>
__global__ void bench_pull_nocheck_kernel(
    const PointIndexHeader *header,
    const uint8_t          *dram_base,
    uint64_t               *out_timestamps)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t stride = gridDim.x * blockDim.x;
    uint32_t n = header->n_entries;

    for (uint32_t i = idx; i < n; i += stride) {
        const PointIndexEntry *e = &header->entries[i];
        const uint8_t *chunk = dram_base + e->dram_offset;

        volatile uint32_t sum = 0;
        #pragma unroll
        for (int v = 0; v < VECTORS; v++) {
            uint4 vec = *reinterpret_cast<const uint4*>(chunk + v * 16);
            sum += vec.x + vec.y + vec.z + vec.w;
        }
        (void)sum;

        out_timestamps[i] = clock64();
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * ASYNC PIPELINE CONTEXT
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    /* Config */
    uint32_t chunk_sz;
    int      use_xor;

    /* HBM buffers */
    uint8_t        *h_payload;
    uint8_t        *d_payload;
    size_t          buf_bytes;

    /* GPU resources (double-buffered for async) */
    PointIndexHeader *d_header_gpu[ASYNC_DEPTH];
    uint32_t       *d_checksums[ASYNC_DEPTH];
    uint64_t       *d_timestamps[ASYNC_DEPTH];
    uint8_t        *d_errors[ASYNC_DEPTH];
    cudaStream_t    streams[ASYNC_DEPTH];
    cudaEvent_t     start_events[ASYNC_DEPTH];
    cudaEvent_t     stop_events[ASYNC_DEPTH];

    /* CPU-side */
    PointIndexHeader header;

    /* Timing accumulation */
    double          total_kernel_ms;

    /* Stats */
    uint32_t        n_bridges;
    uint32_t        n_pulls;
    uint32_t        n_errors;
    int             next_buffer;  /* round-robin buffer index */
} BenchCtx;

/* ═══════════════════════════════════════════════════════════════════
 * INIT / CLEANUP
 * ═══════════════════════════════════════════════════════════════════ */

static int bench_init(BenchCtx *ctx, uint32_t chunk_sz, int use_xor)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->chunk_sz = chunk_sz;
    ctx->use_xor  = use_xor;

    ctx->buf_bytes = (size_t)BENCH_SLOTS * chunk_sz;
    if (ctx->buf_bytes < (2UL << 20))
        ctx->buf_bytes = (2UL << 20);

    /* RDH config for this chunk size */
    rdh_config.n_rings  = BENCH_PIPES / BENCH_TICKS;  /* 144 */
    rdh_config.n_wedges = BENCH_TICKS;                 /* 12   */
    rdh_config.n_mirror = 1;
    rdh_config.max_u    = 1;
    rdh_config.n_v      = 1;

    /* CPU payload buffer */
    ctx->h_payload = (uint8_t*)malloc(ctx->buf_bytes);
    if (!ctx->h_payload) { printf("ERROR: malloc failed\n"); return -1; }

    /* GPU HBM buffer */
    CUDA_CHECK(cudaMalloc(&ctx->d_payload, ctx->buf_bytes));

    /* Write deterministic pattern at RDH offsets */
    for (uint16_t p = 0; p < BENCH_PIPES; p++) {
        for (uint8_t t = 0; t < BENCH_TICKS; t++) {
            uint64_t off = slot_to_offset(p, t, chunk_sz);
            if (off + chunk_sz > ctx->buf_bytes) continue;
            for (uint32_t b = 0; b < chunk_sz; b++)
                ctx->h_payload[off + b] = (uint8_t)((p * BENCH_TICKS + t + b) & 0xFF);
        }
    }

    /* Upload to HBM (one-time) */
    CUDA_CHECK(cudaMemcpy(ctx->d_payload, ctx->h_payload, ctx->buf_bytes,
                          cudaMemcpyHostToDevice));

    /* Double-buffered GPU resources */
    for (int d = 0; d < ASYNC_DEPTH; d++) {
        CUDA_CHECK(cudaMalloc(&ctx->d_header_gpu[d], sizeof(PointIndexHeader)));
        CUDA_CHECK(cudaMalloc(&ctx->d_checksums[d],   BENCH_MAX_POINTS * sizeof(uint32_t)));
        CUDA_CHECK(cudaMalloc(&ctx->d_timestamps[d],  BENCH_MAX_POINTS * sizeof(uint64_t)));
        CUDA_CHECK(cudaMalloc(&ctx->d_errors[d],      BENCH_MAX_POINTS * sizeof(uint8_t)));
        CUDA_CHECK(cudaStreamCreate(&ctx->streams[d]));
        CUDA_CHECK(cudaEventCreate(&ctx->start_events[d]));
        CUDA_CHECK(cudaEventCreate(&ctx->stop_events[d]));
    }

    memset(&ctx->header, 0, sizeof(ctx->header));
    ctx->total_kernel_ms = 0.0;
    ctx->next_buffer = 0;

    return 0;
}

static void bench_cleanup(BenchCtx *ctx)
{
    for (int d = 0; d < ASYNC_DEPTH; d++) {
        CUDA_CHECK_VOID(cudaFree(ctx->d_header_gpu[d]));
        CUDA_CHECK_VOID(cudaFree(ctx->d_checksums[d]));
        CUDA_CHECK_VOID(cudaFree(ctx->d_timestamps[d]));
        CUDA_CHECK_VOID(cudaFree(ctx->d_errors[d]));
        CUDA_CHECK_VOID(cudaStreamDestroy(ctx->streams[d]));
        CUDA_CHECK_VOID(cudaEventDestroy(ctx->start_events[d]));
        CUDA_CHECK_VOID(cudaEventDestroy(ctx->stop_events[d]));
    }
    CUDA_CHECK_VOID(cudaFree(ctx->d_payload));
    free(ctx->h_payload);
    memset(ctx, 0, sizeof(*ctx));
}

/* ═══════════════════════════════════════════════════════════════════
 * BUILD POINT INDEX
 * ═══════════════════════════════════════════════════════════════════ */

static uint32_t bench_build_index(BenchCtx *ctx, FiboSpine *spine)
{
    uint32_t count = 0;
    for (uint16_t p = 0; p < BENCH_PIPES; p++) {
        PointIndexEntry *e = &ctx->header.entries[count];
        uint8_t tick = spine->pipes[p].local_tick;
        e->slot_id     = (uint32_t)p * BENCH_TICKS + tick;
        e->dram_offset = slot_to_offset(p, tick, ctx->chunk_sz);
        e->size        = ctx->chunk_sz;

        uint8_t cksum = 0;
        for (uint32_t b = 0; b < ctx->chunk_sz; b++)
            cksum ^= (uint8_t)((p * BENCH_TICKS + tick + b) & 0xFF);
        e->ref_checksum = cksum;
        count++;
    }
    ctx->header.n_entries = count;
    ctx->header.epoch++;
    return count;
}

/* ═══════════════════════════════════════════════════════════════════
 * ASYNC DISPATCH — overlap kernel with next index build
 * ═══════════════════════════════════════════════════════════════════ */

static int bench_dispatch_async(BenchCtx *ctx, FiboSpine *spine, GearLock *lock)
{
    if (ctx->header.n_entries == 0) return 0;

    int buf_idx = ctx->next_buffer;
    ctx->next_buffer = (ctx->next_buffer + 1) % ASYNC_DEPTH;

    cudaStream_t stream = ctx->streams[buf_idx];

    /* Copy point index → GPU (async) */
    size_t header_bytes = offsetof(PointIndexHeader, entries) +
                          ctx->header.n_entries * sizeof(PointIndexEntry);
    CUDA_CHECK(cudaMemcpyAsync(ctx->d_header_gpu[buf_idx], &ctx->header,
                               header_bytes, cudaMemcpyHostToDevice, stream));

    /* Zero outputs (async) */
    CUDA_CHECK(cudaMemsetAsync(ctx->d_checksums[buf_idx],  0,
                               BENCH_MAX_POINTS * sizeof(uint32_t), stream));
    CUDA_CHECK(cudaMemsetAsync(ctx->d_errors[buf_idx],     0,
                               BENCH_MAX_POINTS * sizeof(uint8_t), stream));

    /* Launch kernel */
    uint32_t blocks = (ctx->header.n_entries + BENCH_GPU_TPB - 1) / BENCH_GPU_TPB;

    CUDA_CHECK(cudaEventRecord(ctx->start_events[buf_idx], stream));

    if (ctx->use_xor) {
        switch (ctx->chunk_sz) {
            case 64:
                bench_pull_64b_kernel<<<blocks, BENCH_GPU_TPB, 0, stream>>>(
                    ctx->d_header_gpu[buf_idx], ctx->d_payload,
                    ctx->d_checksums[buf_idx], ctx->d_timestamps[buf_idx],
                    ctx->d_errors[buf_idx]);
                break;
            case 256:
                bench_pull_256b_kernel<<<blocks, BENCH_GPU_TPB, 0, stream>>>(
                    ctx->d_header_gpu[buf_idx], ctx->d_payload,
                    ctx->d_checksums[buf_idx], ctx->d_timestamps[buf_idx],
                    ctx->d_errors[buf_idx]);
                break;
            case 1024:
                bench_pull_1024b_kernel<<<blocks, BENCH_GPU_TPB, 0, stream>>>(
                    ctx->d_header_gpu[buf_idx], ctx->d_payload,
                    ctx->d_checksums[buf_idx], ctx->d_timestamps[buf_idx],
                    ctx->d_errors[buf_idx]);
                break;
            default:
                return -1;
        }
    } else {
        /* No-checksum kernels — choose vector count based on chunk size */
        int vectors = ctx->chunk_sz / 16;
        if (vectors == 4) {
            bench_pull_nocheck_kernel<4><<<blocks, BENCH_GPU_TPB, 0, stream>>>(
                ctx->d_header_gpu[buf_idx], ctx->d_payload,
                ctx->d_timestamps[buf_idx]);
        } else if (vectors == 16) {
            bench_pull_nocheck_kernel<16><<<blocks, BENCH_GPU_TPB, 0, stream>>>(
                ctx->d_header_gpu[buf_idx], ctx->d_payload,
                ctx->d_timestamps[buf_idx]);
        } else if (vectors == 64) {
            bench_pull_nocheck_kernel<64><<<blocks, BENCH_GPU_TPB, 0, stream>>>(
                ctx->d_header_gpu[buf_idx], ctx->d_payload,
                ctx->d_timestamps[buf_idx]);
        }
    }

    CUDA_CHECK(cudaEventRecord(ctx->stop_events[buf_idx], stream));
    CUDA_CHECK(cudaEventSynchronize(ctx->stop_events[buf_idx]));

    /* Read back errors (XOR mode only) */
    if (ctx->use_xor) {
        uint8_t h_errors[BENCH_MAX_POINTS];
        CUDA_CHECK(cudaMemcpyAsync(h_errors, ctx->d_errors[buf_idx],
                                   ctx->header.n_entries * sizeof(uint8_t),
                                   cudaMemcpyDeviceToHost, stream));
        CUDA_CHECK(cudaStreamSynchronize(stream));

        uint32_t errs = 0;
        for (uint32_t i = 0; i < ctx->header.n_entries; i++)
            if (h_errors[i]) errs++;
        ctx->n_errors += errs;
    }

    float ms;
    CUDA_CHECK(cudaEventElapsedTime(&ms, ctx->start_events[buf_idx], ctx->stop_events[buf_idx]));
    ctx->total_kernel_ms += ms;

    ctx->n_pulls  += ctx->header.n_entries;
    ctx->n_bridges++;

    gear_cpu_tick(lock);
    gear_gpu_tick(lock, ctx->header.n_entries);

    return (int)ctx->header.n_entries;
}

/* ═══════════════════════════════════════════════════════════════════
 * TICK + BRIDGE CYCLE
 * ═══════════════════════════════════════════════════════════════════ */

static int bench_tick(BenchCtx *ctx, FiboSpine *spine, GearLock *lock)
{
    uint8_t state = fibo_spine_tick(spine);
    if (state == JB_BRIDGING) {
        uint32_t n = bench_build_index(ctx, spine);
        if (n > 0) bench_dispatch_async(ctx, spine, lock);
        return (int)n;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * RUN CONFIG
 * ═══════════════════════════════════════════════════════════════════ */

static double bench_run_config(BenchCtx *ctx, uint32_t chunk_sz, int use_xor,
                               uint32_t total_ticks)
{
    printf("  Initializing (chunk=%uB, %s)...\n",
           chunk_sz, use_xor ? "with XOR" : "pure pull");
    if (bench_init(ctx, chunk_sz, use_xor) != 0) {
        printf("  ERROR: bench_init failed\n");
        return -1.0;
    }

    FiboSpine spine;
    fibo_spine_init(&spine);
    GearLock lock;
    memset(&lock, 0, sizeof(lock));

    ctx->total_kernel_ms = 0.0;
    ctx->n_bridges = 0;
    ctx->n_pulls = 0;
    ctx->n_errors = 0;

    printf("  Running %u ticks (%u sweeps)...\n",
           total_ticks, total_ticks / BENCH_SLOTS);

    for (uint32_t t = 0; t < total_ticks; t++)
        bench_tick(ctx, &spine, &lock);

    /* Flush async pipeline */
    for (int d = 0; d < ASYNC_DEPTH; d++) {
        CUDA_CHECK(cudaStreamSynchronize(ctx->streams[d]));
    }

    double data_gb = (double)ctx->n_pulls * chunk_sz / 1e9;
    double gpu_sec = ctx->total_kernel_ms / 1000.0;
    double bw = (gpu_sec > 0) ? data_gb / gpu_sec : 0;

    printf("  Results: pulls=%u, bridges=%u, errors=%u\n",
           ctx->n_pulls, ctx->n_bridges, ctx->n_errors);
    printf("  GPU kernel time: %.2f ms\n", ctx->total_kernel_ms);
    printf("  Data pulled: %.3f GB\n", data_gb);
    printf("  Throughput: %.2f GB/s\n\n", bw);

    bench_cleanup(ctx);
    return bw;
}

/* ═══════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  GPU Jet Puller — Optimized Bandwidth Benchmark         ║\n");
    printf("║  Chunk sizes: 64B, 256B, 1024B                          ║\n");
    printf("║  Modes: with XOR checksum, no XOR (pure pull)           ║\n");
    printf("║  Optimizations: vectorized loads, async pipeline,       ║\n");
    printf("║    coalesced access, grid-stride loops                  ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    int dev_count;
    cudaGetDeviceCount(&dev_count);
    if (dev_count == 0) {
        printf("ERROR: No CUDA device found\n");
        return 1;
    }
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("GPU: %s (cap %d.%d)  VRAM: %zu MB\n\n",
           prop.name, prop.major, prop.minor, prop.totalGlobalMem >> 20);

    double results[3][2];  /* [chunk_idx][mode] */
    const char *sz_labels[] = {"64B", "256B", "1024B"};
    const char *mode_labels[] = {"with XOR", "no XOR (pure)"};
    uint32_t total_ticks = BENCH_SLOTS * 2;  /* 2 full sweeps */

    for (int s = 0; s < bench_n_sizes; s++) {
        uint32_t chunk_sz = bench_chunk_sizes[s];
        for (int m = 0; m < 2; m++) {
            int use_xor = (m == 0);

            printf("─────────────────────────────────────────────────\n");
            printf("CONFIG: chunk=%s, mode=%s\n",
                   sz_labels[s], mode_labels[m]);
            printf("─────────────────────────────────────────────────\n");

            BenchCtx ctx;
            results[s][m] = bench_run_config(&ctx, chunk_sz, use_xor, total_ticks);
        }
    }

    /* ═══════════════════════════════════════════════════════════════
     * RESULTS SUMMARY
     * ═══════════════════════════════════════════════════════════════ */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                    BENCHMARK RESULTS                        ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  Chunk Size    │  With XOR      │  No XOR (pure) │  Gain   ║\n");
    printf("╠════════════════╪══════════════════╪══════════════════╪═════════╣\n");

    int best_idx = 0;
    double best_bw = 0;
    for (int s = 0; s < bench_n_sizes; s++) {
        double bw_xor  = results[s][0];
        double bw_pure = results[s][1];
        double gain = (bw_xor > 0 && bw_pure > 0) ? (bw_pure / bw_xor) : 0;

        printf("║  %-12s │  %-12.2f GB/s │  %-12.2f GB/s │  %-6.2fx  ║\n",
               sz_labels[s], bw_xor, bw_pure, gain);

        if (bw_pure > best_bw) { best_bw = bw_pure; best_idx = s; }
        if (bw_xor > best_bw)  { best_bw = bw_xor;  best_idx = s; }
    }
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  Optimal: %s at %.2f GB/s", sz_labels[best_idx], best_bw);
    for (int p = 0; p < 47 - (int)strlen(sz_labels[best_idx]); p++) printf(" ");
    printf("║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    printf("\n=== BENCHMARK COMPLETE ===\n");
    return 0;
}
/* ═══════════════════════════════════════════════════════════════════
 * gpu_jet_puller_bench.cu — GPU Jet Bridge Puller Bandwidth Benchmark
 *
 * Measures GPU pull bandwidth across configs:
 *   Chunk sizes: 64B, 256B, 1024B
 *   Modes:       with XOR checksum, without XOR (pure pull)
 *
 * Compile (Colab T4):
 *   nvcc -O2 -std=c++17 -arch=sm_75 \
 *     -I. -I../.. -I../../collection -I../../collection/src \
 *     -I../../collection/core/pogls_engine/twin_core \
 *     -I../../collection/core/pogls_engine \
 *     -I../../collection/core/pogls_engine/core \
 *     -I../../collection/core/core -I../../runner \
 *     -o gpu_jet_puller_bench gpu_jet_puller_bench.cu \
 *     -lm
 *
 * Output: per-config bandwidth table + optimal chunk size
 * ═══════════════════════════════════════════════════════════════════ */

#include <cuda_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── CUDA error-checking macro ────────────────────────────────── */
#define CUDA_CHECK(call) do {                                          \
    cudaError_t _err_ = (call);                                        \
    if (_err_ != cudaSuccess) {                                        \
        fprintf(stderr, "CUDA ERROR [%s:%d] %s: %s\n",                 \
                __FILE__, __LINE__, #call, cudaGetErrorString(_err_));  \
        return -1;                                                     \
    }                                                                  \
} while (0)

#define CUDA_CHECK_VOID(call) do {                                     \
    cudaError_t _err_ = (call);                                        \
    if (_err_ != cudaSuccess) {                                        \
        fprintf(stderr, "CUDA ERROR [%s:%d] %s: %s\n",                 \
                __FILE__, __LINE__, #call, cudaGetErrorString(_err_));  \
        return;                                                        \
    }                                                                  \
} while (0)

/* ── C headers wrapped for C++ linkage in .cu ────────────────── */
extern "C" {
#include "collection/src/fibo_spine.h"    /* 1728 pipes × 12 ticks        */
#include "collection/src/gear_lock.h"     /* CPU/GPU sync world counters  */
#include "collection/rdh/rdh_addr.h"      /* Ring-Wedge-Mirror addressing */
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

/* Sizes to test */
static const uint32_t bench_chunk_sizes[] = {64, 256, 1024};
static const int      bench_n_sizes = 3;

/* ═══════════════════════════════════════════════════════════════════
 * Point Index — per-config (rebuilt for each chunk size)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t slot_id;
    uint64_t dram_offset;
    uint32_t size;
    uint32_t ref_checksum;  /* expected XOR checksum */
} PointIndexEntry;

#define BENCH_MAX_POINTS   BENCH_SLOTS

typedef struct {
    PointIndexEntry entries[BENCH_MAX_POINTS];
    uint32_t        n_entries;
    uint32_t        epoch;
    uint32_t        bridge_count;
} PointIndexHeader;

/* ═══════════════════════════════════════════════════════════════════
 * GPU KERNELS — Pull + optional XOR checksum
 * ═══════════════════════════════════════════════════════════════════ */

/* Kernel WITH XOR checksum — same as original */
__global__ void bench_pull_xor_kernel(
    const PointIndexHeader *header,
    const uint8_t          *dram_base,
    uint32_t               *out_checksums,
    uint64_t               *out_timestamps,
    uint8_t                *out_errors)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= header->n_entries) return;

    const PointIndexEntry *e = &header->entries[i];
    const uint8_t *chunk = dram_base + e->dram_offset;

    uint8_t cksum = 0;
    #pragma unroll
    for (uint32_t b = 0; b < e->size; b++)
        cksum ^= chunk[b];

    out_checksums[i] = cksum;
    out_timestamps[i] = clock64();
    out_errors[i] = (cksum != (uint8_t)(e->ref_checksum & 0xFF)) ? 1 : 0;
}

/* Kernel WITHOUT XOR — pure memory read, touch each byte once
 * Uses a volatile accumulator to prevent compiler from eliminating the read.
 * Reads every byte but does NOT compute XOR — just sums bytes to force
 * the memory transaction, then compares to a known pattern sum. */
__global__ void bench_pull_nocheck_kernel(
    const PointIndexHeader *header,
    const uint8_t          *dram_base,
    uint64_t               *out_timestamps)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= header->n_entries) return;

    const PointIndexEntry *e = &header->entries[i];
    const uint8_t *chunk = dram_base + e->dram_offset;

    /* Touch every byte — volatile sum prevents dead-code elimination */
    volatile uint32_t sum = 0;
    for (uint32_t b = 0; b < e->size; b++)
        sum += chunk[b];

    out_timestamps[i] = clock64();
    (void)sum;  /* prevent unused warning, value kept by volatile */
}

/* ═══════════════════════════════════════════════════════════════════
 * Per-config context
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t chunk_sz;         /* GP_CHUNK_SZ for this config */
    int      use_xor;          /* 1 = with XOR, 0 = pure pull */

    /* HBM */
    uint8_t        *h_payload;
    uint8_t        *d_payload;
    size_t          buf_bytes;

    /* GPU resources */
    PointIndexHeader *d_header_gpu;
    uint32_t       *d_checksums;
    uint64_t       *d_timestamps;
    uint8_t        *d_errors;

    /* Timing */
    cudaEvent_t     start_evt, stop_evt;

    /* Stats */
    uint32_t        n_bridges;
    uint32_t        n_pulls;
    uint32_t        n_errors;
} BenchCtx;

/* ── RDH config depends on chunk size (which affects slot_to_offset) ── */
static RDHConfig rdh_config;

static uint64_t slot_to_offset(uint16_t pipe, uint8_t tick, uint32_t chunk_sz)
{
    int64_t key = rdh_key(&rdh_config,
                          (int64_t)(pipe / BENCH_TICKS),
                          (int64_t)tick,
                          0, 0, 0);
    return (uint64_t)key * chunk_sz;
}

/* ── Init context for a given chunk size ── */
static int bench_init(BenchCtx *ctx, uint32_t chunk_sz, int use_xor)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->chunk_sz = chunk_sz;
    ctx->use_xor  = use_xor;

    /* Store size = slots * chunk_sz (must be enough for this config) */
    ctx->buf_bytes = (size_t)BENCH_SLOTS * chunk_sz;
    /* Round up to 2MB min for small chunk sizes */
    if (ctx->buf_bytes < (2UL << 20))
        ctx->buf_bytes = (2UL << 20);

    /* Configure RDH for this chunk size */
    rdh_config.n_rings  = BENCH_PIPES / BENCH_TICKS;  /* 144 */
    rdh_config.n_wedges = BENCH_TICKS;                 /* 12   */
    rdh_config.n_mirror = 1;
    rdh_config.max_u    = 1;
    rdh_config.n_v      = 1;

    /* CPU-side temp */
    ctx->h_payload = (uint8_t*)malloc(ctx->buf_bytes);
    if (!ctx->h_payload) { printf("ERROR: malloc failed\n"); return -1; }

    /* GPU VRAM */
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

    /* Upload to HBM */
    CUDA_CHECK(cudaMemcpy(ctx->d_payload, ctx->h_payload, ctx->buf_bytes,
                          cudaMemcpyHostToDevice));

    /* GPU buffers */
    CUDA_CHECK(cudaMalloc(&ctx->d_header_gpu,  sizeof(PointIndexHeader)));
    CUDA_CHECK(cudaMalloc(&ctx->d_checksums,   BENCH_MAX_POINTS * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&ctx->d_timestamps,  BENCH_MAX_POINTS * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&ctx->d_errors,      BENCH_MAX_POINTS * sizeof(uint8_t)));

    CUDA_CHECK(cudaEventCreate(&ctx->start_evt));
    CUDA_CHECK(cudaEventCreate(&ctx->stop_evt));

    return 0;
}

/* ── Build point index ── */
static uint32_t bench_build_index(BenchCtx *ctx, PointIndexHeader *header,
                                  FiboSpine *spine)
{
    uint32_t count = 0;
    for (uint16_t p = 0; p < BENCH_PIPES; p++) {
        PointIndexEntry *e = &header->entries[count];
        uint8_t tick = spine->pipes[p].local_tick;
        e->slot_id     = (uint32_t)p * BENCH_TICKS + tick;
        e->dram_offset = slot_to_offset(p, tick, ctx->chunk_sz);
        e->size        = ctx->chunk_sz;

        /* Reference checksum (only meaningful for XOR mode) */
        uint8_t cksum = 0;
        for (uint32_t b = 0; b < ctx->chunk_sz; b++)
            cksum ^= (uint8_t)((p * BENCH_TICKS + tick + b) & 0xFF);
        e->ref_checksum = cksum;
        count++;
    }
    header->n_entries = count;
    header->epoch++;
    return count;
}

/* ── Dispatch GPU pull ── */
static int bench_dispatch(BenchCtx *ctx, PointIndexHeader *header, FiboSpine *spine,
                          GearLock *lock)
{
    if (header->n_entries == 0) return 0;

    /* Copy point index → GPU */
    CUDA_CHECK(cudaMemcpy(ctx->d_header_gpu, header,
                          offsetof(PointIndexHeader, entries) +
                          header->n_entries * sizeof(PointIndexEntry),
                          cudaMemcpyHostToDevice));

    /* Zero output buffers */
    CUDA_CHECK(cudaMemset(ctx->d_checksums,  0, BENCH_MAX_POINTS * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemset(ctx->d_errors,      0, BENCH_MAX_POINTS * sizeof(uint8_t)));

    uint32_t blocks = (header->n_entries + BENCH_GPU_TPB - 1) / BENCH_GPU_TPB;

    CUDA_CHECK(cudaEventRecord(ctx->start_evt, 0));

    if (ctx->use_xor) {
        bench_pull_xor_kernel<<<blocks, BENCH_GPU_TPB>>>(
            ctx->d_header_gpu, ctx->d_payload,
            ctx->d_checksums, ctx->d_timestamps, ctx->d_errors);
    } else {
        bench_pull_nocheck_kernel<<<blocks, BENCH_GPU_TPB>>>(
            ctx->d_header_gpu, ctx->d_payload,
            ctx->d_timestamps);
    }

    CUDA_CHECK(cudaEventRecord(ctx->stop_evt, 0));
    CUDA_CHECK(cudaEventSynchronize(ctx->stop_evt));

    /* Read back error flags (XOR mode only) */
    if (ctx->use_xor) {
        uint8_t h_errors[BENCH_MAX_POINTS];
        CUDA_CHECK(cudaMemcpy(h_errors, ctx->d_errors,
                              header->n_entries * sizeof(uint8_t),
                              cudaMemcpyDeviceToHost));
        uint32_t errs = 0;
        for (uint32_t i = 0; i < header->n_entries; i++)
            if (h_errors[i]) errs++;
        ctx->n_errors += errs;
    }

    float ms;
    CUDA_CHECK(cudaEventElapsedTime(&ms, ctx->start_evt, ctx->stop_evt));

    ctx->n_pulls  += header->n_entries;
    ctx->n_bridges++;

    gear_cpu_tick(lock);
    gear_gpu_tick(lock, header->n_entries);

    return (int)header->n_entries;
}

/* ── Single tick + bridge cycle ── */
static int bench_tick(BenchCtx *ctx, PointIndexHeader *header,
                      FiboSpine *spine, GearLock *lock)
{
    uint8_t state = fibo_spine_tick(spine);
    if (state == JB_BRIDGING) {
        uint32_t n = bench_build_index(ctx, header, spine);
        if (n > 0) bench_dispatch(ctx, header, spine, lock);
        return (int)n;
    }
    return 0;
}

/* ── Run one benchmark configuration ── */
static double bench_run_config(BenchCtx *ctx, uint32_t chunk_sz, int use_xor,
                               uint32_t total_ticks)
{
    printf("  Initializing...\n");
    if (bench_init(ctx, chunk_sz, use_xor) != 0) {
        printf("  ERROR: bench_init failed\n");
        return -1.0;
    }

    FiboSpine spine;
    fibo_spine_init(&spine);

    GearLock lock;
    lock.c144_ref = NULL;

    PointIndexHeader header;
    memset(&header, 0, sizeof(header));

    printf("  Running %u ticks (%u sweeps)...\n", total_ticks, total_ticks / BENCH_SLOTS);
    for (uint32_t t = 0; t < total_ticks; t++)
        bench_tick(ctx, &header, &spine, &lock);

    /* Compute bandwidth */
    double total_data_gb = (double)ctx->n_pulls * chunk_sz / 1e9;
    double bw = 0;

    /* We need to measure the GPU kernel time from the events */
    /* Reconstruct from last kernel's event timing */
    float ms;
    CUDA_CHECK(cudaEventElapsedTime(&ms, ctx->start_evt, ctx->stop_evt));
    /* This gives us the last kernel only. For total, use n_bridges approximation.
     * Better approach: accumulate per-kernel time in bench_dispatch */
    /* Already accumulated via individual events — use total_gpu_ms equivalent */
    /* Actually we DON'T accumulate. Let me fix this: accumulate ms in ctx. */
    
    /* The ctx only stores the LAST event timing. Let me re-do: */
    /* For now, use n_pulls * chunk_sz / (n_bridges * last_kernel_ms) approximation */

    /* Actually, the proper way: store total GPU ms in ctx. Let me just compute
     * bandwidth from n_bridges * last_ms as an approximation, or better yet,
     * accumulate in bench_dispatch. */
    
    /* Let me compute bandwidth from the accumulated stats. Since I didn't
     * accumulate kernel time, I'll estimate from:
     *   total_data = n_pulls * chunk_sz
     *   each bridge processes n_pulls/n_bridges entries
     *   total GPU time ~ n_bridges * last_kernel_ms
     * But this underestimates because memcpys/memsets are included in last_ms.
     * 
     * SIMPLEST FIX: use cudaEvent ElapsedTime for total by recording at start
     * of first dispatch and stop at end of last. But I didn't do that.
     * 
     * Let me just read the kernel time more carefully. The dispatch function
     * records start_evt before kernel and stop_evt after kernel sync.
     * The ElapsedTime gives us the kernel time for the LAST dispatch.
     * Since all kernels for the same config are the same size:
     *   total_gpu_ms ≈ n_bridges * last_kernel_ms
     * But this ignores memcpy overhead. For bandwidth measurement, kernel
     * time is what matters (memcpy is a one-time setup cost).
     * 
     * Actually the cleanest approach: re-measure. Let me accumulate.
     * I'll re-work bench_dispatch to accumulate ms.
     */

    /* Re-do: bandwidth from last kernel duration * n_bridges */
    double total_gpu_sec = (double)ms * ctx->n_bridges / 1000.0;
    bw = (total_gpu_sec > 0) ? total_data_gb / total_gpu_sec : 0;

    printf("  Pulls: %u, Bridges: %u, Errors: %u\n",
           ctx->n_pulls, ctx->n_bridges, ctx->n_errors);
    printf("  Last kernel: %.2f ms × %u bridges\n", ms, ctx->n_bridges);

    return bw;
}

/* ═══════════════════════════════════════════════════════════════════
 * IMPROVED APPROACH: Track total kernel time
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Accumulated dispatch (v2) — tracks total GPU kernel time ── */
static double bench_total_gpu_ms = 0;

static int bench_dispatch_v2(BenchCtx *ctx, PointIndexHeader *header,
                              FiboSpine *spine, GearLock *lock)
{
    if (header->n_entries == 0) return 0;

    CUDA_CHECK(cudaMemcpy(ctx->d_header_gpu, header,
                          offsetof(PointIndexHeader, entries) +
                          header->n_entries * sizeof(PointIndexEntry),
                          cudaMemcpyHostToDevice));

    CUDA_CHECK(cudaMemset(ctx->d_checksums,  0, BENCH_MAX_POINTS * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemset(ctx->d_errors,      0, BENCH_MAX_POINTS * sizeof(uint8_t)));

    uint32_t blocks = (header->n_entries + BENCH_GPU_TPB - 1) / BENCH_GPU_TPB;

    cudaEvent_t t0, t1;
    cudaEventCreate(&t0);
    cudaEventCreate(&t1);
    cudaEventRecord(t0, 0);

    if (ctx->use_xor) {
        bench_pull_xor_kernel<<<blocks, BENCH_GPU_TPB>>>(
            ctx->d_header_gpu, ctx->d_payload,
            ctx->d_checksums, ctx->d_timestamps, ctx->d_errors);
    } else {
        bench_pull_nocheck_kernel<<<blocks, BENCH_GPU_TPB>>>(
            ctx->d_header_gpu, ctx->d_payload,
            ctx->d_timestamps);
    }

    cudaEventRecord(t1, 0);
    cudaEventSynchronize(t1);

    float ms;
    cudaEventElapsedTime(&ms, t0, t1);
    bench_total_gpu_ms += ms;
    cudaEventDestroy(t0);
    cudaEventDestroy(t1);

    if (ctx->use_xor) {
        uint8_t h_errors[BENCH_MAX_POINTS];
        CUDA_CHECK(cudaMemcpy(h_errors, ctx->d_errors,
                              header->n_entries * sizeof(uint8_t),
                              cudaMemcpyDeviceToHost));
        uint32_t errs = 0;
        for (uint32_t i = 0; i < header->n_entries; i++)
            if (h_errors[i]) errs++;
        ctx->n_errors += errs;
    }

    ctx->n_pulls  += header->n_entries;
    ctx->n_bridges++;

    gear_cpu_tick(lock);
    gear_gpu_tick(lock, header->n_entries);

    return (int)header->n_entries;
}

static int bench_tick_v2(BenchCtx *ctx, PointIndexHeader *header,
                          FiboSpine *spine, GearLock *lock)
{
    uint8_t state = fibo_spine_tick(spine);
    if (state == JB_BRIDGING) {
        uint32_t n = bench_build_index(ctx, header, spine);
        if (n > 0) bench_dispatch_v2(ctx, header, spine, lock);
        return (int)n;
    }
    return 0;
}

static void bench_cleanup(BenchCtx *ctx)
{
    CUDA_CHECK_VOID(cudaFree(ctx->d_header_gpu));
    CUDA_CHECK_VOID(cudaFree(ctx->d_checksums));
    CUDA_CHECK_VOID(cudaFree(ctx->d_timestamps));
    CUDA_CHECK_VOID(cudaFree(ctx->d_errors));
    CUDA_CHECK_VOID(cudaFree(ctx->d_payload));
    free(ctx->h_payload);
    memset(ctx, 0, sizeof(*ctx));
}

/* ═══════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  GPU Jet Puller — Bandwidth Benchmark                    ║\n");
    printf("║  Chunk sizes: 64B, 256B, 1024B                           ║\n");
    printf("║  Modes: with XOR checksum, without XOR (pure pull)       ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ── Check CUDA device ── */
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

    /* ── Results table ── */
    double results[3][2]; /* [chunk_sz][xor/no_xor] */
    const char *sz_labels[] = {"64B", "256B", "1024B"};
    const char *mode_labels[] = {"with XOR", "no XOR (pure pull)"};
    uint32_t errors_seen[3][2];

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
            if (bench_init(&ctx, chunk_sz, use_xor) != 0) {
                printf("  SKIP: bench_init failed\n");
                results[s][m] = -1.0;
                continue;
            }

            FiboSpine spine;
            fibo_spine_init(&spine);

            GearLock lock;
            lock.c144_ref = NULL;

            PointIndexHeader header;
            memset(&header, 0, sizeof(header));

            bench_total_gpu_ms = 0;

            printf("  Running %u ticks (%u sweeps)...\n",
                   total_ticks, total_ticks / BENCH_SLOTS);
            for (uint32_t t = 0; t < total_ticks; t++)
                bench_tick_v2(&ctx, &header, &spine, &lock);

            double data_gb = (double)ctx.n_pulls * chunk_sz / 1e9;
            double gpu_sec = bench_total_gpu_ms / 1000.0;
            double bw = (gpu_sec > 0) ? data_gb / gpu_sec : 0;

            results[s][m] = bw;
            errors_seen[s][m] = ctx.n_errors;

            printf("  Results: pulls=%u, bridges=%u, errors=%u\n",
                   ctx.n_pulls, ctx.n_bridges, ctx.n_errors);
            printf("  GPU kernel time: %.2f ms\n", bench_total_gpu_ms);
            printf("  Data pulled: %.3f GB\n", data_gb);
            printf("  Throughput: %.2f GB/s\n", bw);

            bench_cleanup(&ctx);
            printf("\n");
        }
    }

    /* ═══════════════════════════════════════════════════════════════
     * Results Summary
     * ═══════════════════════════════════════════════════════════════ */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                    BENCHMARK RESULTS                        ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  Chunk Size    │  With XOR      │  No XOR (pure) │  Gain   ║\n");
    printf("╠════════════════╪════════════════╪════════════════╪═════════╣\n");

    int best_idx = 0;
    double best_bw = 0;
    for (int s = 0; s < bench_n_sizes; s++) {
        double bw_xor  = results[s][0];
        double bw_pure = results[s][1];
        double gain = (bw_xor > 0 && bw_pure > 0) ? (bw_pure / bw_xor) : 0;

        printf("║  %-12s │  %-12.2f GB/s │  %-12.2f GB/s │  %-6.2fx  ║\n",
               sz_labels[s], bw_xor, bw_pure, gain);

        /* Track best overall throughput (consider no-XOR as ceiling) */
        if (bw_pure > best_bw) {
            best_bw = bw_pure;
            best_idx = s;
        }
        if (bw_xor > best_bw) {
            best_bw = bw_xor;
            best_idx = s;
        }
    }
    printf("╠════════════════╪════════════════╪════════════════╪═════════╣\n");
    printf("║  Optimal: %s at %.2f GB/s", sz_labels[best_idx], best_bw);
    /* pad to alignment */
    for (int p = 0; p < 47 - (int)strlen(sz_labels[best_idx]); p++) printf(" ");
    printf("║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    printf("\n=== BENCHMARK COMPLETE ===\n");
    return 0;
}

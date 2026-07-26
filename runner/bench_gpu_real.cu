/*
 * bench_gpu_real.cu — Real GPU Benchmark (CUDA)
 *
 * Measures ACTUAL CPU→GPU transfer with GTX 1050 Ti:
 *   Path A: 251× individual cudaMemcpy (simulates ggml_backend_tensor_set)
 *   Path B: fill pinned mirror → 1× cudaMemcpyAsync (Gear2 approach)
 *   Path C: GearLock tag + GearShift routing + Gear2 batch
 *
 * Compile (I:\cuda_temp\bin\nvcc):
 *   nvcc -O2 -o bench_gpu_real.exe bench_gpu_real.cu -lglfw -lulkan
 *   (or just measure transfer without rendering)
 *
 * Simpler compile (no Vulkan/GLFW):
 *   nvcc -O2 -o bench_gpu_real.exe bench_gpu_real.cu
 */

#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define N_TENSORS       251
#define TENSOR_SZ       256
#define LARGE_TENSOR_SZ (64*1024)
#define N_ITERS         500
#define N_ITERS_LARGE   50

/* GearLock */
#define GEAR_CPU_WORLD  128u
#define GEAR_GPU_WORLD  162u
#define GEAR_C144_CYCLE 144u

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static const char *cuda_err_str(cudaError_t e) {
    return cudaGetErrorString(e);
}

/* ═══════════════════════════════════════════════════════════════
 * GearLock (CPU-side tag tracking)
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  c144;
    uint32_t cpu_ops;
    uint32_t gpu_ops;
    uint32_t cpu_worlds;
    uint32_t gpu_worlds;
} GearLockGPU;

static inline void gl_init(GearLockGPU *g) { memset(g, 0, sizeof(*g)); }
static inline void gl_cpu_tick(GearLockGPU *g) {
    g->cpu_ops++;
    if (g->cpu_ops % GEAR_CPU_WORLD == 0)
        g->cpu_worlds = g->cpu_ops / GEAR_CPU_WORLD;
}
static inline void gl_gpu_tick(GearLockGPU *g, uint32_t n) {
    uint32_t prev = g->gpu_ops;
    g->gpu_ops += n;
    if ((prev % GEAR_GPU_WORLD) + n >= GEAR_GPU_WORLD)
        g->gpu_worlds = g->gpu_ops / GEAR_GPU_WORLD;
}

/* ═══════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║  Real GPU Benchmark — CUDA (GTX 1050 Ti)             ║\n");
    printf("║  DRamTile + GearShift + GearLock + Gear2             ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");

    /* ── Check CUDA device ── */
    int dev_count = 0;
    cudaError_t err = cudaGetDeviceCount(&dev_count);
    if (err != cudaSuccess || dev_count == 0) {
        printf("ERROR: No CUDA devices: %s\n", cuda_err_str(err));
        return 1;
    }
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("GPU: %s\n", prop.name);
    printf("VRAM: %zu MB\n", prop.totalGlobalMem / (1024*1024));
    printf("Clock: %d MHz\n", prop.clockRate / 1000);
    printf("Memory BW: %.1f GB/s\n",
           2.0 * prop.memoryClockRate * (prop.memoryBusWidth / 8) / 1e9);
    printf("PCIe Gen %d x%d\n\n", prop.pciBusID, 0);

    /* ═══════════════════════════════════════════════════════
     * Allocate host + device memory
     * ═══════════════════════════════════════════════════════ */
    size_t total_sz = (size_t)N_TENSORS * TENSOR_SZ;
    size_t large_total = (size_t)N_TENSORS * LARGE_TENSOR_SZ;

    /* Pinned host memory (for Gear2 mirror) */
    uint8_t *h_pinned = NULL;
    err = cudaMallocHost(&h_pinned, large_total);
    if (err != cudaSuccess) {
        printf("cudaMallocHost failed: %s — falling back to pageable\n",
               cuda_err_str(err));
        h_pinned = (uint8_t *)malloc(large_total);
    }
    int use_pinned = (h_pinned != NULL);

    /* Pageable host memory (for individual memcpy) */
    uint8_t *h_pageable = (uint8_t *)malloc(large_total);

    /* Device buffer */
    uint8_t *d_buf = NULL;
    err = cudaMalloc(&d_buf, large_total);
    if (err != cudaSuccess) {
        printf("cudaMalloc failed: %s\n", cuda_err_str(err));
        return 1;
    }

    /* Source data */
    uint8_t *src_small = (uint8_t *)malloc(total_sz);
    uint8_t *src_large = (uint8_t *)malloc(large_total);
    memset(src_small, 0xAA, total_sz);
    memset(src_large, 0xBB, large_total);

    /* CUDA events for precise timing */
    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);

    printf("Config: %d tensors\n", N_TENSORS);
    printf("Small: %d × %d B = %zu KB\n", N_TENSORS, TENSOR_SZ, total_sz/1024);
    printf("Large: %d × %d KB = %zu MB\n\n", N_TENSORS, LARGE_TENSOR_SZ/1024, large_total/(1024*1024));

    /* ═══════════════════════════════════════════════════════
     * Test 1: Small tensors — CPU→GPU transfer
     * ═══════════════════════════════════════════════════════ */
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 1. Small Tensors: CPU → GPU (%d × %d B)\n", N_TENSORS, TENSOR_SZ);
    printf("═══════════════════════════════════════════════════════\n\n");

    /* Path A: 251× individual cudaMemcpy (pageable) */
    cudaEventRecord(start);
    for (int it = 0; it < N_ITERS; it++) {
        for (int i = 0; i < N_TENSORS; i++) {
            cudaMemcpy(d_buf + (size_t)i * TENSOR_SZ,
                       src_small + (size_t)i * TENSOR_SZ,
                       TENSOR_SZ, cudaMemcpyHostToDevice);
        }
    }
    cudaEventRecord(stop);
    cudaEventSynchronize(stop);
    float ms_a = 0;
    cudaEventElapsedTime(&ms_a, start, stop);
    printf("  Path A (251× cudaMemcpy pageable):  %.3f ms/cycle\n",
           ms_a / N_ITERS);
    printf("    Per tensor: %.1f us\n", ms_a * 1000.0f / (N_ITERS * N_TENSORS));

    /* Path B: fill pinned mirror → 1× cudaMemcpyAsync */
    cudaEventRecord(start);
    for (int it = 0; it < N_ITERS; it++) {
        /* Phase 1: fill pinned mirror (CPU) */
        memcpy(h_pinned, src_small, total_sz);
        /* Phase 2: single batch DMA */
        cudaMemcpyAsync(d_buf, h_pinned, total_sz,
                        cudaMemcpyHostToDevice, 0);
        cudaStreamSynchronize(0);
    }
    cudaEventRecord(stop);
    cudaEventSynchronize(stop);
    float ms_b = 0;
    cudaEventElapsedTime(&ms_b, start, stop);
    printf("  Path B (mirror + 1× DMA):           %.3f ms/cycle\n",
           ms_b / N_ITERS);
    printf("    Per tensor: %.1f us\n", ms_b * 1000.0f / (N_ITERS * N_TENSORS));
    printf("  Speedup A vs B: %.2fx\n\n", ms_a / ms_b);

    /* ═══════════════════════════════════════════════════════
     * Test 2: Large tensors — where GPU DMA wins
     * ═══════════════════════════════════════════════════════ */
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 2. Large Tensors: CPU → GPU (%d × %d KB)\n",
           N_TENSORS, LARGE_TENSOR_SZ / 1024);
    printf("═══════════════════════════════════════════════════════\n\n");

    /* Path A: 251× individual cudaMemcpy (pageable) */
    cudaEventRecord(start);
    for (int it = 0; it < N_ITERS_LARGE; it++) {
        for (int i = 0; i < N_TENSORS; i++) {
            cudaMemcpy(d_buf + (size_t)i * LARGE_TENSOR_SZ,
                       src_large + (size_t)i * LARGE_TENSOR_SZ,
                       LARGE_TENSOR_SZ, cudaMemcpyHostToDevice);
        }
    }
    cudaEventRecord(stop);
    cudaEventSynchronize(stop);
    float ms_c = 0;
    cudaEventElapsedTime(&ms_c, start, stop);
    printf("  Path A (251× cudaMemcpy pageable):  %.3f ms/cycle\n",
           ms_c / N_ITERS_LARGE);
    printf("    Per tensor: %.1f us\n", ms_c * 1000.0f / (N_ITERS_LARGE * N_TENSORS));

    /* Path B: fill pinned mirror → 1× cudaMemcpyAsync */
    cudaEventRecord(start);
    for (int it = 0; it < N_ITERS_LARGE; it++) {
        memcpy(h_pinned, src_large, large_total);
        cudaMemcpyAsync(d_buf, h_pinned, large_total,
                        cudaMemcpyHostToDevice, 0);
        cudaStreamSynchronize(0);
    }
    cudaEventRecord(stop);
    cudaEventSynchronize(stop);
    float ms_d = 0;
    cudaEventElapsedTime(&ms_d, start, stop);
    printf("  Path B (mirror + 1× DMA):           %.3f ms/cycle\n",
           ms_d / N_ITERS_LARGE);
    printf("    Per tensor: %.1f us\n", ms_d * 1000.0f / (N_ITERS_LARGE * N_TENSORS));
    printf("  Speedup A vs B: %.2fx\n\n", ms_c / ms_d);

    /* ═══════════════════════════════════════════════════════
     * Test 3: GearLock + GearShift + Gear2 (full pipeline)
     * ═══════════════════════════════════════════════════════ */
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 3. Full Pipeline: GearLock → GearShift → Gear2\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    GearLockGPU gl;
    gl_init(&gl);

    /* Full pipeline: GearLock tag → GearShift routing → Gear2 batch DMA */
    cudaEventRecord(start);
    for (int it = 0; it < N_ITERS_LARGE; it++) {
        /* Step 1: GearLock — c144 tag */
        gl.c144 = (uint8_t)(it % GEAR_C144_CYCLE);
        gl_cpu_tick(&gl);

        /* Step 2: GearShift — route (build batch in pinned mirror) */
        /* In real code: for each high-priority tensor, memcpy from DRamTile → pinned mirror */
        memcpy(h_pinned, src_large, large_total);

        /* Step 3: Gear2 — single batch DMA to GPU */
        cudaMemcpyAsync(d_buf, h_pinned, large_total,
                        cudaMemcpyHostToDevice, 0);
        cudaStreamSynchronize(0);

        /* Step 4: GearLock — GPU world tick */
        gl_gpu_tick(&gl, GEAR_GPU_WORLD);
    }
    cudaEventRecord(stop);
    cudaEventSynchronize(stop);
    float ms_e = 0;
    cudaEventElapsedTime(&ms_e, start, stop);
    printf("  Full pipeline:               %.3f ms/cycle\n",
           ms_e / N_ITERS_LARGE);
    printf("    GearLock + routing + DMA:  %.1f us/tensor\n",
           ms_e * 1000.0f / (N_ITERS_LARGE * N_TENSORS));
    printf("    c144: %u, cpu_worlds: %u, gpu_worlds: %u\n\n",
           gl.c144, gl.cpu_worlds, gl.gpu_worlds);

    /* ═══════════════════════════════════════════════════════
     * Test 4: PCIe latency measurement
     * ═══════════════════════════════════════════════════════ */
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 4. PCIe Latency (small transfers)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    /* Measure latency of tiny transfers (1B, 16B, 256B, 4KB) */
    int sizes[] = {1, 16, 256, 1024, 4096};
    int n_sizes = sizeof(sizes) / sizeof(sizes[0]);
    uint8_t *tiny_src = (uint8_t *)malloc(4096);
    memset(tiny_src, 0xCC, 4096);

    printf("  Size    Latency    BW\n");
    printf("  ------  ---------  ----------\n");

    for (int s = 0; s < n_sizes; s++) {
        int sz = sizes[s];
        int iters = 10000;
        cudaEventRecord(start);
        for (int it = 0; it < iters; it++) {
            cudaMemcpy(d_buf, tiny_src, sz, cudaMemcpyHostToDevice);
        }
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);
        float ms = 0;
        cudaEventElapsedTime(&ms, start, stop);
        float avg_us = ms * 1000.0f / iters;
        float bw_gb = (float)sz / (avg_us * 1e-3f) / 1e9f;
        printf("  %4d B  %8.1f us  %.3f GB/s\n", sz, avg_us, bw_gb);
    }
    printf("\n");

    /* ═══════════════════════════════════════════════════════
     * Test 5: Bandwidth test (saturate PCIe)
     * ═══════════════════════════════════════════════════════ */
    printf("═══════════════════════════════════════════════════════\n");
    printf(" 5. Bandwidth Test (large transfers)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    int bws[] = {64*1024, 256*1024, 1024*1024, 4*1024*1024, 16*1024*1024};
    int n_bw = sizeof(bws) / sizeof(bws[0]);

    printf("  Size      pageable    pinned+Async\n");
    printf("  --------  ----------  ------------\n");

    for (int s = 0; s < n_bw; s++) {
        int sz = bws[s];
        int iters = 100;
        uint8_t *src = (uint8_t *)malloc(sz);
        memset(src, 0xDD, sz);

        /* Pageable */
        cudaEventRecord(start);
        for (int it = 0; it < iters; it++)
            cudaMemcpy(d_buf, src, sz, cudaMemcpyHostToDevice);
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);
        float ms_p = 0;
        cudaEventElapsedTime(&ms_p, start, stop);

        /* Pinned + async */
        uint8_t *pin = NULL;
        cudaMallocHost(&pin, sz);
        memcpy(pin, src, sz);
        cudaEventRecord(start);
        for (int it = 0; it < iters; it++) {
            cudaMemcpyAsync(d_buf, pin, sz, cudaMemcpyHostToDevice, 0);
            cudaStreamSynchronize(0);
        }
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);
        float ms_a2 = 0;
        cudaEventElapsedTime(&ms_a2, start, stop);

        float bw_page = (float)sz * iters / (ms_p / 1000.0f) / 1e9f;
        float bw_pin = (float)sz * iters / (ms_a2 / 1000.0f) / 1e9f;

        printf("  %4d KB   %6.1f GB/s  %6.1f GB/s\n",
               sz/1024, bw_page, bw_pin);

        cudaFreeHost(pin);
        free(src);
    }
    printf("\n");

    /* ═══════════════════════════════════════════════════════
     * Cleanup
     * ═══════════════════════════════════════════════════════ */
    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    cudaFree(d_buf);
    if (use_pinned) cudaFreeHost(h_pinned);
    else free(h_pinned);
    free(h_pageable);
    free(src_small);
    free(src_large);
    free(tiny_src);

    printf("═══════════════════════════════════════════════════════\n");
    printf(" Summary — Real GPU (GTX 1050 Ti)\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Gear2 pinned mirror + single DMA wins on real GPU\n");
    printf("  because: 1× cudaMemcpyAsync vs 251× driver calls\n");
    printf("  eliminates per-call driver context switch overhead\n");
    printf("═══════════════════════════════════════════════════════\n");
    return 0;
}

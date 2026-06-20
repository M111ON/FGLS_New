/*
 * bench_stream_twin.cu — Twin Geometry Stream Benchmark
 * Modes: CPU-only | GPU-only | CPU+GPU overlap
 *
 * Colab build:
 *   !nvcc -O2 -o bench bench_stream_twin.cu && ./bench
 *
 * Requires: stream_window.h, gear_lock.h in same directory
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <cuda_runtime.h>

#include "stream_window.h"

/* ── Config ─────────────────────────────────────────────────────── */
#define VRAM_BUDGET_MB   512u
#define N_TENSORS        128u
#define TENSOR_KB        2048u          /* fixed size per tensor     */
#define C144_INTERVAL    144u

#define TENSOR_BYTES     ((uint64_t)TENSOR_KB * 1024)
#define TOTAL_BYTES      ((uint64_t)N_TENSORS * TENSOR_BYTES)

/* ── CUDA error check ───────────────────────────────────────────── */
#define CUDA_CHECK(x) do { \
    cudaError_t e = (x); \
    if (e != cudaSuccess) { \
        fprintf(stderr, "CUDA error %s:%d — %s\n", \
                __FILE__, __LINE__, cudaGetErrorString(e)); \
        exit(1); \
    } \
} while(0)

/* ── GPU kernel: XOR reduction (simulate weight consume) ────────── */
__global__ void gpu_checksum(const uint8_t *buf, uint64_t sz, uint64_t *out)
{
    __shared__ uint64_t sdata[256];
    uint64_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    uint64_t stride = gridDim.x * blockDim.x;
    uint64_t acc = 0;

    for (uint64_t i = tid; i < sz; i += stride)
        acc ^= buf[i];

    sdata[threadIdx.x] = acc;
    __syncthreads();

    /* reduce within block */
    for (int s = blockDim.x/2; s > 0; s >>= 1) {
        if ((int)threadIdx.x < s)
            sdata[threadIdx.x] ^= sdata[threadIdx.x + s];
        __syncthreads();
    }
    if (threadIdx.x == 0)
        atomicXor((unsigned long long *)out,
                  (unsigned long long)sdata[0]);
}

/* ── Timer helpers ──────────────────────────────────────────────── */
static inline double cpu_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static float cuda_elapsed(cudaEvent_t a, cudaEvent_t b) {
    float ms = 0;
    cudaEventElapsedTime(&ms, a, b);
    return ms;
}

/* ── Fake source data (host) ────────────────────────────────────── */
static uint8_t *h_src = NULL;   /* pinned host memory, fake tensors */

static void src_init(void) {
    CUDA_CHECK(cudaMallocHost(&h_src, TOTAL_BYTES));
    for (uint64_t i = 0; i < TOTAL_BYTES; i++)
        h_src[i] = (uint8_t)(i ^ (i >> 8) ^ (i >> 16));
}

/* ══════════════════════════════════════════════════════════════════
 * MODE A — CPU only
 * memcpy + XOR checksum entirely on CPU
 * ══════════════════════════════════════════════════════════════════ */
static void bench_cpu(void) {
    printf("\n── MODE A: CPU only ──\n");

    uint8_t *buf = (uint8_t *)malloc(TENSOR_BYTES);
    uint64_t checksum = 0;

    double t0 = cpu_now();
    for (uint32_t i = 0; i < N_TENSORS; i++) {
        memcpy(buf, h_src + (uint64_t)i * TENSOR_BYTES, TENSOR_BYTES);
        for (uint64_t j = 0; j < TENSOR_BYTES; j += 64)
            checksum ^= buf[j];
    }
    double elapsed = cpu_now() - t0;

    printf("  elapsed   : %.4f s\n", elapsed);
    printf("  throughput: %.2f MB/s\n",
           (double)TOTAL_BYTES / (1024*1024) / elapsed);
    printf("  checksum  : 0x%016llx\n", (unsigned long long)checksum);
    free(buf);
}

/* ══════════════════════════════════════════════════════════════════
 * MODE B — GPU only
 * cudaMemcpy H→D then GPU kernel per tensor (no overlap)
 * ══════════════════════════════════════════════════════════════════ */
static void bench_gpu(void) {
    printf("\n── MODE B: GPU only ──\n");

    uint8_t  *d_buf;
    uint64_t *d_out, h_out = 0;
    CUDA_CHECK(cudaMalloc(&d_buf, TENSOR_BYTES));
    CUDA_CHECK(cudaMalloc(&d_out, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(d_out, 0, sizeof(uint64_t)));

    cudaEvent_t ev0, ev1;
    cudaEventCreate(&ev0); cudaEventCreate(&ev1);

    cudaEventRecord(ev0);
    for (uint32_t i = 0; i < N_TENSORS; i++) {
        CUDA_CHECK(cudaMemcpy(d_buf,
                              h_src + (uint64_t)i * TENSOR_BYTES,
                              TENSOR_BYTES,
                              cudaMemcpyHostToDevice));
        gpu_checksum<<<256, 256>>>(d_buf, TENSOR_BYTES, d_out);
    }
    cudaEventRecord(ev1);
    CUDA_CHECK(cudaDeviceSynchronize());

    float ms = cuda_elapsed(ev0, ev1);
    CUDA_CHECK(cudaMemcpy(&h_out, d_out, sizeof(uint64_t),
                           cudaMemcpyDeviceToHost));

    printf("  elapsed   : %.4f s\n", ms / 1000.0f);
    printf("  throughput: %.2f MB/s\n",
           (double)TOTAL_BYTES / (1024*1024) / (ms/1000.0f));
    printf("  checksum  : 0x%016llx\n", (unsigned long long)h_out);

    cudaFree(d_buf); cudaFree(d_out);
    cudaEventDestroy(ev0); cudaEventDestroy(ev1);
}

/* ══════════════════════════════════════════════════════════════════
 * MODE C — CPU + GPU twin (stream_window overlap)
 * CPU prefetches slot[i+1] while GPU consumes slot[i]
 * Gear boundary flush every C144_INTERVAL ops
 * ══════════════════════════════════════════════════════════════════ */
static void bench_twin(void) {
    printf("\n── MODE C: CPU+GPU twin (stream_window) ──\n");

    /* Pinned window — faster H→D transfer */
    uint64_t win_bytes = (uint64_t)VRAM_BUDGET_MB * 1024 * 1024;
    uint8_t *h_win;
    CUDA_CHECK(cudaMallocHost(&h_win, win_bytes));

    uint8_t  *d_win;
    uint64_t *d_out, h_out = 0;
    CUDA_CHECK(cudaMalloc(&d_win, TENSOR_BYTES * 2)); /* double buffer */
    CUDA_CHECK(cudaMalloc(&d_out, sizeof(uint64_t)));
    CUDA_CHECK(cudaMemset(d_out, 0, sizeof(uint64_t)));

    GearLock gear;
    memset(&gear, 0, sizeof(gear));
    static uint8_t c144_ctr = 0;
    gear.c144_ref = &c144_ctr;

    StreamWindow sw;
    sw_init(&sw, h_win, win_bytes, &gear);

    /* CUDA streams: s0=GPU consume, s1=CPU→GPU transfer */
    cudaStream_t s0, s1;
    cudaStreamCreate(&s0); cudaStreamCreate(&s1);

    cudaEvent_t ev0, ev1;
    cudaEventCreate(&ev0); cudaEventCreate(&ev1);

    /* Double-buffer indices */
    uint8_t *d_slot[2] = { d_win, d_win + TENSOR_BYTES };

    cudaEventRecord(ev0, s0);

    for (uint32_t i = 0; i < N_TENSORS; i++) {
        uint32_t db = i & 1;   /* double buffer index */

        /* CPU: request + "fread" into window slot */
        int slot = sw_request(&sw,
                              "tensor", (uint64_t)i * TENSOR_BYTES,
                              TENSOR_BYTES, 0xD0000000u | (i*60u),
                              SW_FMT_PWC);
        if (slot < 0) {
            sw_flush(&sw);
            c144_ctr = (uint8_t)((c144_ctr+1) % 144);
            slot = sw_request(&sw, "tensor", (uint64_t)i * TENSOR_BYTES,
                              TENSOR_BYTES, 0xD0000000u|(i*60u), SW_FMT_PWC);
        }

        /* CPU: memcpy from fake source into pinned slot */
        memcpy(sw.slots[slot].buf,
               h_src + (uint64_t)i * TENSOR_BYTES,
               TENSOR_BYTES);
        sw_mark_ready(&sw, slot, TENSOR_BYTES);

        /* Async H→D on stream s1 (overlap with GPU work on s0) */
        uint64_t gpu_sz = 0;
        uint8_t *ptr = sw_consume(&sw, slot, &gpu_sz);
        if (ptr) {
            CUDA_CHECK(cudaMemcpyAsync(d_slot[db], ptr, gpu_sz,
                                       cudaMemcpyHostToDevice, s1));
            /* Sync s1 into s0 then launch kernel */
            cudaStreamSynchronize(s1);
            gpu_checksum<<<256, 256, 0, s0>>>(d_slot[db], gpu_sz, d_out);
        }
        sw_release(&sw, slot);

        /* Gear boundary */
        if ((i+1) % C144_INTERVAL == 0) {
            cudaStreamSynchronize(s0);
            sw_flush(&sw);
            c144_ctr = (uint8_t)((c144_ctr+1) % 144);
        }
    }

    cudaEventRecord(ev1, s0);
    CUDA_CHECK(cudaDeviceSynchronize());

    float ms = cuda_elapsed(ev0, ev1);
    CUDA_CHECK(cudaMemcpy(&h_out, d_out, sizeof(uint64_t),
                           cudaMemcpyDeviceToHost));

    printf("  elapsed   : %.4f s\n", ms / 1000.0f);
    printf("  throughput: %.2f MB/s\n",
           (double)TOTAL_BYTES / (1024*1024) / (ms/1000.0f));
    printf("  checksum  : 0x%016llx\n", (unsigned long long)h_out);
    printf("\n");
    sw_print_stats(&sw, stdout);
    printf("\n=== GearLock ===\n");
    printf("  cpu_ops=%u gpu_ops=%u worlds: cpu=%u gpu=%u\n",
           gear.cpu_ops, gear.gpu_ops,
           gear.cpu_worlds, gear.gpu_worlds);

    cudaFree(d_win); cudaFree(d_out);
    cudaFreeHost(h_win);
    cudaStreamDestroy(s0); cudaStreamDestroy(s1);
    cudaEventDestroy(ev0); cudaEventDestroy(ev1);
}

/* ── Main ───────────────────────────────────────────────────────── */
int main(void) {
    /* Device info */
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("=== bench_stream_twin ===\n");
    printf("  GPU       : %s\n", prop.name);
    printf("  VRAM      : %.0f MB\n", prop.totalGlobalMem / (1024.0*1024));
    printf("  Tensors   : %u × %u KB = %.1f MB total\n",
           N_TENSORS, TENSOR_KB,
           (double)TOTAL_BYTES / (1024*1024));

    src_init();

    bench_cpu();
    bench_gpu();
    bench_twin();

    cudaFreeHost(h_src);
    printf("\nDone.\n");
    return 0;
}

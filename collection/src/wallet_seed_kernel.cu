/*
 * wallet_seed_kernel.cu — GPU-accelerated wallet_chunk_seed + wallet_xorfold64
 *
 * Compile:
 *   nvcc -O3 -shared -o wallet_seed.dll wallet_seed_kernel.cu -Xcompiler -fPIC
 *
 * Usage from Python via ctypes:
 *   dll = ctypes.CDLL('./wallet_seed.dll')
 *   dll.wallet_seed_batch(d_in, d_seeds, n)
 *   dll.wallet_xorfold_batch(d_in, d_checksums, n)
 */

#include <stdint.h>
#include <cuda_runtime.h>

#define CHUNK_SZ 64
#define MASK64   0xFFFFFFFFFFFFFFFFULL

__device__ __forceinline__ uint64_t splitmix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

__global__ void wallet_seed_kernel(
    const uint8_t *__restrict__ chunks,   // n * 64
    uint64_t      *__restrict__ seeds,     // n
    int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n) return;

    const uint8_t *c = chunks + (size_t)idx * CHUNK_SZ;

    /* XOR-fold 64 bytes → 8 uint64 words → XOR reduce */
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

    seeds[idx] = splitmix64(acc);
}

__global__ void wallet_xorfold_kernel(
    const uint8_t *__restrict__ chunks,    // n * 64
    uint32_t      *__restrict__ checksums, // n
    int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n) return;

    const uint8_t *c = chunks + (size_t)idx * CHUNK_SZ;

    uint32_t acc = 0;
    #pragma unroll
    for (int i = 0; i < 16; i++) {
        uint32_t w = 0;
        #pragma unroll
        for (int j = 0; j < 4; j++) {
            w |= (uint32_t)c[i * 4 + j] << (j * 8);
        }
        acc ^= w;
    }

    checksums[idx] = acc;
}

/* ── C API for Python ctypes ── */

extern "C" {

int wallet_seed_batch(
    const uint8_t *h_chunks,
    uint64_t      *h_seeds,
    int n)
{
    uint8_t *d_chunks;
    uint64_t *d_seeds;

    size_t in_bytes  = (size_t)n * CHUNK_SZ;
    size_t out_bytes = (size_t)n * sizeof(uint64_t);

    cudaMalloc(&d_chunks, in_bytes);
    cudaMalloc(&d_seeds, out_bytes);

    cudaMemcpy(d_chunks, h_chunks, in_bytes, cudaMemcpyHostToDevice);

    int block = 256;
    int grid = (n + block - 1) / block;
    wallet_seed_kernel<<<grid, block>>>(d_chunks, d_seeds, n);

    cudaMemcpy(h_seeds, d_seeds, out_bytes, cudaMemcpyDeviceToHost);

    cudaFree(d_chunks);
    cudaFree(d_seeds);

    return cudaGetLastError() == cudaSuccess ? 0 : -1;
}

int wallet_xorfold_batch(
    const uint8_t *h_chunks,
    uint32_t      *h_checksums,
    int n)
{
    uint8_t  *d_chunks;
    uint32_t *d_checksums;

    size_t in_bytes  = (size_t)n * CHUNK_SZ;
    size_t out_bytes = (size_t)n * sizeof(uint32_t);

    cudaMalloc(&d_chunks, in_bytes);
    cudaMalloc(&d_checksums, out_bytes);

    cudaMemcpy(d_chunks, h_chunks, in_bytes, cudaMemcpyHostToDevice);

    int block = 256;
    int grid = (n + block - 1) / block;
    wallet_xorfold_kernel<<<grid, block>>>(d_chunks, d_checksums, n);

    cudaMemcpy(h_checksums, d_checksums, out_bytes, cudaMemcpyDeviceToHost);

    cudaFree(d_chunks);
    cudaFree(d_checksums);

    return cudaGetLastError() == cudaSuccess ? 0 : -1;
}

/* Combined: seed + checksum in one pass */
int wallet_batch(
    const uint8_t *h_chunks,
    uint64_t      *h_seeds,
    uint32_t      *h_checksums,
    int n)
{
    uint8_t  *d_chunks;
    uint64_t *d_seeds;
    uint32_t *d_checksums;

    size_t in_bytes  = (size_t)n * CHUNK_SZ;

    cudaMalloc(&d_chunks, in_bytes);
    cudaMalloc(&d_seeds, (size_t)n * sizeof(uint64_t));
    cudaMalloc(&d_checksums, (size_t)n * sizeof(uint32_t));

    cudaMemcpy(d_chunks, h_chunks, in_bytes, cudaMemcpyHostToDevice);

    int block = 256;
    int grid = (n + block - 1) / block;
    wallet_seed_kernel<<<grid, block>>>(d_chunks, d_seeds, n);
    wallet_xorfold_kernel<<<grid, block>>>(d_chunks, d_checksums, n);

    cudaMemcpy(h_seeds, d_seeds, (size_t)n * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_checksums, d_checksums, (size_t)n * sizeof(uint32_t), cudaMemcpyDeviceToHost);

    cudaFree(d_chunks);
    cudaFree(d_seeds);
    cudaFree(d_checksums);

    return cudaGetLastError() == cudaSuccess ? 0 : -1;
}

} /* extern "C" */

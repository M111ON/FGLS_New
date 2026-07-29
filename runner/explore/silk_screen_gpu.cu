// silk_screen_gpu.cu — CUDA Silk Screen Encoder/Decoder
// Compile: nvcc -O2 -std=c++14 -o silk_screen_gpu silk_screen_gpu.cu
// Run:     ./silk_screen_gpu [model.gguf]
//
// Maps silk screen identity filter to GPU scatter/gather:
//   encode: data[idx] → silk[idx%10][idx/10%6][idx/60%1440]  (scatter)
//   decode: silk[...] → output[idx]                             (gather)
//
// Architecture:
//   Thread block = 256 threads
//   Grid = ceil(LAYER_SLOTS / 256) blocks
//   Each thread handles 1 weight index

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <cuda_runtime.h>

#define N_BOXES     10
#define N_DIRS      6
#define CLOCK_MAX   1440
#define LAYER_SLOTS (N_BOXES * N_DIRS * CLOCK_MAX)  // 86400
#define GGUF_MAGIC  0x46554747u
#define MAX_TENSORS 256
#define BLOCK_SIZE  256

// ── GGUF Reader (CPU) ───────────────────────────────────────
typedef struct {
    uint64_t n_tensors;
    char     *names[MAX_TENSORS];
    uint32_t dtypes[MAX_TENSORS];
    uint64_t offsets[MAX_TENSORS];
    uint64_t sizes[MAX_TENSORS];
} GGUFFile;

static int gguf_open(const char *path, GGUFFile *gf) {
    memset(gf, 0, sizeof(GGUFFile));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t magic, version; uint64_t n_tensors, n_kv;
    fread(&magic, 4, 1, f); fread(&version, 4, 1, f);
    fread(&n_tensors, 8, 1, f); fread(&n_kv, 8, 1, f);
    if (magic != GGUF_MAGIC) { fclose(f); return -1; }
    gf->n_tensors = n_tensors < MAX_TENSORS ? n_tensors : MAX_TENSORS;
    for (uint64_t i = 0; i < n_kv; i++) {
        uint64_t klen; fread(&klen, 8, 1, f); fseek(f, klen, SEEK_CUR);
        uint32_t vt; fread(&vt, 4, 1, f);
        switch(vt) {
            case 0: case 1: fseek(f,1,SEEK_CUR); break;
            case 2: case 3: fseek(f,2,SEEK_CUR); break;
            case 4: case 5: case 6: fseek(f,4,SEEK_CUR); break;
            case 7: fseek(f,1,SEEK_CUR); break;
            case 8: { uint64_t s; fread(&s,8,1,f); fseek(f,s,SEEK_CUR); break; }
            case 9: { uint32_t at; uint64_t al; fread(&at,4,1,f); fread(&al,8,1,f);
                if(at==8) for(uint64_t j=0;j<al;j++){uint64_t s;fread(&s,8,1,f);fseek(f,s,SEEK_CUR);}
                else fseek(f,al*(at<=3?2:at<=6?4:1),SEEK_CUR); break; }
            default: fseek(f,4,SEEK_CUR); break;
        }
    }
    for (uint64_t i = 0; i < gf->n_tensors; i++) {
        uint64_t nlen; fread(&nlen, 8, 1, f);
        gf->names[i] = (char*)calloc(nlen+1,1);
        fread(gf->names[i], nlen, 1, f);
        uint32_t nd; fread(&nd, 4, 1, f);
        for (uint32_t j=0;j<nd;j++){uint64_t d;fread(&d,8,1,f);}
        fread(&gf->dtypes[i], 4, 1, f);
        fread(&gf->offsets[i], 8, 1, f);
    }
    for (uint64_t i = 0; i < gf->n_tensors; i++) {
        if (i+1 < gf->n_tensors) gf->sizes[i] = gf->offsets[i+1]-gf->offsets[i];
        else { long c=ftell(f); fseek(f,0,SEEK_END); gf->sizes[i]=ftell(f)-gf->offsets[i]; fseek(f,c,SEEK_SET); }
    }
    fclose(f); return 0;
}

static void gguf_close(GGUFFile *gf) {
    for (uint64_t i = 0; i < gf->n_tensors; i++) free(gf->names[i]);
}

// ── GPU Kernels ─────────────────────────────────────────────

// Encode: scatter data into silk screen
// Each thread: one weight → one (box, dir, tick) slot
__global__ void silk_encode_kernel(
    const int8_t *__restrict__ data,
    int8_t *__restrict__ silk,  // [N_BOXES][N_DIRS][CLOCK_MAX]
    int n
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n) return;

    int box  = idx % N_BOXES;
    int dir  = (idx / N_BOXES) % N_DIRS;
    int tick = (idx / (N_BOXES * N_DIRS)) % CLOCK_MAX;

    // Linear index into flat silk array
    int silk_idx = box * (N_DIRS * CLOCK_MAX) + dir * CLOCK_MAX + tick;
    silk[silk_idx] = data[idx];
}

// Decode: gather from silk screen
__global__ void silk_decode_kernel(
    const int8_t *__restrict__ silk,
    int8_t *__restrict__ out,
    int n
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n) return;

    int box  = idx % N_BOXES;
    int dir  = (idx / N_BOXES) % N_DIRS;
    int tick = (idx / (N_BOXES * N_DIRS)) % CLOCK_MAX;

    int silk_idx = box * (N_DIRS * CLOCK_MAX) + dir * CLOCK_MAX + tick;
    out[idx] = silk[silk_idx];
}

// Verify kernel: count exact matches
__global__ void silk_verify_kernel(
    const int8_t *__restrict__ a,
    const int8_t *__restrict__ b,
    int *__restrict__ exact_count,
    int *__restrict__ worst_err,
    int n
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n) return;

    int err = abs((int)a[idx] - (int)b[idx]);
    if (err == 0) atomicAdd(exact_count, 1);
    // Simple atomic max for worst error
    int old = atomicMax(worst_err, err);
    (void)old;
}

// ── GPU Timer helper ────────────────────────────────────────
static double gpu_ms(cudaEvent_t start, cudaEvent_t stop) {
    float ms;
    cudaEventElapsedTime(&ms, start, stop);
    return (double)ms;
}

// ── Main ────────────────────────────────────────────────────
int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "/i/model/Qwen3-0.6B-Q8_0.gguf";

    printf("============================================================\n");
    printf("  Silk Screen GPU Benchmark — CUDA scatter/gather\n");
    printf("  Layer: %d slots = %d × %d × %d\n", LAYER_SLOTS, N_BOXES, N_DIRS, CLOCK_MAX);
    printf("============================================================\n\n");

    // Device info
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("  GPU: %s\n", prop.name);
    printf("  SM count: %d\n", prop.multiProcessorCount);
    printf("  Global mem: %.0f MB\n", prop.totalGlobalMem / 1e6);
    printf("  Clock: %.0f MHz\n", prop.clockRate / 1000.0);
    printf("  Mem BW: %.1f GB/s\n\n",
           2.0 * prop.memoryClockRate * (prop.memoryBusWidth / 8) / 1e9);

    // Open GGUF
    GGUFFile gf;
    if (gguf_open(model, &gf) < 0) { printf("FAIL: cannot open GGUF\n"); return 1; }
    printf("  Model: %s\n", model);
    printf("  Tensors: %llu\n", (unsigned long long)gf.n_tensors);

    int best = -1; uint64_t best_sz = 0;
    for (uint64_t i = 0; i < gf.n_tensors; i++) {
        if (gf.dtypes[i] != 8) continue;
        if (gf.sizes[i] > best_sz) { best = (int)i; best_sz = gf.sizes[i]; }
    }
    printf("  Tensor[%d]: %s (%llu bytes)\n\n", best, gf.names[best], (unsigned long long)gf.sizes[best]);

    // Extract int8 from GGUF
    int8_t *h_buf = (int8_t*)calloc(LAYER_SLOTS + 256, 1);
    FILE *f = fopen(model, "rb");
    if (!f) { printf("FAIL: reopen\n"); return 1; }
    fseek(f, gf.offsets[best], SEEK_SET);
    int nr = 0;
    uint64_t nb = gf.sizes[best] / 34;
    for (uint64_t b = 0; b < nb && nr < LAYER_SLOTS; b++) {
        fseek(f, 2, SEEK_CUR);
        for (int j = 0; j < 32 && nr < LAYER_SLOTS; j++) { fread(&h_buf[nr], 1, 1, f); nr++; }
    }
    fclose(f);
    printf("  Extracted: %d int8 values\n\n", nr);

    // ── GPU allocation ───────────────────────────────────────
    int8_t *d_data, *d_silk, *d_decoded;
    int *d_exact, *d_worst;
    cudaMalloc(&d_data, LAYER_SLOTS);
    cudaMalloc(&d_silk, LAYER_SLOTS);  // same size (identity)
    cudaMalloc(&d_decoded, LAYER_SLOTS);
    cudaMalloc(&d_exact, sizeof(int));
    cudaMalloc(&d_worst, sizeof(int));

    // Copy data to GPU
    cudaMemcpy(d_data, h_buf, LAYER_SLOTS, cudaMemcpyHostToDevice);

    int grid = (LAYER_SLOTS + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // ── Warm up ──────────────────────────────────────────────
    for (int i = 0; i < 10; i++) {
        silk_encode_kernel<<<grid, BLOCK_SIZE>>>(d_data, d_silk, LAYER_SLOTS);
        silk_decode_kernel<<<grid, BLOCK_SIZE>>>(d_silk, d_decoded, LAYER_SLOTS);
    }
    cudaDeviceSynchronize();

    // ── Benchmark: GPU Encode ────────────────────────────────
    int N_ITERS = 10000;
    cudaEvent_t t_start, t_stop;
    cudaEventCreate(&t_start);
    cudaEventCreate(&t_stop);

    cudaEventRecord(t_start);
    for (int i = 0; i < N_ITERS; i++) {
        silk_encode_kernel<<<grid, BLOCK_SIZE>>>(d_data, d_silk, LAYER_SLOTS);
    }
    cudaEventRecord(t_stop);
    cudaDeviceSynchronize();
    double enc_ms = gpu_ms(t_start, t_stop);

    // ── Benchmark: GPU Decode ────────────────────────────────
    cudaEventRecord(t_start);
    for (int i = 0; i < N_ITERS; i++) {
        silk_decode_kernel<<<grid, BLOCK_SIZE>>>(d_silk, d_decoded, LAYER_SLOTS);
    }
    cudaEventRecord(t_stop);
    cudaDeviceSynchronize();
    double dec_ms = gpu_ms(t_start, t_stop);

    // ── Verify ───────────────────────────────────────────────
    int zero = 0;
    cudaMemcpy(d_exact, &zero, sizeof(int), cudaMemcpyHostToDevice);
    cudaMemcpy(d_worst, &zero, sizeof(int), cudaMemcpyHostToDevice);

    silk_encode_kernel<<<grid, BLOCK_SIZE>>>(d_data, d_silk, LAYER_SLOTS);
    silk_decode_kernel<<<grid, BLOCK_SIZE>>>(d_silk, d_decoded, LAYER_SLOTS);
    silk_verify_kernel<<<grid, BLOCK_SIZE>>>(d_data, d_decoded, d_exact, d_worst, LAYER_SLOTS);
    cudaDeviceSynchronize();

    int h_exact, h_worst;
    cudaMemcpy(&h_exact, d_exact, sizeof(int), cudaMemcpyDeviceToHost);
    cudaMemcpy(&h_worst, d_worst, sizeof(int), cudaMemcpyDeviceToHost);

    // ── Results ──────────────────────────────────────────────
    double tw = (double)N_ITERS * LAYER_SLOTS;

    printf("=== GPU ENCODE (scatter) ===\n");
    printf("  Iterations: %d\n", N_ITERS);
    printf("  Total time: %.1f ms\n", enc_ms);
    printf("  Per layer:  %.3f ms\n", enc_ms / N_ITERS);
    printf("  Throughput: %.1f M weights/sec\n", tw / enc_ms / 1000.0);

    printf("\n=== GPU DECODE (gather) ===\n");
    printf("  Iterations: %d\n", N_ITERS);
    printf("  Total time: %.1f ms\n", dec_ms);
    printf("  Per layer:  %.3f ms\n", dec_ms / N_ITERS);
    printf("  Throughput: %.1f M weights/sec\n", tw / dec_ms / 1000.0);

    printf("\n=== LOSSLESS ===\n");
    printf("  Exact: %d/%d (%.1f%%)\n", h_exact, LAYER_SLOTS, 100.0*h_exact/LAYER_SLOTS);
    printf("  Worst: %d\n", h_worst);
    printf("  Status: %s\n", h_worst == 0 ? "✓ LOSSLESS" : "✗ LOSSY");

    // ── CPU comparison (rough) ───────────────────────────────
    printf("\n=== CPU vs GPU ===\n");
    printf("  GPU encode: %.3f ms/layer\n", enc_ms / N_ITERS);
    printf("  GPU decode: %.3f ms/layer\n", dec_ms / N_ITERS);
    printf("  (CPU encode ~0.24 ms/layer on i7 — see bench_silk_throughput)\n");

    // ── Cleanup ──────────────────────────────────────────────
    cudaFree(d_data); cudaFree(d_silk); cudaFree(d_decoded);
    cudaFree(d_exact); cudaFree(d_worst);
    cudaEventDestroy(t_start); cudaEventDestroy(t_stop);
    free(h_buf);
    gguf_close(&gf);
    return 0;
}

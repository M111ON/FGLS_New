#!/usr/bin/env python3
"""Step 2: Write, compile, and run GPU Jet Puller benchmark on real tensor data."""
import os, sys, subprocess, time

RAW = "/content/tensor_data.bin"

def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

# Write CUDA source
CUDA_SRC = """
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define TPB 256
#define MAX_PULLS 100000
#define CUDA_CHECK(call) do { cudaError_t e = (call); if (e != cudaSuccess) { fprintf(stderr,"CUDA ERR: %s\\n",cudaGetErrorString(e)); return -1; } } while(0)

__global__ void pull_kernel(const uint8_t *base, const uint32_t *off, uint32_t n, uint32_t sz, uint64_t *ts) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t s = gridDim.x * blockDim.x;
    for (; i < n; i += s) {
        const uint8_t *chunk = base + off[i];
        volatile uint32_t sum = 0;
        for (uint32_t v = 0; v < sz/16; v++) {
            uint4 vec = *((const uint4*)(chunk + v*16));
            sum += vec.x + vec.y + vec.z + vec.w;
        } (void)sum; ts[i] = clock64();
    }
}

__global__ void pull_xor_kernel(const uint8_t *base, const uint32_t *off, uint32_t n, uint32_t sz, uint64_t *ts, uint8_t *err, const uint32_t *ref) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t s = gridDim.x * blockDim.x;
    for (; i < n; i += s) {
        const uint8_t *chunk = base + off[i];
        uint32_t ck = 0;
        for (uint32_t v = 0; v < sz/16; v++) {
            uint4 vec = *((const uint4*)(chunk + v*16));
            ck ^= vec.x ^ vec.y ^ vec.z ^ vec.w;
        }
        uint8_t ck8 = ck ^ (ck>>8) ^ (ck>>16) ^ (ck>>24);
        err[i] = (ck8 != (uint8_t)(ref[i] & 0xFF)) ? 1 : 0;
        ts[i] = clock64();
    }
}

int main() {
    printf("=== REAL MODEL WEIGHT BENCHMARK ===\\n\\n");
    int nd; cudaGetDeviceCount(&nd);
    cudaDeviceProp p; cudaGetDeviceProperties(&p, 0);
    printf("GPU: %s\\n", p.name);
    
    FILE *fp = fopen("/content/tensor_data.bin", "rb");
    if (!fp) { fprintf(stderr,"ERROR: tensor_data.bin not found\\n"); return 1; }
    fseek(fp,0,SEEK_END); size_t fsz = ftell(fp); rewind(fp);
    uint8_t *h = (uint8_t*)malloc(fsz);
    fread(h,1,fsz,fp); fclose(fp);
    printf("Tensor data: %.1f MB\\n\\n", fsz/1e6);
    
    uint8_t *d; CUDA_CHECK(cudaMalloc(&d, fsz));
    CUDA_CHECK(cudaMemcpy(d, h, fsz, cudaMemcpyHostToDevice));
    
    uint32_t sizes[] = {64,256,1024};
    double results[3][2];
    double best = 0; int best_s=0, best_m=0;
    
    for (int s=0; s<3; s++) { for (int m=0; m<2; m++) {
        uint32_t sz = sizes[s];
        int use_xor = (m==0);
        uint32_t np = fsz/sz; if (np>MAX_PULLS) np=MAX_PULLS; if (np<1) np=1;
        uint32_t step = (fsz/sz)/np; if (step<1) step=1;
        
        uint32_t *ho = (uint32_t*)malloc(np*4);
        uint32_t *hr = (uint32_t*)malloc(np*4);
        for (uint32_t i=0; i<np; i++) {
            uint64_t o = ((uint64_t)i*step*sz) % fsz;
            ho[i] = (uint32_t)o;
            uint32_t ck=0; for (uint32_t b=0; b<sz && o+b<fsz; b++) ck ^= h[o+b];
            hr[i] = ck;
        }
        
        uint32_t *do_, *dr_; uint64_t *dt; uint8_t *de;
        CUDA_CHECK(cudaMalloc(&do_, np*4)); CUDA_CHECK(cudaMalloc(&dr_, np*4));
        CUDA_CHECK(cudaMalloc(&dt, np*8)); CUDA_CHECK(cudaMalloc(&de, np));
        CUDA_CHECK(cudaMemcpy(do_, ho, np*4, cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(dr_, hr, np*4, cudaMemcpyHostToDevice));
        
        uint32_t blk = (np+TPB-1)/TPB;
        cudaEvent_t st, sp; cudaEventCreate(&st); cudaEventCreate(&sp);
        cudaEventRecord(st);
        if (use_xor) pull_xor_kernel<<<blk,TPB>>>(d,do_,np,sz,dt,de,dr_);
        else pull_kernel<<<blk,TPB>>>(d,do_,np,sz,dt);
        cudaEventRecord(sp); cudaEventSynchronize(sp);
        float ms; cudaEventElapsedTime(&ms,st,sp);
        
        uint32_t errs=0;
        if (use_xor) {
            uint8_t *he = (uint8_t*)malloc(np);
            CUDA_CHECK(cudaMemcpy(he, de, np, cudaMemcpyDeviceToHost));
            for (uint32_t i=0; i<np; i++) if (he[i]) errs++;
            free(he);
        }
        
        double gb = (double)np*sz/1e9;
        double bw = (ms>0) ? gb/(ms/1000) : 0;
        printf("%s %s: pulls=%d err=%d time=%.1fms bw=%.1fGB/s\\n",
               sizes[s]==64?"64B":sizes[s]==256?"256B":"1024B",
               use_xor?"XOR":"PULL", np, errs, ms, bw);
        results[s][m]=bw;
        if (bw>best) { best=bw; best_s=s; best_m=m; }
        
        cudaEventDestroy(st); cudaEventDestroy(sp);
        CUDA_CHECK(cudaFree(do_)); CUDA_CHECK(cudaFree(dr_));
        CUDA_CHECK(cudaFree(dt)); CUDA_CHECK(cudaFree(de));
        free(ho); free(hr);
    }}
    
    printf("\\n--- BEST: %s %s at %.1f GB/s ---\\n",
           sizes[best_s]==64?"64B":sizes[best_s]==256?"256B":"1024B",
           best_m==0?"XOR":"PULL", best);
    printf("Total data: %.1f MB\\n", fsz/1e6);
    CUDA_CHECK(cudaFree(d)); free(h);
    printf("\\n=== BENCHMARK COMPLETE ===\\n");
    return 0;
}
"""

src_path = "/content/gpu_jet_puller_bench.cu"
bin_path = "/content/gpu_jet_puller_bench"

with open(src_path, "w") as f:
    f.write(CUDA_SRC)

log(f"CUDA source written ({len(CUDA_SRC)} B)")

# Check tensor data exists
if not os.path.isfile(RAW) or os.path.getsize(RAW) < 1000000:
    log(f"ERROR: {RAW} not found or too small")
    sys.exit(1)

sz_mb = os.path.getsize(RAW) / 1e6
log(f"Tensor data: {sz_mb:.1f} MB")

# Compile
log("Compiling...")
r = subprocess.run(["nvcc", "-O3", "-std=c++17", "-arch=sm_75", "-o", bin_path, src_path, "-lm"],
                  capture_output=True, text=True, timeout=120)
if r.returncode != 0:
    log(f"COMPILE FAILED: {r.stderr[:500]}")
    sys.exit(1)
log(f"Binary: {os.path.getsize(bin_path)/1e3:.0f} KB")

# Run
log("Running benchmark...")
os.chmod(bin_path, 0o755)
r = subprocess.run([bin_path], capture_output=True, text=True, timeout=300)
for line in r.stdout.split("\n"):
    if line.strip():
        print(f"  {line}", flush=True)
if r.stderr.strip():
    log(f"STDERR: {r.stderr[:300]}")
log(f"Exit: {r.returncode}")
log("DONE")
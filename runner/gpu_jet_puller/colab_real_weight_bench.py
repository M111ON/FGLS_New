#!/usr/bin/env python3
"""Extract real model tensor data, then run GPU Jet Puller bandwidth benchmark."""
import os, sys, subprocess, struct, json, time

LOG = "/content/bench_results.log"
MODEL = "/content/Qwen3-0.6B-Q4_0.gguf"

def log(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(LOG, "a") as f:
        f.write(line + "\n")

def extract_tensors():
    """Use Python to parse GGUF and write raw tensor data."""
    raw_path = "/content/tensor_data.bin"
    meta_path = "/content/tensor_meta.json"
    if os.path.isfile(raw_path) and os.path.getsize(raw_path) > 1e6:
        log(f"Tensor data exists ({os.path.getsize(raw_path)/1e6:.1f} MB)")
        return raw_path, meta_path
    
    log(f"Extracting tensors from {MODEL}...")
    
    # Use huggingface's gguf library
    try:
        from gguf import GGUFReader
        reader = GGUFReader(MODEL)
        tensors = reader.tensors
        log(f"GGUF: {len(tensors)} tensors")
        
        # Write concatenated tensor data
        meta = {"n_tensors": len(tensors), "total_bytes": 0, "tensors": []}
        with open(raw_path, "wb") as out:
            for i, t in enumerate(tensors):
                data = t.data.tobytes()
                out.write(data)
                meta["tensors"].append({
                    "name": t.name,
                                        "shape": [int(d) for d in t.shape],
                                        "n_bytes": int(len(data)),
                                        "offset_in_blob": int(meta["total_bytes"])
                })
                meta["total_bytes"] += len(data)
                if i < 5:
                    log(f"  [{i}] {t.name}: shape={list(t.shape)}, {len(data)} B")
        
        with open(meta_path, "w") as f:
            json.dump(meta, f)
        log(f"Extracted {meta['total_bytes']/1e6:.1f} MB tensor data")
        return raw_path, meta_path
    
    except ImportError:
        log("gguf library not available, using manual parser")
    
    # Manual GGUF parser as fallback
    with open(MODEL, "rb") as f:
        data = f.read()
    
    # Parse header
    import struct
    magic = struct.unpack("<I", data[0:4])[0]
    version = struct.unpack("<I", data[4:8])[0]
    n_tensors = struct.unpack("<Q", data[8:16])[0]
    n_metadata = struct.unpack("<Q", data[16:24])[0]
    log(f"GGUF v{version}: {n_tensors} tensors, {n_metadata} metadata entries")
    
    pos = 24
    # Skip metadata
    for i in range(n_metadata):
        key_len = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8
        pos += key_len  # skip key
        vtype = struct.unpack("<I", data[pos:pos+4])[0]; pos += 4
        if vtype == 8:  # string
            slen = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8 + slen
        elif vtype == 9:  # array
            atype = struct.unpack("<I", data[pos:pos+4])[0]; pos += 4
            alen = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8
            for j in range(alen):
                if atype == 8:  # string array elements
                    slen = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8 + slen
                else:
                    pos += 4
        elif vtype in (10, 11, 12):  # 64-bit
            pos += 8
        elif vtype in (18, 19):  # 16-bit
            pos += 2
        else:  # 32-bit and small
            pos += 4
    
    # Parse tensor info
    tensors = []
    for i in range(n_tensors):
        name_len = struct.unpack("<Q", data[pos:pos+8])[0]; pos += 8
        name = data[pos:pos+name_len].decode('utf-8', errors='replace'); pos += name_len
        n_dims = struct.unpack("<I", data[pos:pos+4])[0]; pos += 4
        dims = [struct.unpack("<Q", data[pos+d*8:pos+d*8+8])[0] for d in range(n_dims)]
        pos += n_dims * 8
        ggml_type = struct.unpack("<I", data[pos:pos+4])[0]; pos += 4
        tensors.append((name, n_dims, dims, ggml_type))
    
    # Read data offsets
    offsets = []
    for i in range(n_tensors):
        offsets.append(struct.unpack("<Q", data[pos:pos+8])[0]); pos += 8
    
    # Write concatenated tensor data
    meta = {"n_tensors": n_tensors, "total_bytes": 0, "tensors": []}
    with open(raw_path, "wb") as out:
        for i in range(n_tensors):
            name, n_dims, dims, ggml_type = tensors[i]
            if i + 1 < n_tensors:
                size = offsets[i+1] - offsets[i]
            else:
                size = len(data) - offsets[i]
            chunk = data[offsets[i]:offsets[i]+size]
            out.write(chunk)
            meta["tensors"].append({
                "name": name,
                "shape": dims,
                "n_bytes": len(chunk),
                "offset_in_blob": meta["total_bytes"]
            })
            meta["total_bytes"] += len(chunk)
            if i < 5:
                log(f"  [{i}] {name}: {len(chunk)} B, dims={dims}")
    
    with open(meta_path, "w") as f:
        json.dump(meta, f)
    log(f"Extracted {meta['total_bytes']/1e6:.1f} MB")
    return raw_path, meta_path

def compile_benchmark():
    """Write and compile a simple CUDA benchmark that reads tensor_data.bin."""
    binary = "/content/gpu_jet_puller_raw"
    src_path = "/content/gpu_jet_puller_gguf_raw.cu"
    
    if os.path.isfile(binary):
        log(f"Binary exists")
        return binary
    
    cuda_src = """#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define CUDA_CHECK(call) do { cudaError_t e = (call); if (e != cudaSuccess) { fprintf(stderr,"CUDA ERR %s:%d: %s\\n",__FILE__,__LINE__,cudaGetErrorString(e)); return -1; } } while(0)
#define TPB 256
#define MAX_PULLS 100000

__global__ void pull_kernel(const uint8_t *base, const uint32_t *offs, uint32_t n, uint32_t sz, uint64_t *ts) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t s = gridDim.x * blockDim.x;
    for (; i < n; i += s) {
        const uint8_t *chunk = base + offs[i];
        volatile uint32_t sum = 0;
        for (uint32_t v = 0; v < sz/16; v++) {
            uint4 vec = *((const uint4*)(chunk + v*16));
            sum += vec.x + vec.y + vec.z + vec.w;
        } (void)sum;
        ts[i] = clock64();
    }
}

__global__ void pull_xor_kernel(const uint8_t *base, const uint32_t *offs, uint32_t n, uint32_t sz, uint64_t *ts, uint8_t *errs, const uint32_t *refs) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t s = gridDim.x * blockDim.x;
    for (; i < n; i += s) {
        const uint8_t *chunk = base + offs[i];
        uint32_t cksum = 0;
        for (uint32_t v = 0; v < sz/16; v++) {
            uint4 vec = *((const uint4*)(chunk + v*16));
            cksum ^= vec.x ^ vec.y ^ vec.z ^ vec.w;
        }
        errs[i] = ((cksum ^ (cksum>>8) ^ (cksum>>16) ^ (cksum>>24)) != (uint8_t)(refs[i] & 0xFF)) ? 1 : 0;
        ts[i] = clock64();
    }
}

int main(int argc, char **argv) {
    const char *data_path = "/content/tensor_data.bin";
    uint32_t chunk_sizes[] = {64, 256, 1024};
    int n_sizes = 3;
    
    printf("╔══ GPU JET PULLER — REAL MODEL WEIGHT ══╗\\n\\n");
    
    int nd; cudaGetDeviceCount(&nd);
    cudaDeviceProp p; cudaGetDeviceProperties(&p, 0);
    printf("GPU: %s  VRAM: %zu MB\\n\\n", p.name, p.totalGlobalMem>>20);
    
    FILE *fp = fopen(data_path, "rb");
    if (!fp) { fprintf(stderr,"Can't open %s\\n", data_path); return 1; }
    fseek(fp, 0, SEEK_END);
    size_t fsize = ftell(fp); rewind(fp);
    uint8_t *h_data = (uint8_t*)malloc(fsize);
    fread(h_data, 1, fsize, fp); fclose(fp);
    printf("Loaded tensor data: %.1f MB\\n\\n", fsize/1e6);
    
    uint8_t *d_hbm;
    CUDA_CHECK(cudaMalloc(&d_hbm, fsize));
    CUDA_CHECK(cudaMemcpy(d_hbm, h_data, fsize, cudaMemcpyHostToDevice));
    
    double results[3][2];
    for (int s = 0; s < n_sizes; s++) {
        uint32_t sz = chunk_sizes[s];
        for (int m = 0; m < 2; m++) {
            int xor_mode = (m == 0);
            printf("── chunk=%uB, %s ──\\n", sz, xor_mode ? "with XOR" : "pure pull");
            
            uint32_t n_pull = fsize / sz;
            if (n_pull > MAX_PULLS) n_pull = MAX_PULLS;
            if (n_pull < 1) n_pull = 1;
            uint32_t step = (fsize / sz) / n_pull;
            if (step < 1) step = 1;
            
            uint32_t *h_off = (uint32_t*)malloc(n_pull * sizeof(uint32_t));
            uint32_t *h_ref = (uint32_t*)malloc(n_pull * sizeof(uint32_t));
            for (uint32_t i = 0; i < n_pull; i++) {
                uint64_t off = ((uint64_t)i * step * sz) % fsize;
                h_off[i] = (uint32_t)off;
                uint32_t ck = 0;
                for (uint32_t b = 0; b < sz && off+b < fsize; b++) ck ^= h_data[off+b];
                h_ref[i] = ck;
            }
            
            uint32_t *d_off, *d_ref; uint64_t *d_ts; uint8_t *d_err;
            CUDA_CHECK(cudaMalloc(&d_off, n_pull*4));
            CUDA_CHECK(cudaMalloc(&d_ref, n_pull*4));
            CUDA_CHECK(cudaMalloc(&d_ts, n_pull*8));
            CUDA_CHECK(cudaMalloc(&d_err, n_pull));
            CUDA_CHECK(cudaMemcpy(d_off, h_off, n_pull*4, cudaMemcpyHostToDevice));
            CUDA_CHECK(cudaMemcpy(d_ref, h_ref, n_pull*4, cudaMemcpyHostToDevice));
            
            uint32_t blocks = (n_pull+TPB-1)/TPB;
            cudaEvent_t st, sp; cudaEventCreate(&st); cudaEventCreate(&sp);
            cudaEventRecord(st);
            if (xor_mode)
                pull_xor_kernel<<<blocks,TPB>>>(d_hbm,d_off,n_pull,sz,d_ts,d_err,d_ref);
            else
                pull_kernel<<<blocks,TPB>>>(d_hbm,d_off,n_pull,sz,d_ts);
            cudaEventRecord(sp); cudaEventSynchronize(sp);
            float ms; cudaEventElapsedTime(&ms,st,sp);
            
            uint32_t errs = 0;
            if (xor_mode) {
                uint8_t *h_err = (uint8_t*)malloc(n_pull);
                CUDA_CHECK(cudaMemcpy(h_err, d_err, n_pull, cudaMemcpyDeviceToHost));
                for (uint32_t i = 0; i < n_pull; i++) if (h_err[i]) errs++;
                free(h_err);
            }
            
            double gb = (double)n_pull * sz / 1e9;
            double bw = (ms > 0) ? gb / (ms/1000) : 0;
            printf("  pulls=%u errors=%u time=%.2fms data=%.3fGB bw=%.2fGB/s\\n\\n", n_pull, errs, ms, gb, bw);
            results[s][m] = bw;
            
            cudaEventDestroy(st); cudaEventDestroy(sp);
            CUDA_CHECK(cudaFree(d_off)); CUDA_CHECK(cudaFree(d_ref));
            CUDA_CHECK(cudaFree(d_ts)); CUDA_CHECK(cudaFree(d_err));
            free(h_off); free(h_ref);
        }
    }
    
    printf("\\n═══ RESULTS ═══\\n");
    printf("Chunk    │ Pure Pull │ With XOR\\n");
    printf("─────────┼───────────┼──────────\\n");
    double best = 0; int bs = 0, bm = 0;
    for (int s = 0; s < n_sizes; s++) {
        const char *label = chunk_sizes[s]==64?"64B":chunk_sizes[s]==256?"256B":"1024B";
        printf("%-7s │ %.1f GB/s  │ %.1f GB/s\\n", label, results[s][0], results[s][1]);
        for (int m=0; m<2; m++) if(results[s][m]>best){best=results[s][m];bs=s;bm=m;}
    }
    printf("\\nOptimal: %s %s at %.1f GB/s\\n", 
           chunk_sizes[bs]==64?"64B":chunk_sizes[bs]==256?"256B":"1024B",
           bm==0?"pure":"XOR", best);
    printf("Total tensor data: %.1f MB\\n", fsize/1e6);
    printf("\\n=== BENCHMARK COMPLETE ===\\n");
    
    CUDA_CHECK(cudaFree(d_hbm));
    free(h_data);
    return 0;
}"""
    
    with open(src_path, "w") as f:
        f.write(cuda_src)
    
    log("Compiling CUDA benchmark...")
    r = subprocess.run(["nvcc", "-O3", "-std=c++17", "-arch=sm_75", "-o", binary, src_path, "-lm"],
                      capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        log(f"COMPILE FAILED: {r.stderr[:500]}")
        sys.exit(1)
    log(f"Compiled: {os.path.getsize(binary)/1e3:.0f} KB")
    return binary

def main():
    log("=== GPU Jet Puller — Real Model Weight Benchmark ===")
    r = subprocess.run(["nvidia-smi", "--query-gpu=name,memory.total",
                        "--format=csv,noheader"], capture_output=True, text=True, timeout=15)
    log(f"GPU: {r.stdout.strip() if r.returncode == 0 else 'NOT FOUND'}")
    
    raw, meta = extract_tensors()
    binary = compile_benchmark()
    
    log("Running benchmark...")
    os.chmod(binary, 0o755)
    r = subprocess.run([binary], capture_output=True, text=True, timeout=600)
    for line in r.stdout.split("\\n"):
        if line.strip():
            print(line, flush=True)
            with open(LOG, "a") as f:
                f.write(line + "\\n")
    if r.stderr.strip():
        log(f"STDERR: {r.stderr[:500]}")
    
    log(f"Exit: {r.returncode}")
    log(f"Full log: {LOG}")

if __name__ == "__main__":
    main()
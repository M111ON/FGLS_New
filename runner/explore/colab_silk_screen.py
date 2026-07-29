#!/usr/bin/env python3
"""
Colab Silk Screen — self-contained: writes C source, compiles, benchmarks.
Usage: !python3 colab_silk_screen.py [0.6b|3b|7b|14b|30b]

Requires: gcc (apt install gcc on Colab)
"""
import subprocess, sys, os, time, tempfile

# ── Model URLs ───────────────────────────────────────────────
MODELS = {
    "0.6b": ("https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/qwen3-0.6b-q8_0.gguf", "qwen3-0.6b-q8_0.gguf"),
    "3b":   ("https://huggingface.co/Qwen/Qwen2.5-3B-GGUF/resolve/main/qwen2.5-3b-q8_0.gguf", "qwen2.5-3b-q8_0.gguf"),
    "7b":   ("https://huggingface.co/Qwen/Qwen2.5-7B-GGUF/resolve/main/qwen2.5-7b-q8_0.gguf", "qwen2.5-7b-q8_0.gguf"),
    "14b":  ("https://huggingface.co/Qwen/Qwen2.5-14B-GGUF/resolve/main/qwen2.5-14b-q8_0.gguf", "qwen2.5-14b-q8_0.gguf"),
    "30b":  ("https://huggingface.co/Qwen/Qwen2.5-32B-GGUF/resolve/main/qwen2.5-32b-q8_0.gguf", "qwen2.5-32b-q8_0.gguf"),
}

# ── Encoder C source (embedded) ─────────────────────────────
ENCODER_C = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#define N_BOXES    10
#define N_DIRS     6
#define CLOCK_MAX  1440
#define LAYER_SLOTS (N_BOXES * N_DIRS * CLOCK_MAX)
#define GGUF_MAGIC 0x46554747u
#define MAX_TENSORS 256

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
                if(at==8){for(uint64_t j=0;j<al;j++){uint64_t s;fread(&s,8,1,f);fseek(f,s,SEEK_CUR);}}
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

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec*1000.0+ts.tv_nsec/1e6;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s model.gguf\n", argv[0]); return 1; }

    printf("============================================================\n");
    printf("  Silk Screen Benchmark — Colab\n");
    printf("  Layer: %d slots = %d × %d × %d\n", LAYER_SLOTS, N_BOXES, N_DIRS, CLOCK_MAX);
    printf("============================================================\n\n");

    GGUFFile gf;
    if (gguf_open(argv[1], &gf) < 0) { printf("FAIL: cannot open GGUF\n"); return 1; }
    printf("  Tensors: %" PRIu64 "\n", gf.n_tensors);

    int best = -1; uint64_t best_sz = 0;
    for (uint64_t i = 0; i < gf.n_tensors; i++) {
        if (gf.dtypes[i] != 8) continue;
        if (gf.sizes[i] > best_sz) { best = (int)i; best_sz = gf.sizes[i]; }
    }
    printf("  Tensor[%d]: %s (%" PRIu64 " bytes)\n\n", best, gf.names[best], gf.sizes[best]);

    // Extract int8
    int8_t *buf = (int8_t*)calloc(LAYER_SLOTS+256, 1);
    FILE *f = fopen(argv[1], "rb");
    fseek(f, gf.offsets[best], SEEK_SET);
    int nr = 0;
    uint64_t nb = gf.sizes[best] / 34;
    for (uint64_t b = 0; b < nb && nr < LAYER_SLOTS; b++) {
        fseek(f, 2, SEEK_CUR);
        for (int j = 0; j < 32 && nr < LAYER_SLOTS; j++) { fread(&buf[nr], 1, 1, f); nr++; }
    }
    fclose(f);
    printf("  Extracted: %d int8 values\n\n", nr);

    // Warm up
    int8_t silk[N_BOXES][N_DIRS][CLOCK_MAX];
    int8_t decoded[LAYER_SLOTS];
    for (int i = 0; i < 10; i++) {
        for (int idx = 0; idx < LAYER_SLOTS; idx++)
            silk[idx%N_BOXES][(idx/N_BOXES)%N_DIRS][(idx/(N_BOXES*N_DIRS))%CLOCK_MAX] = buf[idx];
        for (int idx = 0; idx < LAYER_SLOTS; idx++)
            decoded[idx] = silk[idx%N_BOXES][(idx/N_BOXES)%N_DIRS][(idx/(N_BOXES*N_DIRS))%CLOCK_MAX];
    }

    int N_ITERS = 1000;
    // Encode bench
    double t0 = now_ms();
    for (int it = 0; it < N_ITERS; it++)
        for (int idx = 0; idx < LAYER_SLOTS; idx++)
            silk[idx%N_BOXES][(idx/N_BOXES)%N_DIRS][(idx/(N_BOXES*N_DIRS))%CLOCK_MAX] = buf[idx];
    double enc_ms = now_ms() - t0;

    // Decode bench
    t0 = now_ms();
    for (int it = 0; it < N_ITERS; it++)
        for (int idx = 0; idx < LAYER_SLOTS; idx++)
            decoded[idx] = silk[idx%N_BOXES][(idx/N_BOXES)%N_DIRS][(idx/(N_BOXES*N_DIRS))%CLOCK_MAX];
    double dec_ms = now_ms() - t0;

    // Verify
    int exact = 0, worst = 0;
    for (int idx = 0; idx < LAYER_SLOTS; idx++) {
        int e = abs((int)buf[idx]-(int)decoded[idx]);
        if (e > worst) worst = e;
        if (e == 0) exact++;
    }

    double tw = (double)N_ITERS * LAYER_SLOTS;
    printf("=== RESULTS ===\n");
    printf("  Encode: %.1f ms total, %.3f ms/layer, %.1f M weights/sec\n",
           enc_ms, enc_ms/N_ITERS, tw/enc_ms/1000.0);
    printf("  Decode: %.1f ms total, %.3f ms/layer, %.1f M weights/sec\n",
           dec_ms, dec_ms/N_ITERS, tw/dec_ms/1000.0);
    printf("  Lossless: %d/%d exact (%.1f%%), worst=%d — %s\n",
           exact, LAYER_SLOTS, 100.0*exact/LAYER_SLOTS, worst,
           worst==0 ? "✓ LOSSLESS" : "✗ LOSSY");

    // Scale
    double lm = enc_ms / N_ITERS;
    printf("\n=== SCALE (CPU encode) ===\n");
    printf("  %-12s %10s %8s %10s\n", "Model", "Layers", "Silk(GB)", "Time(s)");
    struct { const char*n; uint64_t w; } M[] = {
        {"0.6B",600000000},{"3B",3000000000},{"7B",7000000000},
        {"14B",14000000000},{"30B",30000000000}
    };
    for (int i=0;i<5;i++) {
        int l=(int)((M[i].w+LAYER_SLOTS-1)/LAYER_SLOTS);
        printf("  %-12s %10d %8.1f %10.1f\n", M[i].n, l, (double)l*LAYER_SLOTS/1e9, l*lm/1000.0);
    }
    printf("\n  ✓ All values identity-mapped (lossless by design)\n");

    free(buf);
    for (uint64_t i=0;i<gf.n_tensors;i++) free(gf.names[i]);
    return 0;
}
'''

def run(cmd, **kw):
    print(f"  $ {cmd}")
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)
    if r.stdout: print(r.stdout)
    if r.returncode != 0 and r.stderr: print(f"  ERR: {r.stderr[:500]}")
    return r.returncode

def main():
    print("=" * 60)
    print("  Colab Silk Screen Benchmark")
    print("=" * 60)

    key = sys.argv[1] if len(sys.argv) > 1 else "0.6b"
    if key not in MODELS:
        print(f"  Unknown model: {key}")
        print(f"  Available: {', '.join(MODELS.keys())}")
        return 1

    url, name = MODELS[key]
    print(f"\n  Model: {key.upper()} → {name}")

    # 1. Ensure gcc
    print("\n=== Step 1: Ensure compiler ===")
    run("apt-get update -qq && apt-get install -y -qq gcc > /dev/null 2>&1 || true")

    # 2. Write C source
    print("\n=== Step 2: Write encoder source ===")
    with open("silk_screen_bench.c", "w") as f:
        f.write(ENCODER_C)
    print("  ✓ Written silk_screen_bench.c")

    # 3. Compile
    print("\n=== Step 3: Compile ===")
    rc = run("gcc -O2 -std=c11 -Wall -o silk_screen_bench silk_screen_bench.c -lm")
    if rc != 0:
        print("  ✗ Compile failed"); return 1
    print("  ✓ Compiled")

    # 4. Download model
    print(f"\n=== Step 4: Download {name} ===")
    if not os.path.exists(name):
        rc = run(f"curl -L -o {name} '{url}'")
        if rc != 0: print("  ✗ Download failed"); return 1
    else:
        print(f"  ✓ Already exists ({os.path.getsize(name)/1e6:.0f} MB)")

    # 5. Benchmark
    print("\n=== Step 5: Benchmark ===")
    t0 = time.time()
    rc = run(f"./silk_screen_bench {name}", timeout=600)
    elapsed = time.time() - t0
    print(f"\n  Wall time: {elapsed:.1f}s")
    return rc

if __name__ == "__main__":
    sys.exit(main())

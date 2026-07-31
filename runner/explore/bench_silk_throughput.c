// bench_silk_throughput.c
// Real GGUF benchmark — encode + decode throughput
// Compile: gcc -O2 -std=c11 -Wall -o bench_silk_throughput.exe bench_silk_throughput.c -lm
// Run:     bench_silk_throughput.exe [model.gguf]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#define N_BOXES    10
#define N_DIRS     6
#define CLOCK_MAX  1440
#define LAYER_SLOTS (N_BOXES * N_DIRS * CLOCK_MAX)  // 86400
#define GGUF_MAGIC 0x46554747u
#define MAX_TENSORS 256

// ── GGUF Reader ─────────────────────────────────────────────
typedef struct {
    uint64_t n_tensors;
    char     *names[MAX_TENSORS];
    uint32_t dtypes[MAX_TENSORS];
    uint64_t offsets[MAX_TENSORS];
    uint64_t sizes[MAX_TENSORS];
} GGUFFile;

static FILE *gguf_fopen(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) return f;
    // MSYS2 /i/ → Windows I:\ conversion
    if (path[0] == '/' && path[2] == '/') {
        char winpath[512];
        snprintf(winpath, sizeof(winpath), "%c:\\%s", path[1], path + 3);
        for (char *p = winpath; *p; p++) if (*p == '/') *p = '\\';
        f = fopen(winpath, "rb");
    }
    return f;
}

static int gguf_open(const char *path, GGUFFile *gf) {
    memset(gf, 0, sizeof(GGUFFile));
    FILE *f = gguf_fopen(path);
    if (!f) return -1;

    uint32_t magic, version;
    uint64_t n_tensors, n_kv;
    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);
    fread(&n_tensors, 8, 1, f);
    fread(&n_kv, 8, 1, f);
    if (magic != GGUF_MAGIC) { fclose(f); return -1; }
    gf->n_tensors = n_tensors < MAX_TENSORS ? n_tensors : MAX_TENSORS;

    for (uint64_t i = 0; i < n_kv; i++) {
        uint64_t klen; fread(&klen, 8, 1, f);
        fseek(f, klen, SEEK_CUR);
        uint32_t vtype; fread(&vtype, 4, 1, f);
        switch (vtype) {
            case 0: case 1: fseek(f, 1, SEEK_CUR); break;
            case 2: case 3: fseek(f, 2, SEEK_CUR); break;
            case 4: case 5: case 6: fseek(f, 4, SEEK_CUR); break;
            case 7: fseek(f, 1, SEEK_CUR); break;
            case 8: { uint64_t sl; fread(&sl, 8, 1, f); fseek(f, sl, SEEK_CUR); break; }
            case 9: { uint32_t at; uint64_t al; fread(&at,4,1,f); fread(&al,8,1,f);
                if(at==8){for(uint64_t j=0;j<al;j++){uint64_t sl;fread(&sl,8,1,f);fseek(f,sl,SEEK_CUR);}}
                else fseek(f,al*(at<=3?2:at<=6?4:1),SEEK_CUR); break; }
            case 10: case 11: case 12: fseek(f, 8, SEEK_CUR); break;
            default: fseek(f, 4, SEEK_CUR); break;
        }
    }

    for (uint64_t i = 0; i < gf->n_tensors; i++) {
        uint64_t nlen; fread(&nlen, 8, 1, f);
        gf->names[i] = (char*)calloc(nlen + 1, 1);
        if (!gf->names[i]) {
            printf("names alloc failed\n");
            fclose(f);
            return 1;
        }
        fread(gf->names[i], nlen, 1, f);
        uint32_t nd; fread(&nd, 4, 1, f);
        for (uint32_t j = 0; j < nd; j++) { uint64_t d; fread(&d, 8, 1, f); }
        fread(&gf->dtypes[i], 4, 1, f);
        fread(&gf->offsets[i], 8, 1, f);
    }

    for (uint64_t i = 0; i < gf->n_tensors; i++) {
        if (i + 1 < gf->n_tensors)
            gf->sizes[i] = gf->offsets[i+1] - gf->offsets[i];
        else {
            long cur = ftell(f);
            fseek(f, 0, SEEK_END);
            gf->sizes[i] = ftell(f) - gf->offsets[i];
            fseek(f, cur, SEEK_SET);
        }
    }
    fclose(f);
    return 0;
}

static void gguf_close(GGUFFile *gf) {
    for (uint64_t i = 0; i < gf->n_tensors; i++) free(gf->names[i]);
}

// ── High-resolution timer ───────────────────────────────────
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ── Silk Screen operations ──────────────────────────────────
// Encode: data[idx] → filter[idx%10][idx/10%6][idx/60%1440]
static void silk_encode(int8_t silk[N_BOXES][N_DIRS][CLOCK_MAX],
                        const int8_t *data, int n) {
    for (int idx = 0; idx < n; idx++) {
        int box = idx % N_BOXES;
        int dir = (idx / N_BOXES) % N_DIRS;
        int tick = (idx / (N_BOXES * N_DIRS)) % CLOCK_MAX;
        silk[box][dir][tick] = data[idx];
    }
}

// Decode: filter → output (identity)
static void silk_decode(const int8_t silk[N_BOXES][N_DIRS][CLOCK_MAX],
                        int8_t *out, int n) {
    for (int idx = 0; idx < n; idx++) {
        int box = idx % N_BOXES;
        int dir = (idx / N_BOXES) % N_DIRS;
        int tick = (idx / (N_BOXES * N_DIRS)) % CLOCK_MAX;
        out[idx] = silk[box][dir][tick];
    }
}

// Verify: compare two buffers
static int verify(const int8_t *a, const int8_t *b, int n, int *worst) {
    int exact = 0;
    *worst = 0;
    for (int i = 0; i < n; i++) {
        int err = abs((int)a[i] - (int)b[i]);
        if (err > *worst) *worst = err;
        if (err == 0) exact++;
    }
    return exact;
}

// ── Main benchmark ──────────────────────────────────────────
int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "/i/model/Qwen3-0.6B-Q8_0.gguf";

    printf("============================================================\n");
    printf("  Silk Screen Throughput Benchmark\n");
    printf("============================================================\n");
    printf("  Layer: %d slots = %d × %d × %d\n\n", LAYER_SLOTS, N_BOXES, N_DIRS, CLOCK_MAX);

    // ── 1. Open GGUF ────────────────────────────────────────
    GGUFFile gf;
    if (gguf_open(model, &gf) < 0) {
        printf("  FAIL: cannot open %s\n", model);
        return 1;
    }
    printf("  Model: %s\n", model);
    printf("  Tensors: %" PRIu64 "\n", gf.n_tensors);

    // Find largest Q8_0 tensor
    int best = -1;
    uint64_t best_sz = 0;
    for (uint64_t i = 0; i < gf.n_tensors; i++) {
        if (gf.dtypes[i] != 8) continue;
        if (gf.sizes[i] > best_sz) { best = (int)i; best_sz = gf.sizes[i]; }
    }
    if (best < 0) {
        printf("  FAIL: no Q8_0 tensor found\n");
        gguf_close(&gf);
        return 1;
    }

    printf("  Tensor[%d]: %s\n", best, gf.names[best]);
    printf("    dtype=%u  size=%" PRIu64 " bytes\n", gf.dtypes[best], gf.sizes[best]);

    // ── 2. Extract int8 values ───────────────────────────────
    int8_t *buf = (int8_t*)calloc(LAYER_SLOTS + 256, 1);
    if (!buf) {
        printf("buf alloc failed\n");
        fclose(f);
        return 1;
    }
    FILE *f = gguf_fopen(model);
    if (!f) { printf("FAIL: reopen\n"); free(buf); gguf_close(&gf); return 1; }
    fseek(f, gf.offsets[best], SEEK_SET);

    int n_read = 0;
    uint64_t n_blocks = gf.sizes[best] / 34;
    for (uint64_t b = 0; b < n_blocks && n_read < LAYER_SLOTS; b++) {
        fseek(f, 2, SEEK_CUR);  // skip fp16 scale
        for (int j = 0; j < 32 && n_read < LAYER_SLOTS; j++) {
            fread(&buf[n_read], 1, 1, f);
            n_read++;
        }
    }
    fclose(f);
    printf("  Extracted: %d int8 values\n\n", n_read);

    // ── 3. Warm up ───────────────────────────────────────────
    int8_t silk[N_BOXES][N_DIRS][CLOCK_MAX];
    int8_t decoded[LAYER_SLOTS];
    int worst;

    for (int i = 0; i < 10; i++) {
        silk_encode(silk, buf, LAYER_SLOTS);
        silk_decode(silk, decoded, LAYER_SLOTS);
    }

    // ── 4. Benchmark: Encode throughput ──────────────────────
    int N_ITERS = 1000;
    double t0 = now_ms();
    for (int i = 0; i < N_ITERS; i++) {
        silk_encode(silk, buf, LAYER_SLOTS);
    }
    double t1 = now_ms();
    double enc_ms = t1 - t0;
    double total_weights = (double)N_ITERS * LAYER_SLOTS;

    printf("=== ENCODE (data → silk screen) ===\n");
    printf("  Iterations:    %d\n", N_ITERS);
    printf("  Total time:    %.1f ms\n", enc_ms);
    printf("  Per layer:     %.3f ms\n", enc_ms / N_ITERS);
    printf("  Throughput:    %.1f M weights/sec\n", total_weights / enc_ms / 1000.0);
    printf("  Bandwidth:     %.1f GB/s (read + store)\n",
           total_weights / enc_ms / 1000.0 * 1.0 / 1024.0);

    // ── 5. Benchmark: Decode throughput ──────────────────────
    t0 = now_ms();
    for (int i = 0; i < N_ITERS; i++) {
        silk_decode(silk, decoded, LAYER_SLOTS);
    }
    t1 = now_ms();
    double dec_ms = t1 - t0;

    printf("\n=== DECODE (silk screen → output) ===\n");
    printf("  Iterations:    %d\n", N_ITERS);
    printf("  Total time:    %.1f ms\n", dec_ms);
    printf("  Per layer:     %.3f ms\n", dec_ms / N_ITERS);
    printf("  Throughput:    %.1f M weights/sec\n", total_weights / dec_ms / 1000.0);
    printf("  Bandwidth:     %.1f GB/s\n",
           total_weights / dec_ms / 1000.0 * 1.0 / 1024.0);

    // ── 6. Verify lossless ───────────────────────────────────
    silk_encode(silk, buf, LAYER_SLOTS);
    silk_decode(silk, decoded, LAYER_SLOTS);
    int exact = verify(buf, decoded, LAYER_SLOTS, &worst);

    printf("\n=== LOSSLESS VERIFICATION ===\n");
    printf("  Exact:  %d/%d (%.1f%%)\n", exact, LAYER_SLOTS, 100.0*exact/LAYER_SLOTS);
    printf("  Worst:  %d\n", worst);
    printf("  Status: %s\n", worst == 0 ? "✓ LOSSLESS" : "✗ LOSSY");

    // ── 7. Scale projection ──────────────────────────────────
    double layer_ms = enc_ms / N_ITERS;
    printf("\n=== SCALE PROJECTION (encode only) ===\n");
    printf("  %-16s %12s %10s %10s %12s\n",
           "Model", "Weights", "Layers", "Silk(GB)", "Encode(s)");
    printf("  %-16s %12s %10s %10s %12s\n",
           "-----", "-------", "------", "--------", "---------");

    struct { const char *name; uint64_t w; } models[] = {
        {"Qwen3-0.6B",  600000000ULL},
        {"Qwen2.5-3B", 3000000000ULL},
        {"Qwen2.5-7B", 7000000000ULL},
        {"Qwen2.5-14B",14000000000ULL},
        {"Qwen2.5-30B",30000000000ULL},
    };
    for (int i = 0; i < 5; i++) {
        int layers = (int)((models[i].w + LAYER_SLOTS - 1) / LAYER_SLOTS);
        double gb = (double)layers * LAYER_SLOTS / 1e9;
        double sec = layers * layer_ms / 1000.0;
        printf("  %-16s %12" PRIu64 " %10d %10.1f %12.1f\n",
               models[i].name, models[i].w, layers, gb, sec);
    }

    printf("\n  Layer encode: %.3f ms (%.1f M weights/sec)\n", layer_ms, LAYER_SLOTS/layer_ms/1000.0);
    printf("  Storage ratio: 1:1 (identity = raw int8)\n");
    printf("  Value: STRUCTURED ACCESS — 60 read heads/tick\n\n");

    free(buf);
    gguf_close(&gf);
    return 0;
}

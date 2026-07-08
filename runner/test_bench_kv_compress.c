/*
 * test_bench_kv_compress.c — KV Cache Compression Benchmark
 * ═══════════════════════════════════════════════════════════
 *
 * Compare 3 methods for compressing KV cache XOR diffs:
 *   A. RLE (KV Remap current method)
 *   B. Binary Shell (64B chunks → FLAT/SPARSE/DENSE)
 *   C. Diamond Shell (64B chunks → 3D rotation + fibo_intersect)
 *
 * Mock KV data: 6 layers × 512 embd × 1024 ctx × f16 = 12 MB
 * Tests at 0%/15%/40%/60%/85% change.
 *
 * Build: gcc -O2 -std=c11 -I../collection/geopixel -I../collection/Hfolder
 *        -I../collection/geo_jump_module/include -I../collection/dgls/diamond/include
 *        -o test_bench_kv_compress.exe test_bench_kv_compress.c -lm
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* ── Includes for compression codecs ─────────────────── */
#include "kv_remap.h"
#include "binary_shell_codec.h"
#include "diamond_shell_v2.h"

/* ── Timer helpers ───────────────────────────────────── */
typedef struct { long tv_sec; long tv_nsec; } BenchTimer;

static inline void timer_now(BenchTimer *t) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    t->tv_sec  = (long)(c.QuadPart / f.QuadPart);
    t->tv_nsec = (long)(c.QuadPart % f.QuadPart * 1000000000LL / f.QuadPart);
#else
    clock_gettime(CLOCK_MONOTONIC, (struct timespec *)t);
#endif
}

static inline double timer_diff_ms(BenchTimer *a, BenchTimer *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 +
           (double)(b->tv_nsec - a->tv_nsec) / 1000000.0;
}

/* ── KV Cache parameters (LFM2-like) ─────────────────── */
#define N_LAYERS     6
#define N_EMBD       512
#define N_CTX        1024
#define NB1          (N_EMBD * (int)sizeof(uint16_t))
#define KV_LAYER_BYTES (N_EMBD * N_CTX * (int)sizeof(uint16_t))
#define KV_TOTAL_BYTES (N_LAYERS * 2 * N_EMBD * N_CTX * (int)sizeof(uint16_t))

/* ── Mock KV data generation ─────────────────────────── */
static void kv_generate_full(uint16_t *buf, int n_embd, int n_ctx, int n_layers2,
                             int fill_pct, uint32_t seed, int is_skeleton)
{
    for (int l = 0; l < n_layers2; l++) {
        for (int pos = 0; pos < n_ctx; pos++) {
            int active = (pos * 100 / n_ctx) < fill_pct;
            for (int d = 0; d < n_embd; d++) {
                if (!active) {
                    buf[l * n_embd * n_ctx + pos * n_embd + d] = 0;
                } else {
                    uint32_t hash = (uint32_t)(l * 1000000 + pos * 7919 + d * 104729 + seed);
                    uint16_t val = (uint16_t)((hash ^ (hash >> 16)) & 0xFFFF);
                    val = val % 2048;
                    buf[l * n_embd * n_ctx + pos * n_embd + d] = val;
                }
            }
        }
    }
}

static void apply_change(uint16_t *buf, int pct, uint32_t seed_xor) {
    for (int l = 0; l < N_LAYERS * 2; l++) {
        for (int pos = 0; pos < N_CTX; pos++) {
            if ((pos * 100 / N_CTX) < pct) {
                for (int d = 0; d < N_EMBD; d++) {
                    uint32_t hash = (uint32_t)(l * 3000000 + pos * 1337 + d * 99991 + seed_xor);
                    buf[l * N_EMBD * N_CTX + pos * N_EMBD + d] =
                        (uint16_t)((hash ^ (hash >> 16)) % 2048);
                }
            }
        }
    }
}

/* ── Method A: RLE compress (from kv_remap.h) ────────── */
typedef struct {
    uint64_t raw_size;
    uint64_t comp_size;
    double   ratio;
    double   encode_ms;
    double   decode_ms;
    int      method;  /* 0=raw, 1=RLE, 2=GEO */
} RLEResult;

static RLEResult method_rle(const uint8_t *diff, size_t diff_size) {
    RLEResult r;
    memset(&r, 0, sizeof(r));
    r.raw_size = diff_size;

    BenchTimer t0, t1;

    /* Encode */
    void *comp = NULL;
    size_t comp_sz = 0;
    timer_now(&t0);
    int method = kv_remap_compress(diff, diff_size, &comp, &comp_sz);
    timer_now(&t1);
    r.encode_ms = timer_diff_ms(&t0, &t1);
    r.method = method;

    if (comp && comp_sz > 0) {
        r.comp_size = comp_sz;
        r.ratio = (double)diff_size / (double)comp_sz;

        /* Decode */
        size_t dec_sz = 0;
        timer_now(&t0);
        void *dec = kv_remap_decompress(comp, comp_sz, &dec_sz);
        timer_now(&t1);
        r.decode_ms = timer_diff_ms(&t0, &t1);

        /* Verify */
        if (dec && dec_sz == diff_size) {
            if (memcmp(dec, diff, diff_size) != 0) {
                fprintf(stderr, "  [RLE] VERIFY FAILED!\n");
            }
        } else {
            fprintf(stderr, "  [RLE] decode size mismatch: %zu vs %zu\n", dec_sz, diff_size);
        }
        free(comp);
        free(dec);
    } else {
        /* Method stored raw (n_runs=0) */
        r.comp_size = diff_size;
        r.ratio = 1.0;
    }

    return r;
}

/* ── Method B: Binary Shell (64B chunks) ─────────────── */
typedef struct {
    uint64_t raw_size;
    uint64_t comp_size;
    double   ratio;
    double   encode_ms;
    double   decode_ms;
    uint64_t flat_count;
    uint64_t sparse_count;
    uint64_t dense_count;
} BinShellResult;

static BinShellResult method_binary_shell(const uint8_t *diff, size_t diff_size) {
    BinShellResult r;
    memset(&r, 0, sizeof(r));
    r.raw_size = diff_size;

    uint64_t n_chunks = (diff_size + 63) / 64;

    /* Allocate output buffer (worst case: 70B per chunk) */
    uint8_t *comp = (uint8_t *)malloc(n_chunks * 70);
    if (!comp) return r;

    BenchTimer t0, t1;

    /* Encode */
    timer_now(&t0);
    uint64_t comp_offset = 0;
    for (uint64_t ci = 0; ci < n_chunks; ci++) {
        uint64_t off = ci * 64;
        uint8_t chunk[64];
        size_t remain = (size_t)(diff_size - off);
        memset(chunk, 0, 64);
        memcpy(chunk, diff + off, remain < 64 ? remain : 64);

        BinChunkResult cr = bin_classify_chunk(chunk);
        comp_offset += bin_encode_chunk(comp + comp_offset, chunk, &cr);

        switch (cr.flag) {
            case BIN_FLAG_FLAT:   r.flat_count++;   break;
            case BIN_FLAG_SPARSE: r.sparse_count++; break;
            case BIN_FLAG_DENSE:  r.dense_count++;  break;
        }
    }
    timer_now(&t1);
    r.encode_ms = timer_diff_ms(&t0, &t1);
    r.comp_size = comp_offset;
    r.ratio = (double)diff_size / (double)comp_offset;

    /* Decode + verify */
    uint8_t *dec = (uint8_t *)malloc(n_chunks * 64);
    if (dec) {
        timer_now(&t0);
        uint64_t dec_offset = 0;
        for (uint64_t ci = 0; ci < n_chunks; ci++) {
            uint32_t sz = bin_decode_chunk(comp + dec_offset, dec + ci * 64);
            dec_offset += sz;
        }
        timer_now(&t1);
        r.decode_ms = timer_diff_ms(&t0, &t1);

        if (memcmp(dec, diff, diff_size) != 0) {
            fprintf(stderr, "  [BIN] VERIFY FAILED!\n");
        }
        free(dec);
    }

    free(comp);
    return r;
}

/* ── Method C: Diamond Shell (64B chunks) ────────────── */
typedef struct {
    uint64_t raw_size;
    uint64_t comp_size;
    double   ratio;
    double   encode_ms;
    double   decode_ms;
    uint64_t flat_count;
    uint64_t sparse_count;
    uint64_t dense_count;
} DiaShellResult;

static DiaShellResult method_diamond_shell(const uint8_t *diff, size_t diff_size) {
    DiaShellResult r;
    memset(&r, 0, sizeof(r));
    r.raw_size = diff_size;

    uint64_t n_chunks = (diff_size + 63) / 64;

    /* Allocate output buffer (worst case: 66B per chunk) */
    uint8_t *comp = (uint8_t *)malloc(n_chunks * 66);
    if (!comp) return r;

    BenchTimer t0, t1;

    /* Encode — use shell_stream_encode for proven path */
    uint64_t comp_offset = 0;
    timer_now(&t0);
    comp_offset = shell_stream_encode(diff, n_chunks, comp);
    timer_now(&t1);
    r.encode_ms = timer_diff_ms(&t0, &t1);
    r.comp_size = comp_offset;
    r.ratio = (double)diff_size / (double)comp_offset;

    /* Decode + verify */
    uint8_t *dec = (uint8_t *)malloc(n_chunks * 64);
    if (dec) {
        timer_now(&t0);
        uint64_t dec_offset = 0;
        for (uint64_t ci = 0; ci < n_chunks; ci++) {
            uint32_t sz = shell_stream_decode(comp + dec_offset, 1, dec + ci * 64);
            dec_offset += sz;
        }
        timer_now(&t1);
        r.decode_ms = timer_diff_ms(&t0, &t1);

        if (memcmp(dec, diff, diff_size) != 0) {
            fprintf(stderr, "  [DIA] VERIFY FAILED!\n");
        }
        free(dec);
    }

    free(comp);
    return r;
}

/* ── Pretty print ────────────────────────────────────── */
static void print_separator(const char *title) {
    fprintf(stderr, "\n═══════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "  %s\n", title);
    fprintf(stderr, "═══════════════════════════════════════════════════════════════\n");
}

/* ── Main ────────────────────────────────────────────── */
int main(void) {
    print_separator("KV Cache Compression Benchmark");
    fprintf(stderr, "  Config: %d layers × %d embd × %d ctx × 2B (f16)\n",
        N_LAYERS, N_EMBD, N_CTX);
    fprintf(stderr, "  Total: %.1f MB\n", (double)KV_TOTAL_BYTES / (1048576.0));

    /* Allocate buffers */
    size_t kv_u16 = (size_t)N_LAYERS * 2 * N_EMBD * N_CTX;
    uint16_t *skeleton = (uint16_t *)calloc(kv_u16, sizeof(uint16_t));
    uint16_t *current  = (uint16_t *)calloc(kv_u16, sizeof(uint16_t));
    if (!skeleton || !current) { fprintf(stderr, "OOM\n"); return 1; }

    /* Generate skeleton */
    kv_generate_full(skeleton, N_EMBD, N_CTX, N_LAYERS * 2, 100, 42, 1);

    /* Test scenarios */
    struct {
        const char *label;
        int pct;
        uint32_t seed;
        const char *meaning;
    } tests[] = {
        {"0%",   0,   0,    "skeleton replay — no change"},
        {"15%", 15,  777,   "same topic, new sentence"},
        {"40%", 40,  123,   "related topic shift"},
        {"60%", 60,  456,   "new topic emerging"},
        {"85%", 85,  999,   "topic changed, near-full"},
    };
    int n_tests = 5;

    /* Results storage */
    RLEResult      rle[5];
    BinShellResult bin[5];
    DiaShellResult dia[5];

    for (int t = 0; t < n_tests; t++) {
        char title[128];
        snprintf(title, sizeof(title), "%s change — %s", tests[t].label, tests[t].meaning);
        print_separator(title);

        /* Generate current state */
        memcpy(current, skeleton, kv_u16 * sizeof(uint16_t));
        if (tests[t].pct > 0)
            apply_change(current, tests[t].pct, tests[t].seed);

        /* Compute XOR diff */
        uint8_t *diff = (uint8_t *)malloc(KV_TOTAL_BYTES);
        const uint8_t *sk = (const uint8_t *)skeleton;
        const uint8_t *cu = (const uint8_t *)current;
        for (uint64_t i = 0; i < KV_TOTAL_BYTES; i++)
            diff[i] = sk[i] ^ cu[i];

        /* Count non-zero bytes */
        uint64_t nz = 0;
        for (uint64_t i = 0; i < KV_TOTAL_BYTES; i++)
            if (diff[i] != 0) nz++;
        fprintf(stderr, "  XOR diff: %llu / %llu bytes (%.1f%% changed)\n",
            (unsigned long long)nz, (unsigned long long)KV_TOTAL_BYTES,
            (double)nz / KV_TOTAL_BYTES * 100.0);

        /* Run all 3 methods */
        rle[t] = method_rle(diff, KV_TOTAL_BYTES);
        bin[t] = method_binary_shell(diff, KV_TOTAL_BYTES);
        dia[t] = method_diamond_shell(diff, KV_TOTAL_BYTES);

        /* Print results */
        fprintf(stderr, "\n  [A] RLE (KV Remap):\n");
        fprintf(stderr, "    ratio: %.2fx  enc: %.3f ms  dec: %.3f ms\n",
            rle[t].ratio, rle[t].encode_ms, rle[t].decode_ms);

        fprintf(stderr, "\n  [B] Binary Shell:\n");
        fprintf(stderr, "    ratio: %.2fx  enc: %.3f ms  dec: %.3f ms\n",
            bin[t].ratio, bin[t].encode_ms, bin[t].decode_ms);
        fprintf(stderr, "    FLAT: %llu  SPARSE: %llu  DENSE: %llu\n",
            (unsigned long long)bin[t].flat_count,
            (unsigned long long)bin[t].sparse_count,
            (unsigned long long)bin[t].dense_count);

        fprintf(stderr, "\n  [C] Diamond Shell:\n");
        fprintf(stderr, "    ratio: %.2fx  enc: %.3f ms  dec: %.3f ms\n",
            dia[t].ratio, dia[t].encode_ms, dia[t].decode_ms);
        fprintf(stderr, "    FLAT: %llu  SPARSE: %llu  DENSE: %llu\n",
            (unsigned long long)dia[t].flat_count,
            (unsigned long long)dia[t].sparse_count,
            (unsigned long long)dia[t].dense_count);

        free(diff);
    }

    /* Summary table */
    print_separator("Summary Comparison");
    fprintf(stderr, "\n  %-5s │ %-18s │ %-18s │ %-18s\n", "Chg", "RLE", "Binary Shell", "Diamond Shell");
    fprintf(stderr, "  ──────┼────────────────────┼────────────────────┼────────────────────\n");
    for (int t = 0; t < n_tests; t++) {
        fprintf(stderr, "  %-5s │ %5.2fx %6.3f ms   │ %5.2fx %6.3f ms   │ %5.2fx %6.3f ms\n",
            tests[t].label,
            rle[t].ratio, rle[t].encode_ms,
            bin[t].ratio, bin[t].encode_ms,
            dia[t].ratio, dia[t].encode_ms);
    }

    /* Winner analysis */
    print_separator("Winner Analysis");
    for (int t = 0; t < n_tests; t++) {
        double best_ratio = rle[t].ratio;
        const char *best = "RLE";
        if (bin[t].ratio > best_ratio) { best_ratio = bin[t].ratio; best = "Binary"; }
        if (dia[t].ratio > best_ratio) { best_ratio = dia[t].ratio; best = "Diamond"; }
        fprintf(stderr, "  %5s → %s (%.2fx)\n", tests[t].label, best, best_ratio);
    }

    free(skeleton);
    free(current);
    fprintf(stderr, "\nDone.\n");
    return 0;
}

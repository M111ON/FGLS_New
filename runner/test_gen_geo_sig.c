/*
 * test_gen_geo_sig.c — P1: Generative Geo-Signature Benchmark
 * ═══════════════════════════════════════════════════════════
 *
 * Compare 3 storage strategies for KV cache XOR diffs:
 *   A. Full store: store complete XOR diff (raw bytes)
 *   B. Diamond Shell: FLAT=2B, SPARSE/DENSE=66B per chunk
 *   C. Generative Sig: FLAT=1B, SPARSE=11B (rot+pc+seed), DENSE=66B
 *
 * Strategy C is the "store rules, not data" paradigm:
 *   - FLAT: just flag byte (regenerate all-zeros)
 *   - SPARSE: geometric signature (rotation + popcount + FNV seed)
 *             attempt regeneration from signature
 *   - DENSE: store full rotated data (no generative shortcut)
 *
 * Mock KV data: 6 layers × 512 embd × 1024 ctx × f16 = 12 MB
 *
 * Build: gcc -O2 -std=c11 -I../collection/geopixel -I../collection/Hfolder
 *        -I../collection/geo_jump_module/include -I../collection/dgls/diamond/include
 *        -o test_gen_geo_sig.exe test_gen_geo_sig.c -L. -lzstd -lm
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

#include "diamond_shell_v2.h"
#include "diamond_shell_codec.h"
#include "binary_shell_codec.h"

/* ── Timer ───────────────────────────────────────────── */
typedef struct { long tv_sec; long tv_nsec; } P1Timer;
static inline void timer_now(P1Timer *t) {
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
static inline double timer_diff_ms(P1Timer *a, P1Timer *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 +
           (double)(b->tv_nsec - a->tv_nsec) / 1000000.0;
}

/* ── KV Cache parameters ─────────────────────────────── */
#define N_LAYERS     6
#define N_EMBD       512
#define N_CTX        1024
#define KV_TOTAL_BYTES (N_LAYERS * 2 * N_EMBD * N_CTX * (int)sizeof(uint16_t))

/* ── Mock KV generation (same as bench_kv_compress) ──── */
static void kv_generate_full(uint16_t *buf, int n_embd, int n_ctx, int n_layers2,
                             int fill_pct, uint32_t seed) {
    for (int l = 0; l < n_layers2; l++)
        for (int pos = 0; pos < n_ctx; pos++) {
            int active = (pos * 100 / n_ctx) < fill_pct;
            for (int d = 0; d < n_embd; d++) {
                if (!active) {
                    buf[l * n_embd * n_ctx + pos * n_embd + d] = 0;
                } else {
                    uint32_t h = (uint32_t)(l * 1000000 + pos * 7919 + d * 104729 + seed);
                    buf[l * n_embd * n_ctx + pos * n_embd + d] = (uint16_t)((h ^ (h >> 16)) % 2048);
                }
            }
        }
}

static void apply_change(uint16_t *buf, int pct, uint32_t seed_xor) {
    for (int l = 0; l < N_LAYERS * 2; l++)
        for (int pos = 0; pos < N_CTX; pos++)
            if ((pos * 100 / N_CTX) < pct)
                for (int d = 0; d < N_EMBD; d++) {
                    uint32_t h = (uint32_t)(l * 3000000 + pos * 1337 + d * 99991 + seed_xor);
                    buf[l * N_EMBD * N_CTX + pos * N_EMBD + d] = (uint16_t)((h ^ (h >> 16)) % 2048);
                }
}

/* ── FNV64 hash (same as diamond_shell) ─────────────── */
static inline uint64_t fnv64(const uint8_t *d, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

/* ── Strategy C: Generative Geo-Signature ──────────────
 *
 * Per-chunk wire format:
 *   FLAT:   [flag:1]                           = 1B
 *   SPARSE: [flag:1][rot:1][isect_pc:1][seed:8] = 11B
 *   DENSE:  [flag:1][rot:1][rotbuf:64]          = 66B (same as Diamond Shell)
 */
typedef struct {
    uint64_t raw_size;
    uint64_t gen_comp_size;      /* strategy C: generative signatures */
    uint64_t diamond_comp_size;  /* strategy B: diamond shell lossless */
    double   gen_ratio;
    double   diamond_ratio;
    double   gen_encode_ms;
    uint64_t flat_count;
    uint64_t sparse_count;
    uint64_t dense_count;
    uint64_t gen_flat_bytes;
    uint64_t gen_sparse_bytes;
    uint64_t gen_dense_bytes;
} GenSigResult;

static GenSigResult method_gen_sig(const uint8_t *diff, size_t diff_size) {
    GenSigResult r;
    memset(&r, 0, sizeof(r));
    r.raw_size = diff_size;

    uint64_t n_chunks = (diff_size + 63) / 64;

    /* Allocate buffers */
    uint8_t *diamond_comp = (uint8_t *)malloc(n_chunks * 66);
    uint8_t *gen_comp = (uint8_t *)malloc(n_chunks * 66);  /* worst case */
    if (!diamond_comp || !gen_comp) { free(diamond_comp); free(gen_comp); return r; }

    P1Timer t0, t1;

    /* Encode with shell_stream_encode (Diamond Shell lossless) */
    timer_now(&t0);
    uint64_t diamond_sz = shell_stream_encode(diff, n_chunks, diamond_comp);
    timer_now(&t1);
    r.diamond_comp_size = diamond_sz;
    r.diamond_ratio = (double)diff_size / (double)diamond_sz;

    /* Now scan the Diamond Shell output and create Generative Sig version */
    uint64_t gen_off = 0;
    uint64_t ds_off = 0;

    for (uint64_t ci = 0; ci < n_chunks; ci++) {
        uint8_t flag = diamond_comp[ds_off];
        uint8_t rot  = diamond_comp[ds_off + 1];

        if (flag == SHELL_FLAG_FLAT) {
            /* FLAT: store just 1 byte (flag only) */
            gen_comp[gen_off++] = 0x00;  /* GEN_FLAG_FLAT */
            r.flat_count++;
            r.gen_flat_bytes++;
            ds_off += 2;  /* skip flag+rot in diamond stream */
        } else if (flag == SHELL_FLAG_SPARSE) {
            /* SPARSE: store flag + rot + isect_pc + FNV seed (11B total) */
            /* We need to re-classify to get isect_pc and seed */
            uint8_t rotbuf[64];
            memcpy(rotbuf, diamond_comp + ds_off + 2, 64);

            /* Rebuild DiamondBlock to get fibo_intersect popcount */
            DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, (uint32_t)ci);
            if (!fold_xor_audit(&db)) {
                db.invert = ~db.core.raw;
                fold_build_quad_mirror(&db);
            }
            uint64_t isect = fold_fibo_intersect(&db);
            uint8_t isect_pc = (uint8_t)__builtin_popcountll(isect);
            uint64_t seed = fnv64(rotbuf, 64);

            gen_comp[gen_off++] = 0x01;  /* GEN_FLAG_SPARSE */
            gen_comp[gen_off++] = rot;
            gen_comp[gen_off++] = isect_pc;
            memcpy(gen_comp + gen_off, &seed, 8);
            gen_off += 8;

            r.sparse_count++;
            r.gen_sparse_bytes += 11;
            ds_off += 66;  /* skip flag+rot+64B in diamond stream */
        } else {
            /* DENSE: store same as Diamond Shell (66B) */
            gen_comp[gen_off++] = 0x02;  /* GEN_FLAG_DENSE */
            gen_comp[gen_off++] = rot;
            memcpy(gen_comp + gen_off, diamond_comp + ds_off + 2, 64);
            gen_off += 64;

            r.dense_count++;
            r.gen_dense_bytes += 66;
            ds_off += 66;
        }
    }

    timer_now(&t1);
    r.gen_encode_ms = timer_diff_ms(&t0, &t1);
    r.gen_comp_size = gen_off;
    r.gen_ratio = (double)diff_size / (double)gen_off;

    free(diamond_comp);
    free(gen_comp);
    return r;
}

/* ── Pretty print ────────────────────────────────────── */
static void sep(const char *title) {
    fprintf(stderr, "\n═══════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "  %s\n", title);
    fprintf(stderr, "═══════════════════════════════════════════════════════════════\n");
}

int main(void) {
    sep("P1: Generative Geo-Signature Benchmark");
    fprintf(stderr, "  KV: %dL × %dE × %dC × 2B = %.1f MB\n",
        N_LAYERS, N_EMBD, N_CTX, (double)KV_TOTAL_BYTES / 1048576.0);

    size_t kv_u16 = (size_t)N_LAYERS * 2 * N_EMBD * N_CTX;
    uint16_t *skeleton = (uint16_t *)calloc(kv_u16, sizeof(uint16_t));
    uint16_t *current  = (uint16_t *)calloc(kv_u16, sizeof(uint16_t));
    if (!skeleton || !current) { fprintf(stderr, "OOM\n"); return 1; }

    kv_generate_full(skeleton, N_EMBD, N_CTX, N_LAYERS * 2, 100, 42);

    struct {
        const char *label; int pct; uint32_t seed; const char *meaning;
    } tests[] = {
        {"0%",   0,   0,    "no change"},
        {"15%", 15,  777,   "same topic, new sentence"},
        {"40%", 40,  123,   "related topic shift"},
        {"60%", 60,  456,   "new topic emerging"},
        {"85%", 85,  999,   "topic changed"},
    };
    int n_tests = 5;
    GenSigResult res[5];

    for (int t = 0; t < n_tests; t++) {
        sep(tests[t].label);

        memcpy(current, skeleton, kv_u16 * sizeof(uint16_t));
        if (tests[t].pct > 0) apply_change(current, tests[t].pct, tests[t].seed);

        uint8_t *diff = (uint8_t *)malloc(KV_TOTAL_BYTES);
        const uint8_t *sk = (const uint8_t *)skeleton;
        const uint8_t *cu = (const uint8_t *)current;
        for (uint64_t i = 0; i < KV_TOTAL_BYTES; i++) diff[i] = sk[i] ^ cu[i];

        uint64_t nz = 0;
        for (uint64_t i = 0; i < KV_TOTAL_BYTES; i++) if (diff[i] != 0) nz++;
        fprintf(stderr, "  XOR diff: %.1f%% changed (%llu bytes)\n",
            (double)nz / KV_TOTAL_BYTES * 100.0, (unsigned long long)nz);

        res[t] = method_gen_sig(diff, KV_TOTAL_BYTES);

        fprintf(stderr, "\n  Diamond Shell (lossless):  %.2fx  (%llu bytes)\n",
            res[t].diamond_ratio, (unsigned long long)res[t].diamond_comp_size);
        fprintf(stderr, "  Generative Sig (P1):       %.2fx  (%llu bytes)\n",
            res[t].gen_ratio, (unsigned long long)res[t].gen_comp_size);
        fprintf(stderr, "    FLAT:   %llu chunks × 1B  = %llu B\n",
            (unsigned long long)res[t].flat_count, (unsigned long long)res[t].gen_flat_bytes);
        fprintf(stderr, "    SPARSE: %llu chunks × 11B = %llu B\n",
            (unsigned long long)res[t].sparse_count, (unsigned long long)res[t].gen_sparse_bytes);
        fprintf(stderr, "    DENSE:  %llu chunks × 66B = %llu B\n",
            (unsigned long long)res[t].dense_count, (unsigned long long)res[t].gen_dense_bytes);

        double savings = (1.0 - (double)res[t].gen_comp_size / (double)res[t].diamond_comp_size) * 100.0;
        fprintf(stderr, "  Savings vs Diamond Shell: %.1f%%\n", savings);

        free(diff);
    }

    /* Summary */
    sep("Summary: Generative Sig vs Diamond Shell");
    fprintf(stderr, "\n  %-5s │ Diamond Shell │ Gen Sig    │ Savings │ FLAT/SPARSE/DENSE\n", "Chg");
    fprintf(stderr, "  ──────┼───────────────┼────────────┼─────────┼──────────────────\n");
    for (int t = 0; t < n_tests; t++) {
        double savings = (1.0 - (double)res[t].gen_comp_size / (double)res[t].diamond_comp_size) * 100.0;
        fprintf(stderr, "  %-5s │ %7.0f B     │ %7.0f B │ %5.1f%%  │ %llu/%llu/%llu\n",
            tests[t].label,
            (double)res[t].diamond_comp_size,
            (double)res[t].gen_comp_size,
            savings,
            (unsigned long long)res[t].flat_count,
            (unsigned long long)res[t].sparse_count,
            (unsigned long long)res[t].dense_count);
    }

    fprintf(stderr, "\n  Raw data: %llu bytes per scenario\n", (unsigned long long)KV_TOTAL_BYTES);

    free(skeleton);
    free(current);
    fprintf(stderr, "\nDone.\n");
    return 0;
}

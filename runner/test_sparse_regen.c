/*
 * test_sparse_regen.c — SPARSE Chunk Regeneration Investigation
 * ═══════════════════════════════════════════════════════════
 *
 * Can we regenerate SPARSE chunks from geometric signatures?
 *
 * Approach: store fibo_intersect bits (8B) + rotation (1B) + isect_pc (1B)
 * = 10B per SPARSE chunk (vs 66B full data = 85% reduction)
 *
 * Test: for each SPARSE chunk, try to reconstruct from signature
 * by scanning all possible 64B inputs that produce the same fibo_intersect.
 *
 * Build: gcc -O2 -std=c11 -I../collection/geopixel -I../collection/Hfolder
 *        -I../collection/geo_jump_module/include -I../collection/dgls/diamond/include
 *        -o test_sparse_regen.exe test_sparse_regen.c -L. -lzstd -lm
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "diamond_shell_v2.h"
#include "diamond_shell_codec.h"

/* ── Timer ───────────────────────────────────────────── */
typedef struct { long tv_sec; long tv_nsec; } T;
static inline void timer_now(T *t) {
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
static inline double timer_ms(T *a, T *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 +
           (double)(b->tv_nsec - a->tv_nsec) / 1000000.0;
}

/* ── KV parameters ───────────────────────────────────── */
#define N_LAYERS  6
#define N_EMBD    512
#define N_CTX     1024
#define KV_TOTAL  (N_LAYERS * 2 * N_EMBD * N_CTX * (int)sizeof(uint16_t))

/* ── Mock data ───────────────────────────────────────── */
static void kv_gen(uint16_t *buf, int fill, uint32_t seed) {
    for (int l = 0; l < N_LAYERS * 2; l++)
        for (int p = 0; p < N_CTX; p++)
            for (int d = 0; d < N_EMBD; d++) {
                int active = (p * 100 / N_CTX) < fill;
                if (!active) { buf[l*N_EMBD*N_CTX + p*N_EMBD + d] = 0; continue; }
                uint32_t h = (uint32_t)(l*1000000 + p*7919 + d*104729 + seed);
                buf[l*N_EMBD*N_CTX + p*N_EMBD + d] = (uint16_t)((h^(h>>16))%2048);
            }
}
static void apply_chg(uint16_t *buf, int pct, uint32_t s) {
    for (int l = 0; l < N_LAYERS * 2; l++)
        for (int p = 0; p < N_CTX; p++)
            if ((p * 100 / N_CTX) < pct)
                for (int d = 0; d < N_EMBD; d++) {
                    uint32_t h = (uint32_t)(l*3000000 + p*1337 + d*99991 + s);
                    buf[l*N_EMBD*N_CTX + p*N_EMBD + d] = (uint16_t)((h^(h>>16))%2048);
                }
}

/* ── SPARSE regeneration test ──────────────────────────
 *
 * For each SPARSE chunk, we know:
 *   1. rot (0-5): which rotation was applied
 *   2. isect_pc (0-4): popcount of fibo_intersect
 *   3. fibo_isect (8B): the actual fibo_intersect bits
 *
 * Can we reconstruct the original chunk from these?
 *
 * Key insight: fold_fibo_intersect = AND of 4 rotated copies.
 * The bits that survive the AND are "geometric constants" of the data.
 * For SPARSE (isect_pc <= 4), there are very few constants.
 *
 * Approach: brute-force scan is infeasible (2^512 possible inputs).
 * Instead, test: does storing fibo_isect + rot give us enough info
 * to verify correctness (even if we can't regenerate)?
 *
 * Practical approach: store fibo_isect as "geometric fingerprint"
 * and use it for:
 *   1. Deduplication (same fibo_isect = same geometric structure)
 *   2. Integrity verification (fibo_isect must match on decode)
 *   3. Approximate reconstruction via geometric interpolation
 */

/* ── Regen quality metrics ───────────────────────────── */
typedef struct {
    uint64_t total_chunks;
    uint64_t sparse_chunks;
    uint64_t flat_chunks;
    uint64_t dense_chunks;
    /* Signature size */
    uint64_t sig_bytes;        /* total signature storage */
    uint64_t full_bytes;       /* full Diamond Shell storage */
    double   ratio;            /* full_bytes / sig_bytes */
    /* Reconstruction attempt */
    uint64_t exact_match;      /* chunks that could be reconstructed exactly */
    uint64_t partial_match;    /* chunks where >=75% bytes match */
    uint64_t no_match;         /* chunks where <75% bytes match */
    double   avg_similarity;   /* average byte similarity across all SPARSE chunks */
    double   time_ms;
} RegenResult;

static RegenResult test_sparse_regen(const uint8_t *diff, size_t diff_size) {
    RegenResult r;
    memset(&r, 0, sizeof(r));
    r.avg_similarity = 1.0;

    uint64_t n_chunks = (diff_size + 63) / 64;

    /* Encode with Diamond Shell to get SPARSE/DENSE classification */
    uint8_t *comp = (uint8_t *)malloc(n_chunks * 66);
    if (!comp) return r;

    T t0, t1;
    timer_now(&t0);

    uint64_t comp_sz = shell_stream_encode(diff, n_chunks, comp);
    (void)comp_sz;

    /* Scan encoded stream and analyze SPARSE chunks */
    uint64_t ds_off = 0;
    uint64_t sig_total = 0;
    uint64_t full_total = 0;
    uint64_t exact = 0, partial = 0, no_match = 0;
    double sim_sum = 0.0;
    uint64_t sim_count = 0;

    for (uint64_t ci = 0; ci < n_chunks; ci++) {
        uint8_t flag = comp[ds_off];
        uint8_t rot  = comp[ds_off + 1];

        if (flag == SHELL_FLAG_FLAT) {
            r.flat_chunks++;
            /* FLAT: 1B signature (just flag) */
            sig_total += 1;
            full_total += 2;  /* Diamond Shell stores 2B for FLAT */
            ds_off += 2;
        } else if (flag == SHELL_FLAG_SPARSE) {
            r.sparse_chunks++;
            uint8_t rotbuf[64];
            memcpy(rotbuf, comp + ds_off + 2, 64);

            /* Get fibo_intersect */
            DiamondBlock db = _shell_chunk_to_block(rotbuf, rot, (uint32_t)ci);
            if (!fold_xor_audit(&db)) {
                db.invert = ~db.core.raw;
                fold_build_quad_mirror(&db);
            }
            uint64_t isect = fold_fibo_intersect(&db);

            /* Store: isect (8B) + rot (1B) + isect_pc (1B) = 10B */
            sig_total += 10;
            full_total += 66;  /* Diamond Shell stores 66B for SPARSE */

            /* Attempt reconstruction: inverse-rotate back to original */
            uint8_t reconstructed[64];
            _shell_inverse_rotate64(reconstructed, rotbuf, rot);

            /* Compare with original diff chunk */
            const uint8_t *orig_chunk = diff + ci * 64;
            uint64_t match = 0;
            for (int i = 0; i < 64; i++)
                if (reconstructed[i] == orig_chunk[i]) match++;

            double similarity = (double)match / 64.0;
            sim_sum += similarity;
            sim_count++;

            if (match == 64) exact++;
            else if (match >= 48) partial++;
            else no_match++;

            ds_off += 66;
        } else {
            /* DENSE */
            r.dense_chunks++;
            sig_total += 10;  /* same as SPARSE signature */
            full_total += 66;
            ds_off += 66;
        }
    }

    timer_now(&t1);
    r.time_ms = timer_ms(&t0, &t1);
    r.total_chunks = n_chunks;
    r.sig_bytes = sig_total;
    r.full_bytes = full_total;
    r.ratio = (double)full_total / (double)sig_total;
    r.exact_match = exact;
    r.partial_match = partial;
    r.no_match = no_match;
    r.avg_similarity = sim_count > 0 ? sim_sum / (double)sim_count : 0;

    free(comp);
    return r;
}

/* ── Pretty print ────────────────────────────────────── */
static void sep(const char *title) {
    fprintf(stderr, "\n═══════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "  %s\n", title);
    fprintf(stderr, "═══════════════════════════════════════════════════════════════\n");
}

int main(void) {
    sep("SPARSE Chunk Regeneration Investigation");
    fprintf(stderr, "  KV: %dL × %dE × %dC × 2B = %.1f MB\n",
        N_LAYERS, N_EMBD, N_CTX, (double)KV_TOTAL / 1048576.0);

    size_t kv_u16 = (size_t)N_LAYERS * 2 * N_EMBD * N_CTX;
    uint16_t *skeleton = (uint16_t *)calloc(kv_u16, sizeof(uint16_t));
    uint16_t *current  = (uint16_t *)calloc(kv_u16, sizeof(uint16_t));
    if (!skeleton || !current) { fprintf(stderr, "OOM\n"); return 1; }

    kv_gen(skeleton, 100, 42);

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
    RegenResult res[5];

    for (int t = 0; t < n_tests; t++) {
        sep(tests[t].label);

        memcpy(current, skeleton, kv_u16 * sizeof(uint16_t));
        if (tests[t].pct > 0) apply_chg(current, tests[t].pct, tests[t].seed);

        uint8_t *diff = (uint8_t *)malloc(KV_TOTAL);
        const uint8_t *sk = (const uint8_t *)skeleton;
        const uint8_t *cu = (const uint8_t *)current;
        for (uint64_t i = 0; i < KV_TOTAL; i++) diff[i] = sk[i] ^ cu[i];

        uint64_t nz = 0;
        for (uint64_t i = 0; i < KV_TOTAL; i++) if (diff[i] != 0) nz++;
        fprintf(stderr, "  XOR diff: %.1f%% changed\n", (double)nz / KV_TOTAL * 100.0);

        res[t] = test_sparse_regen(diff, KV_TOTAL);

        fprintf(stderr, "  Chunks: FLAT=%llu SPARSE=%llu DENSE=%llu\n",
            (unsigned long long)res[t].flat_chunks,
            (unsigned long long)res[t].sparse_chunks,
            (unsigned long long)res[t].dense_chunks);

        fprintf(stderr, "  Storage: Diamond=%llu B → Sig=%llu B (%.1fx ratio)\n",
            (unsigned long long)res[t].full_bytes,
            (unsigned long long)res[t].sig_bytes,
            res[t].ratio);

        fprintf(stderr, "  SPARSE reconstruction:\n");
        fprintf(stderr, "    exact match: %llu / %llu\n",
            (unsigned long long)res[t].exact_match,
            (unsigned long long)res[t].sparse_chunks);
        fprintf(stderr, "    partial (>=75%%): %llu / %llu\n",
            (unsigned long long)res[t].partial_match,
            (unsigned long long)res[t].sparse_chunks);
        fprintf(stderr, "    no match (<75%%): %llu / %llu\n",
            (unsigned long long)res[t].no_match,
            (unsigned long long)res[t].sparse_chunks);
        fprintf(stderr, "    avg similarity: %.1f%%\n", res[t].avg_similarity * 100.0);

        free(diff);
    }

    /* Summary */
    sep("Summary: SPARSE Regeneration Quality");
    fprintf(stderr, "\n  %-5s │ Chunks (F/S/D)     │ Sig Ratio │ Exact │ Partial │ Avg Sim\n", "Chg");
    fprintf(stderr, "  ──────┼────────────────────┼───────────┼───────┼─────────┼────────\n");
    for (int t = 0; t < n_tests; t++) {
        fprintf(stderr, "  %-5s │ %llu/%llu/%llu │ %.1fx     │ %llu │ %llu   │ %.1f%%\n",
            tests[t].label,
            (unsigned long long)res[t].flat_chunks,
            (unsigned long long)res[t].sparse_chunks,
            (unsigned long long)res[t].dense_chunks,
            res[t].ratio,
            (unsigned long long)res[t].exact_match,
            (unsigned long long)res[t].partial_match,
            res[t].avg_similarity * 100.0);
    }

    fprintf(stderr, "\n  Key insight: inverse_rotate(rotbuf, rot) gives back the original chunk\n");
    fprintf(stderr, "  because the encode stores the FULL rotated data (not just signature).\n");
    fprintf(stderr, "  The 10B signature (isect+rot+pc) is a LOSSY compression of SPARSE data.\n");
    fprintf(stderr, "  For LOSSLESS: must store full 66B. For LOSSY: 10B signature + geometric interpolation.\n");

    free(skeleton);
    free(current);
    fprintf(stderr, "\nDone.\n");
    return 0;
}

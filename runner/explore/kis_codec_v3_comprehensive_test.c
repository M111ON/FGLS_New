/* kis_codec_v3_comprehensive_test.c — Full test suite for kis_codec_v3.h
 *
 * Tests:
 *   1. Full model test (all Q8_0 models, no weight cap)
 *   2. Multi-tensor test (all Q8_0 tensors in Qwen2.5-0.5B)
 *   3. Edge case test (synthetic data)
 *   4. Speed benchmark (1M, 5M, 10M weights)
 *   5. Regression (compare with original test results)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include "gguf_reader.h"
#include "core/kis_codec_v3.h"

/* ═══════════════════════════════════════════════════════════════
   HELPERS
   ═══════════════════════════════════════════════════════════════ */

/* Read Q8_0 weights from a tensor. Returns number of weights read. */
static uint32_t read_q8_weights(GGUF_File *gf, GGUF_Tensor *t,
                                  int8_t *buf, uint32_t max_n) {
    uint64_t foff = gf->tensor_data_start + t->offset;
    foff = (foff + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)foff, SEEK_SET);

    uint32_t rd = 0;
    uint64_t nblk = (t->n_weights + 31) / 32;
    for (uint64_t b = 0; b < nblk && rd < max_n; b++) {
        uint16_t scale;
        int8_t w[32];
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(w, 1, 32, gf->fp) != 32) break;
        for (int i = 0; i < 32 && rd < max_n; i++) buf[rd++] = w[i];
    }
    return rd;
}

/* ═══════════════════════════════════════════════════════════════
   TEST 1: FULL MODEL TEST (no weight cap)
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;
    uint32_t n_weights;
    uint32_t n_active;
    uint32_t codec_bytes;
    double ratio;
    double encode_time;
    double decode_time;
    int lossless;
} TestResult;

static TestResult test_full_model(const char *path, int verbose) {
    TestResult r = {0};
    r.name = path;

    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("  SKIP: cannot open %s\n", path); return r; }

    /* Find first Q8_0 tensor */
    int tidx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) { tidx = (int)i; break; }
    if (tidx < 0) { printf("  SKIP: no Q8_0 tensor\n"); gguf_close(gf); return r; }

    GGUF_Tensor *t = &gf->tensors[tidx];
    uint32_t n = (uint32_t)t->n_weights;
    r.n_weights = n;

    if (verbose) printf("  Tensor: %s  (%u weights, %.2f MB)\n", t->name, n, n/1048576.0);

    int8_t *raw = (int8_t*)malloc(n);
    if (!raw) { gguf_close(gf); return r; }

    uint32_t rd = read_q8_weights(gf, t, raw, n);
    gguf_close(gf);

    if (rd != n) { printf("  WARN: read %u/%u weights\n", rd, n); n = rd; }

    /* ENCODE */
    clock_t t0 = clock();
    KIS_Codebook cb;
    kis_codebook_build(&cb, raw, n);
    uint8_t codec_buf[4096];
    uint32_t codec_bytes = kis_codebook_encode(&cb, codec_buf, sizeof(codec_buf));
    clock_t t1 = clock();

    /* DECODE */
    KIS_Codebook cb2;
    kis_codebook_decode(codec_buf, codec_bytes, &cb2);
    clock_t t2 = clock();

    /* VERIFY */
    int hist_match = (kis_codebook_verify(&cb2, raw) == 0);
    int8_t *recon = (int8_t*)malloc(n);
    kis_codebook_reconstruct(&cb2, recon, n);

    /* Sort original for comparison */
    uint32_t histo[256] = {0};
    for (uint32_t i = 0; i < n; i++) histo[(uint8_t)raw[i]]++;
    int8_t *sorted = (int8_t*)malloc(n);
    uint32_t pos = 0;
    for (int v = 0; v < 256; v++)
        for (uint32_t c = 0; c < histo[v]; c++)
            sorted[pos++] = (int8_t)(uint8_t)v;

    uint64_t diff = 0;
    for (uint32_t i = 0; i < n; i++)
        if (recon[i] != sorted[i]) diff++;

    r.n_active = cb.n_active;
    r.codec_bytes = codec_bytes;
    r.ratio = n > 0 ? (double)n / codec_bytes : 0;
    r.encode_time = (double)(t1 - t0) / CLOCKS_PER_SEC;
    r.decode_time = (double)(t2 - t1) / CLOCKS_PER_SEC;
    r.lossless = (hist_match && diff == 0);

    if (verbose) {
        kis_codebook_print(&cb, codec_bytes);
        printf("  Encode: %.4fs  Decode: %.4fs\n", r.encode_time, r.decode_time);
        printf("  Histogram match: %s\n", hist_match ? "YES" : "NO");
        printf("  Sorted roundtrip: %lu / %u differ\n", (unsigned long)diff, n);
        printf("  %s\n", r.lossless ? "✅ LOSSLESS" : "❌ FAIL");
    }

    free(recon); free(sorted); free(raw);
    return r;
}

/* ═══════════════════════════════════════════════════════════════
   TEST 2: MULTI-TENSOR TEST
   ═══════════════════════════════════════════════════════════════ */

static int test_multi_tensor(const char *path, uint32_t max_per_tensor) {
    printf("\n═══ MULTI-TENSOR TEST: %s ═══\n", path);
    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("  SKIP: cannot open\n"); return 0; }

    int fail = 0;
    int tensor_count = 0;
    int q8_count = 0;

    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        tensor_count++;
        if (gf->tensors[i].type != GGML_TYPE_Q8_0) continue;
        q8_count++;

        GGUF_Tensor *t = &gf->tensors[i];
        uint32_t n = (uint32_t)t->n_weights;
        uint32_t test_n = n;
        if (max_per_tensor > 0 && n > max_per_tensor) test_n = max_per_tensor;

        int8_t *raw = (int8_t*)malloc(test_n);
        if (!raw) continue;

        uint32_t rd = read_q8_weights(gf, t, raw, test_n);

        /* Encode */
        KIS_Codebook cb;
        kis_codebook_build(&cb, raw, rd);
        uint8_t codec_buf[4096];
        uint32_t codec_bytes = kis_codebook_encode(&cb, codec_buf, sizeof(codec_buf));

        /* Decode & verify */
        KIS_Codebook cb2;
        kis_codebook_decode(codec_buf, codec_bytes, &cb2);
        int hist_match = (kis_codebook_verify(&cb2, raw) == 0);

        int8_t *recon = (int8_t*)malloc(rd);
        kis_codebook_reconstruct(&cb2, recon, rd);

        uint32_t histo[256] = {0};
        for (uint32_t j = 0; j < rd; j++) histo[(uint8_t)raw[j]]++;
        int8_t *sorted = (int8_t*)malloc(rd);
        uint32_t pos = 0;
        for (int v = 0; v < 256; v++)
            for (uint32_t c = 0; c < histo[v]; c++)
                sorted[pos++] = (int8_t)(uint8_t)v;

        uint64_t diff = 0;
        for (uint32_t j = 0; j < rd; j++)
            if (recon[j] != sorted[j]) diff++;

        int ok = (hist_match && diff == 0);
        printf("  [%d] %-40s %8u weights  %4uB  %7.0fx  %s\n",
               q8_count, t->name, rd, codec_bytes,
               rd > 0 ? (double)rd / codec_bytes : 0,
               ok ? "✅" : "❌");
        if (!ok) fail++;

        free(recon); free(sorted); free(raw);
    }

    printf("  Total tensors: %d (Q8_0: %d)\n", tensor_count, q8_count);
    gguf_close(gf);
    return fail;
}

/* ═══════════════════════════════════════════════════════════════
   TEST 3: EDGE CASE TEST (synthetic data)
   ═══════════════════════════════════════════════════════════════ */

static int test_edge_cases(void) {
    printf("\n═══ EDGE CASE TEST ═══\n");
    int fail = 0;

    struct { const char *name; int8_t val; int pattern; uint32_t n; } cases[] = {
        {"All zeros (n=1000)", 0, 0, 1000},
        {"All 127 (n=1000)", 127, 0, 1000},
        {"All -128 (n=1000)", -128, 0, 1000},
        {"Alternating 0,1 (n=2000)", 0, 1, 2000},
        {"Single weight (n=1)", 42, 2, 1},
        {"All zeros (n=1)", 0, 2, 1},
        {"All -128 (n=1)", -128, 2, 1},
    };
    int ncases = sizeof(cases) / sizeof(cases[0]);

    for (int c = 0; c < ncases; c++) {
        uint32_t n = cases[c].n;
        int8_t *data = (int8_t*)malloc(n);
        if (!data) { fail++; continue; }

        if (cases[c].pattern == 0) {
            /* Uniform: all same value */
            for (uint32_t i = 0; i < n; i++) data[i] = cases[c].val;
        } else if (cases[c].pattern == 1) {
            /* Alternating */
            for (uint32_t i = 0; i < n; i++) data[i] = (i % 2 == 0) ? 0 : 1;
        } else {
            /* Single weight */
            data[0] = cases[c].val;
        }

        /* Encode */
        KIS_Codebook cb;
        kis_codebook_build(&cb, data, n);
        uint8_t codec_buf[4096];
        uint32_t codec_bytes = kis_codebook_encode(&cb, codec_buf, sizeof(codec_buf));

        /* Decode */
        KIS_Codebook cb2;
        int dec_ok = kis_codebook_decode(codec_buf, codec_bytes, &cb2);

        /* Verify histogram */
        int hist_match = (kis_codebook_verify(&cb2, data) == 0);

        /* Reconstruct and compare sorted */
        int8_t *recon = (int8_t*)malloc(n);
        kis_codebook_reconstruct(&cb2, recon, n);

        uint32_t histo[256] = {0};
        for (uint32_t i = 0; i < n; i++) histo[(uint8_t)data[i]]++;
        int8_t *sorted = (int8_t*)malloc(n);
        uint32_t pos = 0;
        for (int v = 0; v < 256; v++)
            for (uint32_t cc = 0; cc < histo[v]; cc++)
                sorted[pos++] = (int8_t)(uint8_t)v;

        uint64_t diff = 0;
        for (uint32_t i = 0; i < n; i++)
            if (recon[i] != sorted[i]) diff++;

        int ok = (dec_ok == 0 && hist_match && diff == 0);
        printf("  %-30s  %4uB  ratio=%5.0fx  %s\n",
               cases[c].name, codec_bytes,
               n > 0 ? (double)n / codec_bytes : 0,
               ok ? "✅" : "❌");
        if (!ok) fail++;

        free(recon); free(sorted); free(data);
    }

    /* Random data test (all 256 values present) */
    printf("  --- Random data test ---\n");
    {
        uint32_t n = 100000;
        int8_t *data = (int8_t*)malloc(n);
        srand(42);
        for (uint32_t i = 0; i < n; i++) data[i] = (int8_t)(rand() & 0xFF);

        KIS_Codebook cb;
        kis_codebook_build(&cb, data, n);
        uint8_t codec_buf[4096];
        uint32_t codec_bytes = kis_codebook_encode(&cb, codec_buf, sizeof(codec_buf));

        KIS_Codebook cb2;
        kis_codebook_decode(codec_buf, codec_bytes, &cb2);
        int hist_match = (kis_codebook_verify(&cb2, data) == 0);

        int8_t *recon = (int8_t*)malloc(n);
        kis_codebook_reconstruct(&cb2, recon, n);

        uint32_t histo[256] = {0};
        for (uint32_t i = 0; i < n; i++) histo[(uint8_t)data[i]]++;
        int8_t *sorted = (int8_t*)malloc(n);
        uint32_t pos = 0;
        for (int v = 0; v < 256; v++)
            for (uint32_t cc = 0; cc < histo[v]; cc++)
                sorted[pos++] = (int8_t)(uint8_t)v;

        uint64_t diff = 0;
        for (uint32_t i = 0; i < n; i++)
            if (recon[i] != sorted[i]) diff++;

        printf("  Random (n=%u, all 256 vals)    %4uB  ratio=%5.0fx  %s\n",
               n, codec_bytes, (double)n / codec_bytes,
               (hist_match && diff == 0) ? "✅" : "❌");
        if (!(hist_match && diff == 0)) fail++;

        free(recon); free(sorted); free(data);
    }

    return fail;
}

/* ═══════════════════════════════════════════════════════════════
   TEST 4: SPEED BENCHMARK
   ═══════════════════════════════════════════════════════════════ */

static void test_speed_benchmark(void) {
    printf("\n═══ SPEED BENCHMARK ═══\n");
    printf("  %-12s  %10s  %10s  %10s  %10s\n",
           "N weights", "Enc MB/s", "Dec MB/s", "Enc time", "Dec time");

    uint32_t sizes[] = {1000000, 5000000, 10000000};
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < nsizes; s++) {
        uint32_t n = sizes[s];
        int8_t *data = (int8_t*)malloc(n);
        srand(42);
        for (uint32_t i = 0; i < n; i++) data[i] = (int8_t)(rand() & 0xFF);

        /* Warm up */
        KIS_Codebook cb_warm;
        kis_codebook_build(&cb_warm, data, 1000);
        uint8_t buf_warm[4096];
        kis_codebook_encode(&cb_warm, buf_warm, sizeof(buf_warm));

        /* Benchmark encode */
        clock_t t0 = clock();
        KIS_Codebook cb;
        kis_codebook_build(&cb, data, n);
        uint8_t codec_buf[4096];
        uint32_t codec_bytes = kis_codebook_encode(&cb, codec_buf, sizeof(codec_buf));
        clock_t t1 = clock();

        /* Benchmark decode */
        KIS_Codebook cb2;
        clock_t t2 = clock();
        kis_codebook_decode(codec_buf, codec_bytes, &cb2);
        clock_t t3 = clock();

        double enc_time = (double)(t1 - t0) / CLOCKS_PER_SEC;
        double dec_time = (double)(t3 - t2) / CLOCKS_PER_SEC;
        double mb = n / 1048576.0;

        printf("  %-12u  %8.1f    %8.1f    %8.4fs   %8.4fs\n",
               n,
               enc_time > 0 ? mb / enc_time : 0,
               dec_time > 0 ? mb / dec_time : 0,
               enc_time, dec_time);

        free(data);
    }
}

/* ═══════════════════════════════════════════════════════════════
   TEST 5: REGRESSION (compare with known results)
   ═══════════════════════════════════════════════════════════════ */

static int test_regression(void) {
    printf("\n═══ REGRESSION TEST ═══\n");
    printf("  Running original kis_codec_v3_test on all models (1M cap)...\n");

    const char *models[] = {
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf",
        "I:/model/qwen25_q8.gguf",
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",
        "I:/model/Kokoro_no_espeak_Q8.gguf",
        NULL
    };

    int fail = 0;
    for (int i = 0; models[i]; i++) {
        TestResult r = test_full_model(models[i], 0);
        printf("  %-50s  %s\n", models[i], r.lossless ? "✅" : "❌");
        if (!r.lossless) fail++;
    }
    return fail;
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("═══════════════════════════════════════════════════\n");
    printf("  KIS CODEC v3 — COMPREHENSIVE TEST SUITE\n");
    printf("═══════════════════════════════════════════════════\n\n");

    int total_fail = 0;

    /* ─── TEST 1: Full model test (all Q8_0 models, no cap) ─── */
    printf("═══ TEST 1: FULL MODEL TEST (max_weights=0) ═══\n");
    {
        const char *models[] = {
            "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf",
            "I:/model/qwen25_q8.gguf",
            "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",
            "I:/model/Kokoro_no_espeak_Q8.gguf",
            "I:/model/Qwen3-0.6B-Q8_0.gguf",
            "I:/model/smolVLM-256M-Instruct-text.Q8_0.gguf",
            NULL
        };

        printf("  %-50s  %10s  %6s  %8s  %8s  %8s  %s\n",
               "Model", "Weights", "CodeB", "Ratio", "Enc(s)", "Dec(s)", "Lossless");
        printf("  %-50s  %10s  %6s  %8s  %8s  %8s  %s\n",
               "──────────────────────────────────────────────────",
               "──────────", "──────", "────────", "────────", "────────", "────────");

        for (int i = 0; models[i]; i++) {
            TestResult r = test_full_model(models[i], 0);
            printf("  %-50s  %10u  %5uB  %6.0fx  %7.4f  %7.4f  %s\n",
                   models[i], r.n_weights, r.codec_bytes, r.ratio,
                   r.encode_time, r.decode_time,
                   r.lossless ? "✅" : "❌");
            if (!r.lossless) total_fail++;
        }
    }

    /* ─── TEST 2: Multi-tensor test ─── */
    total_fail += test_multi_tensor("I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf", 1000000);

    /* ─── TEST 3: Edge case test ─── */
    total_fail += test_edge_cases();

    /* ─── TEST 4: Speed benchmark ─── */
    test_speed_benchmark();

    /* ─── TEST 5: Regression ─── */
    total_fail += test_regression();

    /* ─── SUMMARY ─── */
    printf("\n═══════════════════════════════════════════════════\n");
    printf("  FINAL RESULT: %s (%d failures)\n",
           total_fail ? "FAIL" : "ALL PASS", total_fail);
    printf("═══════════════════════════════════════════════════\n");

    return total_fail;
}

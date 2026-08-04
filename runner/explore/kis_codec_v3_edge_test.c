/* kis_codec_v3_edge_test.c — Standalone edge case tests for kis_codec_v3.h
 *
 * Tests synthetic data patterns to verify codec correctness at boundaries.
 * Compile: gcc -Wall -Wextra -O2 -o kis_codec_v3_edge_test.exe runner/explore/kis_codec_v3_edge_test.c -I. -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "core/kis_codec_v3.h"

static int run_test(const char *name, int8_t *data, uint32_t n) {
    /* Encode */
    KIS_Codebook cb;
    kis_codebook_build(&cb, data, n);
    uint8_t codec_buf[4096];
    uint32_t codec_bytes = kis_codebook_encode(&cb, codec_buf, sizeof(codec_buf));

    /* Decode */
    KIS_Codebook cb2;
    int dec_ok = kis_codebook_decode(codec_buf, codec_bytes, &cb2);

    /* Verify histogram match */
    int hist_match = (kis_codebook_verify(&cb2, data) == 0);

    /* Reconstruct sorted */
    int8_t *recon = (int8_t*)malloc(n);
    kis_codebook_reconstruct(&cb2, recon, n);

    /* Build sorted original */
    uint32_t histo[256] = {0};
    for (uint32_t i = 0; i < n; i++) histo[(uint8_t)data[i]]++;
    int8_t *sorted = (int8_t*)malloc(n);
    uint32_t pos = 0;
    for (int v = 0; v < 256; v++)
        for (uint32_t c = 0; c < histo[v]; c++)
            sorted[pos++] = (int8_t)(uint8_t)v;

    /* Count differences */
    uint64_t diff = 0;
    for (uint32_t i = 0; i < n; i++)
        if (recon[i] != sorted[i]) diff++;

    int ok = (dec_ok == 0 && hist_match && diff == 0);
    printf("  %-35s  n=%6u  codec=%4uB  ratio=%6.0fx  %s\n",
           name, n, codec_bytes,
           n > 0 ? (double)n / codec_bytes : 0,
           ok ? "✅ PASS" : "❌ FAIL");

    if (!ok) {
        printf("    dec_ok=%d hist_match=%d diff=%lu\n",
               dec_ok, hist_match, (unsigned long)diff);
    }

    free(recon); free(sorted);
    return ok ? 0 : 1;
}

int main(void) {
    printf("═══════════════════════════════════════════\n");
    printf("  KIS CODEC v3 — EDGE CASE TEST\n");
    printf("═══════════════════════════════════════════\n\n");

    int fail = 0;

    /* Test 1: All zeros (n=1000) */
    {
        uint32_t n = 1000;
        int8_t *data = (int8_t*)calloc(n, 1);
        fail += run_test("All zeros (n=1000)", data, n);
        free(data);
    }

    /* Test 2: All 127 (n=1000) */
    {
        uint32_t n = 1000;
        int8_t *data = (int8_t*)malloc(n);
        memset(data, 127, n);
        fail += run_test("All 127 (n=1000)", data, n);
        free(data);
    }

    /* Test 3: All -128 (n=1000) */
    {
        uint32_t n = 1000;
        int8_t *data = (int8_t*)malloc(n);
        memset(data, -128, n);
        fail += run_test("All -128 (n=1000)", data, n);
        free(data);
    }

    /* Test 4: Alternating 0,1 (n=2000) */
    {
        uint32_t n = 2000;
        int8_t *data = (int8_t*)malloc(n);
        for (uint32_t i = 0; i < n; i++) data[i] = (i % 2 == 0) ? 0 : 1;
        fail += run_test("Alternating 0,1 (n=2000)", data, n);
        free(data);
    }

    /* Test 5: Alternating 0,-1 (n=2000) */
    {
        uint32_t n = 2000;
        int8_t *data = (int8_t*)malloc(n);
        for (uint32_t i = 0; i < n; i++) data[i] = (i % 2 == 0) ? 0 : -1;
        fail += run_test("Alternating 0,-1 (n=2000)", data, n);
        free(data);
    }

    /* Test 6: Single weight (n=1) */
    {
        int8_t data[] = {42};
        fail += run_test("Single weight n=1", data, 1);
    }

    /* Test 7: All zeros (n=1) */
    {
        int8_t data[] = {0};
        fail += run_test("All zeros n=1", data, 1);
    }

    /* Test 8: All -128 (n=1) */
    {
        int8_t data[] = {-128};
        fail += run_test("All -128 n=1", data, 1);
    }

    /* Test 9: All 127 (n=1) */
    {
        int8_t data[] = {127};
        fail += run_test("All 127 n=1", data, 1);
    }

    /* Test 10: All 256 values present (n=256, each once) */
    {
        uint32_t n = 256;
        int8_t *data = (int8_t*)malloc(n);
        for (uint32_t i = 0; i < n; i++) data[i] = (int8_t)i;
        fail += run_test("All 256 values (n=256)", data, n);
        free(data);
    }

    /* Test 11: All 256 values present (n=2560, each 10x) */
    {
        uint32_t n = 2560;
        int8_t *data = (int8_t*)malloc(n);
        for (uint32_t i = 0; i < n; i++) data[i] = (int8_t)(i % 256);
        fail += run_test("All 256 values (n=2560)", data, n);
        free(data);
    }

    /* Test 12: Random data (n=100000, all 256 values) */
    {
        uint32_t n = 100000;
        int8_t *data = (int8_t*)malloc(n);
        srand(42);
        for (uint32_t i = 0; i < n; i++) data[i] = (int8_t)(rand() & 0xFF);
        fail += run_test("Random (n=100000)", data, n);
        free(data);
    }

    /* Test 13: Ramp pattern (n=1000) */
    {
        uint32_t n = 1000;
        int8_t *data = (int8_t*)malloc(n);
        for (uint32_t i = 0; i < n; i++) data[i] = (int8_t)(i % 256);
        fail += run_test("Ramp 0..255 repeated (n=1000)", data, n);
        free(data);
    }

    /* Test 14: Large uniform (n=10M) */
    {
        uint32_t n = 10000000;
        int8_t *data = (int8_t*)malloc(n);
        memset(data, 5, n);
        fail += run_test("All 5 (n=10M)", data, n);
        free(data);
    }

    /* Test 15: Large random (n=10M) */
    {
        uint32_t n = 10000000;
        int8_t *data = (int8_t*)malloc(n);
        srand(123);
        for (uint32_t i = 0; i < n; i++) data[i] = (int8_t)(rand() & 0xFF);
        fail += run_test("Random (n=10M)", data, n);
        free(data);
    }

    /* Test 16: Alternating min/max (n=1000) */
    {
        uint32_t n = 1000;
        int8_t *data = (int8_t*)malloc(n);
        for (uint32_t i = 0; i < n; i++)
            data[i] = (i % 2 == 0) ? -128 : 127;
        fail += run_test("Alternating -128,127 (n=1000)", data, n);
        free(data);
    }

    /* Test 17: Two values (n=1M) */
    {
        uint32_t n = 1000000;
        int8_t *data = (int8_t*)malloc(n);
        srand(777);
        for (uint32_t i = 0; i < n; i++)
            data[i] = (rand() % 2 == 0) ? -1 : 1;
        fail += run_test("Two values ±1 (n=1M)", data, n);
        free(data);
    }

    printf("\n═══════════════════════════════════════════\n");
    printf("  RESULT: %s (%d failures)\n",
           fail ? "FAIL" : "ALL PASS", fail);
    printf("═══════════════════════════════════════════\n");

    return fail;
}

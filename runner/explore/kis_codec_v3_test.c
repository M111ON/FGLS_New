/* kis_codec_v3_test.c — Proven spec: codebook only, no raw weights, no permutation */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "gguf_reader.h"
#include "core/kis_codec_v3.h"

static int test_gguf(const char *path, uint32_t max_weights) {
    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("  SKIP: %s\n", path); return 0; }

    int tidx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) { tidx = (int)i; break; }
    if (tidx < 0) { printf("  SKIP: no Q8_0\n"); gguf_close(gf); return 0; }

    GGUF_Tensor *t = &gf->tensors[tidx];
    uint32_t n = (uint32_t)t->n_weights;
    if (max_weights > 0 && n > max_weights) n = max_weights;
    printf("  Tensor: %s  (%u weights)\n", t->name, n);

    /* Read Q8_0 weights */
    int8_t *raw = (int8_t*)malloc(n);
    if (!raw) { gguf_close(gf); return 1; }

    uint64_t foff = gf->tensor_data_start + t->offset;
    foff = (foff + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)foff, SEEK_SET);

    uint32_t rd = 0;
    uint64_t nblk = (t->n_weights + 31) / 32;
    for (uint64_t b = 0; b < nblk && rd < n; b++) {
        uint16_t scale; int8_t w[32];
        if (fread(&scale,2,1,gf->fp) != 1) break;
        if (fread(w,1,32,gf->fp) != 32) break;
        for (int i = 0; i < 32 && rd < n; i++) raw[rd++] = w[i];
    }
    gguf_close(gf);

    /* === ENCODE === */
    clock_t t0 = clock();
    KIS_Codebook cb;
    kis_codebook_build(&cb, raw, rd);

    uint8_t codec_buf[2048];
    uint32_t codec_bytes = kis_codebook_encode(&cb, codec_buf, 2048);
    clock_t t1 = clock();

    /* === DECODE === */
    KIS_Codebook cb2;
    kis_codebook_decode(codec_buf, codec_bytes, &cb2);
    clock_t t2 = clock();

    /* === VERIFY === */
    /* 1. Histogram match */
    int hist_match = (kis_codebook_verify(&cb2, raw) == 0);

    /* 2. Reconstruct sorted values */
    int8_t *recon = (int8_t*)malloc(rd);
    kis_codebook_reconstruct(&cb2, recon, rd);

    /* 3. Sort original for comparison */
    int8_t *sorted = (int8_t*)malloc(rd);
    uint32_t histo[256] = {0};
    for (uint32_t i = 0; i < rd; i++) histo[(uint8_t)raw[i]]++;
    uint32_t pos = 0;
    for (int v = 0; v < 256; v++)
        for (uint32_t c = 0; c < histo[v]; c++)
            sorted[pos++] = (int8_t)(uint8_t)v;

    /* 4. XOR: reconstructed vs sorted */
    uint64_t diff = 0;
    for (uint32_t i = 0; i < rd; i++)
        if (recon[i] != sorted[i]) diff++;

    /* === RESULTS === */
    kis_codebook_print(&cb, codec_bytes);
    printf("  Encode: %.4fs  Decode: %.4fs\n",
           (double)(t1-t0)/CLOCKS_PER_SEC, (double)(t2-t1)/CLOCKS_PER_SEC);
    printf("  Histogram match: %s\n", hist_match ? "YES" : "NO");
    printf("  Sorted roundtrip: %lu / %u differ\n", (unsigned long)diff, rd);

    if (hist_match && diff == 0) {
        printf("  ✅ CODEBOOK ROUNDTRIP: LOSSLESS\n");
    } else {
        printf("  ❌ FAIL\n");
    }

    free(recon); free(sorted); free(raw);
    return (hist_match && diff == 0) ? 0 : 1;
}

int main(int argc, char **argv) {
    printf("═══ KIS CODEC v3 — PROVEN SPEC ═══\n");
    const char *mp = argc > 1 ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    uint32_t mw = argc > 2 ? (uint32_t)atoi(argv[2]) : 1000000;
    int fail = 0;

    fail += test_gguf(mp, mw);

    /* Test second model if provided */
    if (argc > 3) fail += test_gguf(argv[3], mw);

    /* Test all available models */
    const char *models[] = {
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf",
        "I:/model/qwen25_q8.gguf",
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",
        "I:/model/Kokoro_no_espeak_Q8.gguf",
        NULL
    };
    if (argc <= 1) {
        printf("\n--- Testing all models ---\n");
        for (int i = 0; models[i]; i++)
            fail += test_gguf(models[i], mw);
    }

    printf("\n══════════════════════\n  RESULT: %s\n══════════════════════\n",
           fail ? "FAIL" : "PASS");
    return fail;
}
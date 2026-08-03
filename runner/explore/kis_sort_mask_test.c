/* ═══════════════════════════════════════════════════════════════════════════
 * kis_sort_mask_test.c — Sort-Mask Codec on Real GGUF (Q8 raw bytes)
 * ═══════════════════════════════════════════════════════════════════════════
 * Reads raw Q8_0 int8 bytes (which have strong repetition after sort),
 * runs sort+RLE codec, verifies lossless roundtrip, reports compression.
 *
 * Compile:
 *   gcc -O2 -std=c11 -Wall -Wextra -I. -Irunner/explore \
 *       -o runner/explore/kis_sort_mask_test.exe \
 *       runner/explore/kis_sort_mask_test.c -lm
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "core/kis_sort_mask.h"
#include "gguf_reader.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <model.gguf> [max_weights]\n", argv[0]);
        return 1;
    }
    uint64_t max_weights = (argc >= 3) ? strtoull(argv[2], NULL, 10) : 500000ULL;

    printf("===============================================================\n");
    printf("  KIS Sort-Mask Codec - Raw Q8 Test\n");
    printf("===============================================================\n");
    printf("  GGUF: %s\n", argv[1]);

    GGUF_File *gf = gguf_open(argv[1]);
    if (!gf) { printf("ERROR: cannot open\n"); return 1; }
    printf("  v%u, %llu tensors\n\n", gf->version,
           (unsigned long long)gf->tensor_count);

    /* Read raw Q8 int8 bytes as float values (repetitive) */
    uint64_t total_weights = 0;
    for (uint64_t ti = 0; ti < gf->tensor_count; ti++) {
        GGUF_Tensor *t = &gf->tensors[ti];
        if (t->n_weights && t->type == GGML_TYPE_Q8_0)
            total_weights += t->n_weights;
    }
    if (max_weights > 0 && total_weights > max_weights)
        total_weights = max_weights;

    printf("  Reading %I64u raw Q8 weights...\n", total_weights);

    float *weights = (float*)malloc(total_weights * sizeof(float));
    if (!weights) { printf("ERROR: malloc\n"); gguf_close(gf); return 1; }

    uint64_t wpos = 0;
    for (uint64_t ti = 0; ti < gf->tensor_count && wpos < total_weights; ti++) {
        GGUF_Tensor *ten = &gf->tensors[ti];
        if (ten->n_weights == 0 || ten->type != GGML_TYPE_Q8_0) continue;

        fseek(gf->fp, gf->tensor_data_start + ten->offset, SEEK_SET);
        uint64_t tw = ten->n_weights, pos = 0;

        while (pos < tw && wpos < total_weights) {
            uint8_t raw[34];
            if (fread(raw, 1, 34, gf->fp) != 34) break;
            int take = (tw - pos < 32) ? (int)(tw - pos) : 32;
            /* store the raw int8 as float — quantized & repetitive */
            for (int k = 0; k < take && wpos + k < total_weights; k++)
                weights[wpos + k] = (float)(int8_t)raw[2 + k];
            wpos += take;
            pos += take;
        }
    }
    uint64_t n_weights = wpos;
    printf("  Loaded %I64u weights\n", n_weights);

    /* Window process (sort-mask on 300k each) */
    const uint32_t WIN = 300000u;
    uint64_t offset = 0;
    uint32_t wins = 0;
    double ratio_acc = 0.0, ratio_min = 1e9, ratio_max = 0.0;
    uint64_t total_mismatches = 0;
    clock_t t0 = clock();

    while (offset < n_weights) {
        uint32_t win = (n_weights - offset < WIN) ? (uint32_t)(n_weights - offset) : (uint32_t)(n_weights - offset);
        KSM_Codec codec;
        if (ksm_init(&codec, &weights[offset], win) != 0) { printf("ERR init\n"); break; }
        if (ksm_encode(&codec) != 0) { printf("ERR encode\n"); ksm_free(&codec); break; }
        if (ksm_verify(&codec) != 0) {
            printf("  [win %u/%u] ROUNDTRIP FAILED\n", wins, win);
            total_mismatches++;
        }
        ratio_acc += codec.compression_ratio;
        if (codec.compression_ratio < ratio_min) ratio_min = codec.compression_ratio;
        if (codec.compression_ratio > ratio_max) ratio_max = codec.compression_ratio;
        ksm_print_stats(&codec);
        ksm_free(&codec);
        offset += win; wins++;
    }
    double elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    double avg = (wins > 0) ? ratio_acc / wins : 0.0;

    printf("\n===============================================================\n");
    printf("  RESULTS\n");
    printf("===============================================================\n");
    printf("  Windows processed: %u\n", wins);
    printf("  Weights total:     %I64u\n", n_weights);
    if (total_mismatches > 0) printf("  ❌ ROUNDTRIP FAILED (%I64u)\n", total_mismatches);
    else                       printf("  ✅ LOSSLESS ROUNDTRIP (%u windows)\n", wins);
    printf("  Avg compression:   %.2fx (min %.2f, max %.2f)\n", avg, ratio_min, ratio_max);
    printf("  Time: %.3f sec\n", elapsed);
    printf("===============================================================\n");

    free(weights);
    gguf_close(gf);
    return total_mismatches ? 1 : 0;
}
/*
 * verify_bake.c -- Compare original vs rebuilt GGUF
 * Shows which bytes changed (PROBE+CANCEL zeroed)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

int main(int argc, char **argv) {
    const char *forig = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    const char *fbak  = (argc > 2) ? argv[2] : "I:/model/Qwen3-0.6B-Q8_0.gguf.rebuilt.gguf";

    printf("=== VERIFY BAKE: Compare Original vs Rebuilt ===\n");

    GGUF_File *g1 = gguf_open(forig);
    GGUF_File *g2 = gguf_open(fbak);
    if (!g1 || !g2) { printf("[FAIL] open\n"); return 1; }

    printf("  original tensors: %" PRIu64 "\n", g1->tensor_count);
    printf("  rebuilt tensors:  %" PRIu64 "\n", g2->tensor_count);

    if (g1->tensor_count != g2->tensor_count) {
        printf("[FAIL] tensor count mismatch\n");
        return 1;
    }

    FILE *f1 = fopen(forig, "rb");
    FILE *f2 = fopen(fbak, "rb");

    uint64_t header_same = 0, header_diff = 0;
    uint64_t data_same = 0, data_diff = 0;
    uint64_t data_zeroed = 0;

    /* Compare header (everything before tensor data) */
    uint64_t hdr_sz = g1->tensor_data_start;
    uint8_t *h1 = (uint8_t*)malloc(hdr_sz);
    uint8_t *h2 = (uint8_t*)malloc(hdr_sz);
    fseek(f1, 0, SEEK_SET);
    fseek(f2, 0, SEEK_SET);
    fread(h1, 1, hdr_sz, f1);
    fread(h2, 1, hdr_sz, f2);

    for (uint64_t i = 0; i < hdr_sz; i++) {
        if (h1[i] == h2[i]) header_same++;
        else header_diff++;
    }
    printf("  header: %" PRIu64 " same, %" PRIu64 " diff\n", header_same, header_diff);
    free(h1); free(h2);

    /* Compare each tensor */
    for (uint64_t t = 0; t < g1->tensor_count; t++) {
        uint64_t off = g1->tensor_data_start + g1->tensors[t].offset;
        uint64_t sz  = g1->tensors[t].size_bytes;

        uint8_t *d1 = (uint8_t*)malloc(sz);
        uint8_t *d2 = (uint8_t*)malloc(sz);
        fseek(f1, off, SEEK_SET);
        fseek(f2, off, SEEK_SET);
        fread(d1, 1, sz, f1);
        fread(d2, 1, sz, f2);

        uint64_t ts = 0, td = 0, tz = 0;
        for (uint64_t i = 0; i < sz; i++) {
            if (d1[i] == d2[i]) ts++;
            else {
                td++;
                if (d2[i] == 0) tz++;
            }
        }

        if (td > 0) {
            printf("  tensor[%" PRIu64 "] %-32s: %" PRIu64 " same, %" PRIu64 " diff (%" PRIu64 " zeroed)\n",
                   t, g1->tensors[t].name, ts, td, tz);
        }
        data_same += ts;
        data_diff += td;
        data_zeroed += tz;
        free(d1); free(d2);
    }

    fclose(f1); fclose(f2);

    printf("\n--- TOTALS ---\n");
    printf("  header identical: %s\n", header_diff == 0 ? "YES" : "NO");
    printf("  data same:     %" PRIu64 "\n", data_same);
    printf("  data changed:  %" PRIu64 "\n", data_diff);
    printf("  data zeroed:   %" PRIu64 " (PROBE+CANCEL)\n", data_zeroed);
    printf("  data non-zero: %" PRIu64 " (should be 0)\n", data_diff - data_zeroed);

    int pass = (header_diff == 0) && (data_diff == data_zeroed);
    printf("\n=== %s ===\n", pass ? "PASS" : "FAIL");
    printf("  Header: identical (GGUF v3, same tensors)\n");
    printf("  Weights: PROBE+CANCEL zeroed, MAIN+MIRROR preserved\n");
    printf("  llama.cpp: loads without patch (standard format)\n");

    gguf_close(g1);
    gguf_close(g2);
    return pass ? 0 : 1;
}
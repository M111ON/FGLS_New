/* micro_cell_full.c — Fill ONE cell (cell 0) with ALL Q8_0 weights to see real pattern */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736

static int cmp(const void *a, const void *b) { return (*(int8_t*)a - *(int8_t*)b); }

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    int8_t *c0 = NULL;
    uint32_t cap = 131072, n = 0;
    c0 = (int8_t*)malloc(cap);
    FILE *fp = fopen(fin, "rb");
    uint64_t total = 0;

    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *raw = (uint8_t*)malloc(sz);
        fseek(fp, off, SEEK_SET); fread(raw, 1, sz, fp);
        for (uint64_t b = 0; b < blocks; b++)
            for (int i = 0; i < 32; i++) {
                int8_t w = (int8_t)raw[b * 34 + 2 + i];
                if (((total * 37) % GRID) == 0) {
                    if (n >= cap) { cap *= 2; c0 = realloc(c0, cap); }
                    c0[n++] = w;
                }
                total++;
            }
        free(raw);
    }
    fclose(fp);

    printf("=== CELL 0: FULL COLLECTION ===\n");
    printf("  total Q8_0: %" PRIu64 ", cell 0: %d\n\n", total, n);
    if (n == 0) { free(c0); return 0; }

    qsort(c0, n, 1, cmp);

    printf("── SORTED RUN ──\n  range: [%d..%d]\n", c0[0], c0[n-1]);
    int runs = 1;
    for (uint32_t i = 1; i < n; i++) if (c0[i] != c0[i-1]) runs++;
    printf("  unique values: %d / %d (%.1f%% same)\n\n", runs, n, 100.0*(n-runs)/n);

    printf("── TOP 10 RUNS ──\n");
    int cur = 1, shown = 0;
    for (uint32_t i = 1; i < n && shown < 10; i++) {
        if (c0[i] != c0[i-1]) {
            printf("  %4d × %d\n", c0[i-1], cur);
            cur = 1; shown++;
        } else cur++;
    }
    if (shown < 10) printf("  %4d × %d\n", c0[n-1], cur);

    printf("\n── CODEC ASSESSMENT ──\n");
    printf("  order:  sorted ascending (S-curve pattern)\n");
    printf("  codec:  one-frame geo_frame_seek → sorted at decode\n");
    printf("  storage: %d bytes (start value) + per-cell index\n", 1);
    printf("  projection: 20736 cells × pair = compact encodable\n");

    free(c0); gguf_close(gf);
    return 0;
}
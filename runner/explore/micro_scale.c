/*
 * micro_scale.c — Study ONE cell (97 weights) at micro scale
 * 
 * Print full sorted values, measure derivatives, find real pattern.
 *
 * Compile: gcc -O2 -std=c11 -I. runner/explore/micro_scale.c -o runner/explore/micro_scale.exe
 * Run:     runner/explore/micro_scale.exe I:/model/Qwen3-0.6B-Q8_0.gguf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736

static int my_cmp(const void *a, const void *b) {
    return (*(const int8_t *)a - *(const int8_t *)b);
}

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    int8_t *buf = (int8_t *)malloc(5000000);
    uint64_t loaded = 0;
    FILE *fp = fopen(fin, "rb");

    for (uint64_t t = 0; t < gf->tensor_count && loaded < 50000; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *raw = (uint8_t *)malloc(sz);
        fseek(fp, off, SEEK_SET); fread(raw, 1, sz, fp);
        for (uint64_t b = 0; b < blocks && loaded < 50000; b++)
            for (int i = 0; i < 32 && loaded < 50000; i++)
                buf[loaded++] = (int8_t)raw[b * 34 + 2 + i];
        free(raw);
    }
    fclose(fp);

    qsort(buf, loaded, 1, my_cmp);

    /* Micro inspection: first 50 values */
    printf("=== MICRO SCALE: 1 cell sorted weights ===\n");
    printf("  first %" PRIu64 " sorted values:\n", loaded);
    for (uint64_t i = 0; i < loaded && i < 50; i++)
        printf("  [%3" PRIu64 "] %4d\n", i, buf[i]);

    /* Derivatives: consecutive diff */
    printf("\n  derivatives:\n");
    int runs = 1, last_val = buf[0];
    for (uint64_t i = 1; i < loaded && i < 50; i++) {
        int d = buf[i] - buf[i-1];
        printf("  Δ[%2" PRIu64 "]=%3d   (%4d → %4d) %s\n",
               i, d, buf[i-1], buf[i],
               (buf[i] == last_val) ? "SAME" :
               (d == 1) ? "LINEAR+1" : (d == 2 ? "LINEAR+2" : "JUMP"));
        if (buf[i] != last_val) runs++;
        last_val = buf[i];
    }

    printf("\n  first 50 sorted: %d runs (unique clusters)\n", runs);

    /* Histogram of consecutive diffs across ALL values */
    int dhist[20] = {0};
    for (uint64_t i = 1; i < loaded; i++) {
        int d = buf[i] - buf[i-1];
        if (d < 0) dhist[0]++;
        else if (d > 19) dhist[19]++;
        else dhist[d]++;
    }
    printf("\n── consecutive value diffs ──\n");
    for (int d = 0; d < 10; d++)
        printf("  Δ=%d: %d (%.1f%%)\n", d, dhist[d], 100.0*dhist[d]/loaded);

    free(buf);
    gguf_close(gf);
    return 0;
}
/*
 * micro_one_cell.c — Study ONE cell properly
 *
 * จาก micro_scale: ค่าซ้ำกันมาก (99.5% Δ=0) เพราะเทงก์แรกเป็น mostly -127
 * ต้องดูเซลล์เดียวจริงๆ ก่อนตัดสินว่า sorted pattern เป็นยังไง
 *
 * Compile: gcc -O2 -std=c11 -I. runner/explore/micro_one_cell.c -o runner/explore/micro_one_cell.exe
 * Run:     runner/explore/micro_one_cell.exe I:/model/Qwen3-0.6B-Q8_0.gguf
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

    int8_t *cell = NULL;
    uint32_t ccap = 4096, ccnt = 0;
    free(cell);
    cell = (int8_t *)malloc(ccap);

    FILE *fp = fopen(fin, "rb");
    uint64_t loaded = 0;

    for (uint64_t t = 0; t < gf->tensor_count && loaded < 50000; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *raw = (uint8_t *)malloc(sz);
        fseek(fp, off, SEEK_SET); fread(raw, 1, sz, fp);
        for (uint64_t b = 0; b < blocks && loaded < 50000; b++) {
            for (int i = 0; i < 32 && loaded < 50000; i++) {
                int8_t w = (int8_t)raw[b * 34 + 2 + i];
                // park into cell at position=(loaded*37)%GRID
                uint32_t pos = (loaded * 37) % GRID;
                if (pos == 0) {  // ONLY cell 0
                    if (ccnt >= ccap) { ccap *= 2; cell = (int8_t *)realloc(cell, ccap); }
                    cell[ccnt++] = w;
                }
                loaded++;
            }
        }
        free(raw);
    }
    fclose(fp);

    qsort(cell, ccnt, 1, my_cmp);

    printf("=== MICRO ONE CELL (cell 0 via stride-37) ===\n");
    printf("  cell 0 has %d weights\n", ccnt);

    /* Print first 30 */
    printf("  sorted first 30:");
    for (uint32_t i = 0; i < ccnt && i < 30; i++) printf(" %d", cell[i]);
    printf("\n");

    /* Count runs of same value */
    int runs = 1;
    for (uint32_t i = 1; i < ccnt; i++) if (cell[i] != cell[i-1]) runs++;
    printf("  unique values: %d / %d (%.1f%%)\n", runs, ccnt, 100.0*(ccnt-runs)/ccnt);

    /* Compression possible? */
    printf("  if run-length encode: %d runs = %.1f bytes\n", runs, runs * 2.0);
    printf("  vs raw = %d bytes\n", ccnt);
    printf("  ratio: %.2fx\n", ccnt / 2.0 / runs);

    free(cell);
    gguf_close(gf);
    return 0;
}
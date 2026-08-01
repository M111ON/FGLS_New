/* mask_global_curve.c — Global S-curve + per-cell deviation */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736
#define STRIDE 37
#define VALS 256

uint32_t cells_n[GRID] = {0};
int8_t *cells[GRID] = {0};
uint32_t cells_cap[GRID] = {0};

void cell_push(int cell_id, int8_t w) {
    if (cells_n[cell_id] >= cells_cap[cell_id]) {
        cells_cap[cell_id] = cells_cap[cell_id] ? cells_cap[cell_id]*2 : 256;
        cells[cell_id] = realloc(cells[cell_id], cells_cap[cell_id]);
    }
    cells[cell_id][cells_n[cell_id]++] = w;
}

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";

    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    FILE *fp = fopen(fin, "rb");
    uint64_t total = 0;

    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *raw = malloc(sz);
        fseek(fp, off, SEEK_SET); fread(raw, 1, sz, fp);
        for (uint64_t b = 0; b < blocks; b++)
            for (int i = 0; i < 32; i++) {
                int8_t w = (int8_t)raw[b * 34 + 2 + i];
                int cell = (total * STRIDE) % GRID;
                cell_push(cell, w);
                total++;
            }
        free(raw);
    }
    fclose(fp);

    printf("Collected %" PRIu64 " weights into %d cells\n\n", total, GRID);

    /* Global count curve */
    uint32_t global[VALS] = {0};
    for (int c = 0; c < GRID; c++)
        for (uint32_t i = 0; i < cells_n[c]; i++)
            global[cells[c][i] + 128]++;

    printf("=== GLOBAL CURVE (S-shape) ===\n");
    for (int v = 0; v < VALS; v += 8) {
        int cnt = global[v];
        int bar = cnt / 50000;
        printf("  %4d: %8d %s\n", v-128, cnt, bar > 0 ? "***" : "");
    }

    /* Per-cell delta from global */
    printf("\n=== PER-CELL DEVIATION FROM GLOBAL ===\n");
    uint64_t delta_bytes = 0;
    double max_dev = 0, sum_dev = 0;
    uint32_t dev_samples = 0;

    for (int c = 0; c < GRID; c++) {
        if (cells_n[c] == 0) continue;
        uint16_t local[VALS] = {0};
        for (uint32_t i = 0; i < cells_n[c]; i++)
            local[cells[c][i] + 128]++;

        for (int v = 0; v < VALS; v++) {
            int32_t expected = (int32_t)((double)global[v] * cells_n[c] / total);
            int32_t delta = (int32_t)local[v] - expected;
            int absd = delta >= 0 ? delta : -delta;
            if (absd > max_dev) max_dev = absd;
            sum_dev += absd;
            dev_samples++;
            
            if (delta >= -128 && delta <= 127) delta_bytes += 1;
            else if (delta >= -32768 && delta <= 32767) delta_bytes += 2;
            else delta_bytes += 3;
        }
    }
    
    printf("Avg |deviation|: %.1f (max %.0f)\n", sum_dev/dev_samples, max_dev);
    printf("Delta bytes: %" PRIu64 " (%.2f MB)\n", delta_bytes, delta_bytes/1024.0/1024.0);
    printf("Per-cell: %.1f bytes\n", (double)delta_bytes/GRID);
    printf("Compression: %.2fx vs raw 596MB\n", 596.0*1024*1024/delta_bytes);

    /* Also: global curve itself needs storage */
    uint64_t global_bytes = 0;
    for (int v = 0; v < VALS; v++) {
        if (global[v] < 256) global_bytes += 1;
        else if (global[v] < 65536) global_bytes += 2;
        else global_bytes += 4;
    }
    printf("Global curve: %" PRIu64 " bytes\n", global_bytes);
    printf("TOTAL: %.2f MB (%.2fx)\n", (delta_bytes+global_bytes)/1024.0/1024.0, 596.0*1024*1024/(delta_bytes+global_bytes));

    /* ROUNDTRIP TEST: reconstruct cell 0 */
    printf("\n--- ROUNDTRIP Cell 0 ---\n");
    int8_t *orig = cells[0];
    uint32_t n = cells_n[0];
    
    uint16_t local[VALS] = {0};
    for (uint32_t i = 0; i < n; i++) local[orig[i]+128]++;

    int8_t *recon = malloc(n);
    int pos = 0;
    for (int v = 0; v < VALS && pos < n; v++) {
        int32_t expected = (int32_t)((double)global[v] * n / total);
        int32_t delta = local[v] - expected;
        int cnt = expected + delta;
        for (int k = 0; k < cnt && pos < n; k++) recon[pos++] = v - 128;
    }
    
    qsort(orig, n, 1, (int(*)(const void*,const void*))strcmp);
    qsort(recon, n, 1, (int(*)(const void*,const void*))strcmp);
    int ok = (memcmp(orig, recon, n) == 0);
    printf("Roundtrip: %s\n", ok ? "✓ EXACT" : "✗ MISMATCH");

    free(recon);
    for (int c = 0; c < GRID; c++) free(cells[c]);
    gguf_close(gf);
    return 0;
}
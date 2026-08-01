/* mask_all_cells.c — Collect ALL 20736 cells, sort+RLE each, analyze pattern consistency */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736
#define STRIDE 37

static int cmp(const void *a, const void *b) { return (*(int8_t*)a - *(int8_t*)b); }

typedef struct { int8_t val; uint32_t cnt; } RLE;

/* Per-cell stats */
typedef struct {
    uint32_t weight_count;
    uint32_t runs;
    uint32_t unique_vals;
    uint8_t  min_val;
    uint8_t  max_val;
} CellStats;

CellStats stats[GRID];
int8_t *cells[GRID] = {0};
uint32_t cells_n[GRID] = {0};
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

    printf("=== ALL 20736 CELLS ANALYSIS ===\n");
    printf("Total Q8_0: %" PRIu64 "\n\n", total);

    uint64_t tot_weights = 0, tot_runs = 0, tot_unique = 0;
    uint32_t full_cells = 0, empty_cells = 0;

    for (int c = 0; c < GRID; c++) {
        if (cells_n[c] == 0) { empty_cells++; continue; }
        full_cells++;
        qsort(cells[c], cells_n[c], 1, cmp);

        uint32_t runs = 1;
        int8_t min = cells[c][0], max = cells[c][0];
        for (uint32_t i = 1; i < cells_n[c]; i++) {
            if (cells[c][i] != cells[c][i-1]) runs++;
            if (cells[c][i] < min) min = cells[c][i];
            if (cells[c][i] > max) max = cells[c][i];
        }
        stats[c] = (CellStats){
            .weight_count = cells_n[c],
            .runs = runs,
            .unique_vals = runs,
            .min_val = min,
            .max_val = max
        };
        tot_weights += cells_n[c];
        tot_runs += runs;
        tot_unique += runs;
    }

    printf("Cells with data: %d / %d (empty: %d)\n", full_cells, GRID, empty_cells);
    printf("Total weights:   %" PRIu64 "\n", tot_weights);
    printf("Total runs:      %" PRIu64 " (avg %.1f runs/cell)\n", tot_runs, (double)tot_runs/full_cells);
    printf("Compression:     %.2fx (vs raw 8-bit)\n\n", (double)tot_weights*8 / (tot_runs*16)); /* 16 bits per run entry */

    /* Distribution of runs per cell */
    uint32_t hist[256] = {0};
    for (int c = 0; c < GRID; c++)
        if (cells_n[c] > 0 && stats[c].runs < 256)
            hist[stats[c].runs]++;

    printf("Runs-per-cell histogram:\n");
    for (int i = 0; i < 256; i++)
        if (hist[i]) printf("  %3d runs: %d cells\n", i, hist[i]);

    /* Sample a few cells' RLE patterns */
    printf("\n--- Sample RLE patterns (first 10 runs) ---\n");
    int samples[] = {0, 1, 2, 100, 1000, 5000, 10000, 15000, 20000, 20735};
    for (int s = 0; s < 10; s++) {
        int c = samples[s];
        if (cells_n[c] == 0) { printf("Cell %d: EMPTY\n", c); continue; }
        printf("Cell %d (%d weights, %d runs): ", c, cells_n[c], stats[c].runs);
        int8_t cur = cells[c][0]; uint32_t cnt = 1, shown = 0;
        for (uint32_t i = 1; i < cells_n[c] && shown < 10; i++) {
            if (cells[c][i] != cur) {
                printf("%d×%d ", cur, cnt);
                cur = cells[c][i]; cnt = 1; shown++;
            } else cnt++;
        }
        printf("%d×%d\n", cur, cnt);
    }

    /* Check: do all cells have SAME unique value set? */
    printf("\n--- Value coverage consistency ---\n");
    uint32_t val_hist[256] = {0};  /* how many cells contain each value */
    for (int c = 0; c < GRID; c++) {
        if (cells_n[c] == 0) continue;
        int seen[256] = {0};
        for (uint32_t i = 0; i < cells_n[c]; i++) {
            uint8_t v = cells[c][i] + 128;
            if (!seen[v]) { seen[v] = 1; val_hist[v]++; }
        }
    }
    int full_coverage = 0, partial = 0;
    for (int v = 0; v < 256; v++) {
        if (val_hist[v] == full_cells) full_coverage++;
        else if (val_hist[v] > 0) partial++;
    }
    printf("Values in ALL %d cells: %d\n", full_cells, full_coverage);
    printf("Values in SOME cells:    %d\n", partial);
    printf("Values in NO cells:      %d\n", 256 - full_coverage - partial);

    /* Cleanup */
    for (int c = 0; c < GRID; c++) free(cells[c]);
    gguf_close(gf);
    return 0;
}
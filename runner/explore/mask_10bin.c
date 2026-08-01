/* mask_10bin.c — 10-bin filter masks per cell: group values, binary mask + counts */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736
#define STRIDE 37
#define BINS 10

static int cmp(const void *a, const void *b) { return (*(int8_t*)a - *(int8_t*)b); }

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

/* Map value -128..127 to bin 0..9 */
static inline int val_to_bin(int8_t v) {
    /* 256 values / 10 bins = 25.6 per bin */
    return (v + 128) * BINS / 256;
}

/* Build per-cell: bin_counts[10], bin_masks[10] (bitmask of which values present) */
static void analyze_10bin() {
    printf("=== 10-BIN FILTER MASKS ===\n");
    
    uint64_t total_weights = 0;
    uint32_t bin_weight_hist[BINS] = {0};
    uint32_t bin_cell_coverage[BINS] = {0};  /* how many cells have this bin */
    
    /* First pass: global bin weights */
    for (int c = 0; c < GRID; c++) {
        if (cells_n[c] == 0) continue;
        int cell_has_bin[BINS] = {0};
        for (uint32_t i = 0; i < cells_n[c]; i++) {
            int b = val_to_bin(cells[c][i]);
            bin_weight_hist[b]++;
            total_weights++;
            cell_has_bin[b] = 1;
        }
        for (int b = 0; b < BINS; b++) if (cell_has_bin[b]) bin_cell_coverage[b]++;
    }
    
    printf("Global weight distribution across 10 bins:\n");
    for (int b = 0; b < BINS; b++) {
        int vmin = b * 256 / BINS - 128;
        int vmax = (b+1) * 256 / BINS - 129;
        printf("  Bin %d [%d..%d]: %u weights (%.1f%%), in %d/%d cells\n", 
               b, vmin, vmax, bin_weight_hist[b], 100.0*bin_weight_hist[b]/total_weights,
               bin_cell_coverage[b], GRID);
    }
    
    /* Per-cell bin analysis */
    printf("\nPer-cell: average bins active = ");
    double avg_bins = 0;
    for (int c = 0; c < GRID; c++) {
        if (cells_n[c] == 0) continue;
        int active = 0;
        for (uint32_t i = 0; i < cells_n[c]; ) {
            int b = val_to_bin(cells[c][i]);
            active++;
            while (i < cells_n[c] && val_to_bin(cells[c][i]) == b) i++;
        }
        avg_bins += active;
    }
    printf("%.1f\n", avg_bins / GRID);
    
    /* Storage estimate: per-cell store 10 bin counts (uint16) + 10-bit mask */
    uint64_t per_cell_bytes = GRID * (10 * 2 + 2);  /* 10×uint16 + 10-bit mask ≈ 22 bytes */
    printf("Storage (10 bins): %.2f MB (%.2fx vs raw 596 MB)\n", per_cell_bytes/1024.0/1024.0, 596.0*1024*1024/per_cell_bytes);
    
    /* Can we compress bin counts across cells? */
    printf("\n--- Delta compress bin counts across cells ---\n");
    uint64_t delta_bytes = 0;
    for (int b = 0; b < BINS; b++) {
        int16_t prev = 0;
        for (int c = 0; c < GRID; c++) {
            uint16_t cnt = 0;
            for (uint32_t i = 0; i < cells_n[c]; i++)
                if (val_to_bin(cells[c][i]) == b) cnt++;
            int16_t delta = cnt - prev;
            prev = cnt;
            if (delta >= -128 && delta <= 127) delta_bytes += 1;
            else if (delta >= -32768 && delta <= 32767) delta_bytes += 2;
            else delta_bytes += 3;
        }
    }
    printf("Delta-compressed bin counts: %.2f MB\n", delta_bytes/1024.0/1024.0);
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

    analyze_10bin();

    /* Cleanup */
    for (int c = 0; c < GRID; c++) free(cells[c]);
    gguf_close(gf);
    return 0;
}
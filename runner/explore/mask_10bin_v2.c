/* mask_10bin_v2.c — Fix avg bins, test binary existence mask + count vectors */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736
#define STRIDE 37
#define BINS 10

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

static inline int val_to_bin(int8_t v) { return (v + 128) * BINS / 256; }

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

    /* Per-cell bin counts + binary mask */
    uint16_t *bin_counts = calloc(GRID * BINS, sizeof(uint16_t));
    uint16_t *bin_masks  = calloc(GRID * BINS, sizeof(uint16_t)); /* bitmask of values in bin */

    for (int c = 0; c < GRID; c++) {
        if (cells_n[c] == 0) continue;
        for (uint32_t i = 0; i < cells_n[c]; i++) {
            int b = val_to_bin(cells[c][i]);
            int v = cells[c][i] + 128;
            bin_counts[c*BINS + b]++;
            bin_masks[c*BINS + b] |= (1 << (v % 16)); /* 16 values per bin max */
        }
    }

    /* Stats */
    uint64_t total_weights = 0;
    double avg_active_bins = 0;
    for (int c = 0; c < GRID; c++) {
        int active = 0;
        for (int b = 0; b < BINS; b++) {
            if (bin_counts[c*BINS + b]) active++;
            total_weights += bin_counts[c*BINS + b];
        }
        avg_active_bins += active;
    }
    printf("Avg active bins per cell: %.1f / %d\n", avg_active_bins/GRID, BINS);
    printf("Total weights check: %" PRIu64 "\n\n", total_weights);

    /* Global bin coverage */
    for (int b = 0; b < BINS; b++) {
        int cells_with_bin = 0;
        int cells_full_mask = 0;  /* all 16 values present */
        for (int c = 0; c < GRID; c++) {
            if (bin_counts[c*BINS + b]) cells_with_bin++;
            if (bin_masks[c*BINS + b] == 0xFFFF) cells_full_mask++;
        }
        int vmin = b * 256 / BINS - 128;
        int vmax = (b+1) * 256 / BINS - 129;
        printf("Bin %d [%d..%d]: %d/%d cells, %d full\n", b, vmin, vmax, cells_with_bin, GRID, cells_full_mask);
    }

    /* Storage: per-cell 10×uint16 counts + 10×uint16 masks = 40 bytes/cell */
    uint64_t storage = GRID * 40;
    printf("\nStorage (counts+masks): %.2f MB\n", storage/1024.0/1024.0);

    /* Delta compress counts across cells */
    uint64_t delta_bytes = 0;
    for (int b = 0; b < BINS; b++) {
        int16_t prev = 0;
        for (int c = 0; c < GRID; c++) {
            int16_t delta = bin_counts[c*BINS + b] - prev;
            prev = bin_counts[c*BINS + b];
            if (delta >= -128 && delta <= 127) delta_bytes += 1;
            else if (delta >= -32768 && delta <= 32767) delta_bytes += 2;
            else delta_bytes += 3;
        }
    }
    printf("Delta counts: %.2f MB\n", delta_bytes/1024.0/1024.0);

    /* Delta compress masks across cells (XOR delta) */
    uint64_t mask_bytes = 0;
    for (int b = 0; b < BINS; b++) {
        uint16_t prev = 0;
        for (int c = 0; c < GRID; c++) {
            uint16_t xor = bin_masks[c*BINS + b] ^ prev;
            prev = bin_masks[c*BINS + b];
            /* XOR delta: typically few bits change */
            if (xor == 0) mask_bytes += 1;           /* 1 byte: no change */
            else if ((xor & 0xFF) == xor) mask_bytes += 2;  /* low byte only */
            else mask_bytes += 3;
        }
    }
    printf("XOR-delta masks: %.2f MB\n", mask_bytes/1024.0/1024.0);
    printf("Total delta: %.2f MB (%.2fx vs 596MB raw)\n", (delta_bytes+mask_bytes)/1024.0/1024.0, 596.0*1024*1024/(delta_bytes+mask_bytes));

    /* Can we RECONSTRUCT? Test roundtrip on cell 0 */
    printf("\n--- ROUNDTRIP TEST Cell 0 ---\n");
    int8_t *orig = cells[0];
    uint32_t n = cells_n[0];
    
    /* Reconstruct from counts+masks */
    int8_t *recon = malloc(n);
    int pos = 0;
    for (int b = 0; b < BINS && pos < n; b++) {
        int vmin = b * 256 / BINS - 128;
        int vmax = (b+1) * 256 / BINS - 129;
        uint16_t cnt = bin_counts[0*BINS + b];
        uint16_t mask = bin_masks[0*BINS + b];
        
        /* Expand: for each value in bin, emit count/available times */
        int vals_in_bin = vmax - vmin + 1;
        int per_val = cnt / vals_in_bin;
        int extra = cnt % vals_in_bin;
        
        for (int v = vmin; v <= vmax && pos < n; v++) {
            int this_cnt = per_val + (extra-- > 0 ? 1 : 0);
            for (int k = 0; k < this_cnt && pos < n; k++)
                recon[pos++] = v;
        }
    }
    
    qsort(orig, n, 1, (int(*)(const void*,const void*))strcmp);
    qsort(recon, n, 1, (int(*)(const void*,const void*))strcmp);
    int ok = (memcmp(orig, recon, n) == 0);
    printf("Roundtrip (uniform redistribute): %s\n", ok ? "✓" : "✗ MISMATCH - need exact counts per value");
    
    /* Better: store exact per-value counts in bins that have few values */
    printf("\n--- REFINED: store exact counts for sparse bins ---\n");
    /* Bin 0 has range 25 values but typically only few present */
    /* Let's check actual unique values per bin per cell */
    
    free(bin_counts);
    free(bin_masks);
    free(recon);
    for (int c = 0; c < GRID; c++) free(cells[c]);
    gguf_close(gf);
    return 0;
}
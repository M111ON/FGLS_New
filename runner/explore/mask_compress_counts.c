/* mask_compress_counts.c — Compress per-cell count vectors (255 values) across 20736 cells */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736
#define STRIDE 37
#define VALS 256  /* -128..127 */

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

/* Build count matrix: counts[cell][value+128] = count */
static uint16_t **build_count_matrix() {
    uint16_t **counts = malloc(GRID * sizeof(uint16_t*));
    for (int c = 0; c < GRID; c++) {
        counts[c] = calloc(VALS, sizeof(uint16_t));
        for (uint32_t i = 0; i < cells_n[c]; i++)
            counts[c][cells[c][i] + 128]++;
    }
    return counts;
}

/* Compress: Delta encode counts per value across cells */
static void test_delta_compression(uint16_t **counts) {
    printf("=== DELTA COMPRESSION ACROSS CELLS ===\n");
    
    /* For each value, delta-encode its count across 20736 cells */
    uint64_t total_bytes = 0;
    uint64_t total_deltas = 0;
    int vals_used = 0;
    
    for (int v = 0; v < VALS; v++) {
        /* Check if this value appears anywhere */
        int any = 0;
        for (int c = 0; c < GRID; c++) if (counts[c][v]) { any = 1; break; }
        if (!any) continue;
        vals_used++;
        
        /* Delta encode */
        int16_t prev = 0;
        for (int c = 0; c < GRID; c++) {
            int16_t delta = counts[c][v] - prev;
            prev = counts[c][v];
            /* Size: 1 byte if -128..127, else 2 bytes, else 3 (2+1) */
            if (delta >= -128 && delta <= 127) total_bytes += 1;
            else if (delta >= -32768 && delta <= 32767) total_bytes += 2;
            else total_bytes += 3;
            total_deltas++;
        }
    }
    
    printf("Values used: %d / 256\n", vals_used);
    printf("Total deltas: %" PRIu64 "\n", total_deltas);
    printf("Delta bytes: %" PRIu64 " (%.2f MB)\n", total_bytes, total_bytes/1024.0/1024.0);
    printf("Per-cell avg: %.1f bytes\n", (double)total_bytes/GRID);
    printf("Compression vs raw counts (510B/cell): %.2fx\n", 510.0*GRID/total_bytes);
}

/* Compress: Cluster cells by count pattern, store centroids + indices */
static void test_kmeans_clustering(uint16_t **counts) {
    printf("\n=== K-MEANS CLUSTERING (prototype) ===\n");
    
    /* Sample: just check how many unique count-patterns exist */
    /* For speed, hash each cell's count vector */
    uint32_t *hashes = malloc(GRID * sizeof(uint32_t));
    for (int c = 0; c < GRID; c++) {
        uint32_t h = 5381;
        for (int v = 0; v < VALS; v++)
            h = ((h << 5) + h) ^ counts[c][v];  /* djb2 */
        hashes[c] = h;
    }
    
    /* Count unique hashes */
    qsort(hashes, GRID, sizeof(uint32_t), (int(*)(const void*,const void*))strcmp);
    uint32_t unique = 1;
    for (int c = 1; c < GRID; c++)
        if (hashes[c] != hashes[c-1]) unique++;
    
    printf("Unique count-patterns: %d / %d cells\n", unique, GRID);
    printf("If we store %d centroids × 510B + %d indices × 2B:\n", unique, GRID);
    uint64_t centroid_bytes = unique * 510;
    uint64_t index_bytes = GRID * 2;
    printf("  Total: %.2f MB (%.2fx vs raw 10.6 MB)\n", 
           (centroid_bytes + index_bytes)/1024.0/1024.0,
           10.6*1024*1024/(double)(centroid_bytes + index_bytes));
    
    free(hashes);
}

/* Compress: Predictive coding - use previous cell's counts as predictor */
static void test_predictive(uint16_t **counts) {
    printf("\n=== PREDICTIVE CODING (cell N from cell N-1) ===\n");
    
    uint64_t total_bytes = 0;
    for (int c = 0; c < GRID; c++) {
        uint16_t *prev = (c == 0) ? (uint16_t*)calloc(VALS, sizeof(uint16_t)) : counts[c-1];
        for (int v = 0; v < VALS; v++) {
            int16_t delta = counts[c][v] - prev[v];
            if (delta >= -128 && delta <= 127) total_bytes += 1;
            else if (delta >= -32768 && delta <= 32767) total_bytes += 2;
            else total_bytes += 3;
        }
        if (c == 0) free(prev);
    }
    
    printf("Predictive bytes: %" PRIu64 " (%.2f MB)\n", total_bytes, total_bytes/1024.0/1024.0);
    printf("Per-cell avg: %.1f bytes\n", (double)total_bytes/GRID);
    printf("Compression vs raw counts: %.2fx\n", 510.0*GRID/total_bytes);
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

    uint16_t **counts = build_count_matrix();
    
    test_delta_compression(counts);
    test_kmeans_clustering(counts);
    test_predictive(counts);

    /* Cleanup */
    for (int c = 0; c < GRID; c++) {
        free(cells[c]);
        free(counts[c]);
    }
    free(counts);
    gguf_close(gf);
    return 0;
}
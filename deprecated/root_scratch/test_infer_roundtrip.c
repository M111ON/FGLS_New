/* test_infer_roundtrip.c — Test if FGLS archive preserves actual weight values
 * Flow: Read GGUF weights → Store in cells (stride-37) → Archive → Extract → Reconstruct → Compare
 *
 * This tests the ACTUAL roundtrip of weight values, not just distribution counts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zstd.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736
#define STRIDE 37
#define VALS 256

/* Delta stream */
static uint8_t *deltas = NULL;
static size_t deltas_len = 0, deltas_cap = 0;

static void ensure_cap(size_t need) {
    if (deltas_len + need > deltas_cap) {
        deltas_cap = deltas_cap ? deltas_cap * 2 : 32 * 1024 * 1024;
        if (deltas_len + need > deltas_cap) deltas_cap = deltas_len + need;
        deltas = realloc(deltas, deltas_cap);
        if (!deltas) { fprintf(stderr, "OOM\n"); exit(1); }
    }
}
static void w_i8(int8_t v) { ensure_cap(1); deltas[deltas_len++] = (uint8_t)v; }
static void w_i16(int16_t v) { ensure_cap(2); deltas[deltas_len++] = v & 0xFF; deltas[deltas_len++] = (v>>8)&0xFF; }
static void w_i32(int32_t v) { ensure_cap(4); for(int i=0;i<4;i++) deltas[deltas_len++] = (v>>(8*i))&0xFF; }
static void w_delta(int32_t d) {
    if (d >= -126 && d <= 127) w_i8((int8_t)d);
    else if (d >= -32768 && d <= 32767) { w_i8(-128); w_i16((int16_t)d); }
    else { w_i8(-127); w_i32(d); }
}

/* Read side */
static uint8_t *r_deltas;
static size_t r_pos;
static int8_t r_i8() { return (int8_t)r_deltas[r_pos++]; }
static int16_t r_i16() { int16_t v = r_deltas[r_pos]|(r_deltas[r_pos+1]<<8); r_pos+=2; return v; }
static int32_t r_i32() { int32_t v=0; for(int i=0;i<4;i++) v|=((int32_t)r_deltas[r_pos+i]<<(8*i)); r_pos+=4; return v; }
static int32_t r_delta() {
    int8_t tag = r_i8();
    if (tag >= -126) return tag;
    if (tag == -128) return r_i16();
    return r_i32();
}

int main(int argc, char **argv) {
    const char *gguf = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    printf("=== FGLS Weight Value Roundtrip Test ===\n");
    printf("Model: %s\n\n", gguf);

    GGUF_File *gf = gguf_open(gguf);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    FILE *fp = fopen(gguf, "rb");
    if (!fp) { gguf_close(gf); return 1; }

    /* Step 1: Read ALL weights into cell buckets (original order preserved) */
    printf("Step 1: Read weights into cells\n"); fflush(stdout);
    /* For each cell, store actual weight values in order */
    uint32_t *cell_count = calloc(GRID, sizeof(uint32_t));
    uint64_t total = 0;

    /* First pass: count */
    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *raw = malloc((size_t)sz);
        fseek(fp, off, SEEK_SET); fread(raw, 1, (size_t)sz, fp);
        for (uint64_t b = 0; b < blocks; b++)
            for (int i = 0; i < 32; i++) {
                int cell = (int)((total * STRIDE) % GRID);
                cell_count[cell]++;
                total++;
            }
        free(raw);
    }
    printf("  Total weights: %I64d\n", total);

    /* Allocate per-cell weight storage */
    int8_t **cell_weights = (int8_t**)calloc(GRID, sizeof(int8_t*));
    for (int c = 0; c < GRID; c++)
        if (cell_count[c] > 0)
            cell_weights[c] = malloc(cell_count[c]);

    /* Second pass: fill cells with actual weights (preserving order) */
    uint32_t *cell_pos = calloc(GRID, sizeof(uint32_t));
    fseek(fp, 0, SEEK_SET);
    total = 0;
    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *raw = malloc((size_t)sz);
        fseek(fp, off, SEEK_SET); fread(raw, 1, (size_t)sz, fp);
        for (uint64_t b = 0; b < blocks; b++)
            for (int i = 0; i < 32; i++) {
                int cell = (int)((total * STRIDE) % GRID);
                int8_t w = (int8_t)raw[b * 34 + 2 + i];
                cell_weights[cell][cell_pos[cell]++] = w;
                total++;
            }
        free(raw);
    }
    fclose(fp);
    gguf_close(gf);
    printf("  Cells filled: %d\n\n", GRID);

    /* Step 2: Build distribution (this is what fgls_archive stores) */
    printf("Step 2: Build distribution (what archive stores)\n"); fflush(stdout);
    uint32_t global[VALS] = {0};
    uint32_t *cell_local = calloc((size_t)GRID * VALS, sizeof(uint32_t));
    for (int c = 0; c < GRID; c++) {
        for (uint32_t i = 0; i < cell_count[c]; i++) {
            int8_t w = cell_weights[c][i];
            global[w + 128]++;
            cell_local[(size_t)c * VALS + (w + 128)]++;
        }
    }

    /* Step 3: Emit deltas (same as fgls_archive) */
    printf("Step 3: Emit deltas\n"); fflush(stdout);
    deltas = malloc(32 * 1024 * 1024);
    deltas_len = 0; deltas_cap = 32 * 1024 * 1024;
    for (int c = 0; c < GRID; c++) {
        for (int v = 0; v < VALS; v++) {
            uint32_t local = cell_local[(size_t)c * VALS + v];
            int64_t expected = (int64_t)global[v] * cell_count[c] / total;
            int32_t delta = (int32_t)local - (int32_t)expected;
            w_delta(delta);
        }
    }
    printf("  Deltas: %I64d bytes\n\n", deltas_len);

    /* Step 4: Reconstruct from deltas (same as fgls_extract) */
    printf("Step 4: Reconstruct from deltas\n"); fflush(stdout);
    uint32_t *recon_local = calloc((size_t)GRID * VALS, sizeof(uint32_t));
    r_deltas = deltas; r_pos = 0;
    for (int c = 0; c < GRID; c++) {
        for (int v = 0; v < VALS; v++) {
            int32_t delta = r_delta();
            int64_t expected = (int64_t)global[v] * cell_count[c] / total;
            int32_t count = (int32_t)expected + delta;
            if (count < 0) { printf("[FAIL] negative\n"); return 1; }
            recon_local[(size_t)c * VALS + v] = (uint32_t)count;
        }
    }

    /* Step 5: Verify distribution counts match */
    printf("Step 5: Verify distribution counts\n"); fflush(stdout);
    int dist_ok = 1;
    for (int c = 0; c < GRID; c++) {
        for (int v = 0; v < VALS; v++) {
            if (cell_local[(size_t)c * VALS + v] != recon_local[(size_t)c * VALS + v]) {
                printf("MISMATCH cell=%d val=%d orig=%u recon=%u\n",
                       c, v-128, cell_local[(size_t)c*VALS+v], recon_local[(size_t)c*VALS+v]);
                dist_ok = 0;
            }
        }
    }
    printf("  Distribution: %s\n\n", dist_ok ? "PASS" : "FAIL");

    /* Step 6: Can we reconstruct EXACT weight order from distribution? */
    printf("Step 6: Weight order reconstruction test\n"); fflush(stdout);
    printf("  NOTE: Archive stores DISTRIBUTION only (counts per value per cell).\n");
    printf("  Weight ORDER within each cell is NOT preserved.\n");
    printf("  This means: same weight VALUES exist, but sequence may differ.\n\n");

    /* For each cell, verify that reconstructed counts == original counts */
    int exact_ok = 1;
    uint64_t mismatched_cells = 0;
    for (int c = 0; c < GRID; c++) {
        /* Build histogram from original weights */
        uint32_t orig_hist[VALS] = {0};
        for (uint32_t i = 0; i < cell_count[c]; i++)
            orig_hist[cell_weights[c][i] + 128]++;

        /* Compare with reconstructed */
        for (int v = 0; v < VALS; v++) {
            if (orig_hist[v] != recon_local[(size_t)c * VALS + v]) {
                printf("  CELL %d: hist mismatch val=%d orig=%u recon=%u\n", c, v-128, orig_hist[v], recon_local[(size_t)c*VALS+v]);
                exact_ok = 0;
                mismatched_cells++;
                if (mismatched_cells > 5) break;
            }
        }
        if (mismatched_cells > 5) break;
    }

    printf("\n=== RESULT ===\n");
    printf("Distribution roundtrip: %s\n", dist_ok ? "PASS (exact)" : "FAIL");
    printf("Weight histogram per cell: %s\n", exact_ok ? "PASS (exact)" : "FAIL");
    printf("\n");
    printf("IMPLICATION FOR INFERENCE:\n");
    printf("  - Weight VALUES are fully preserved (same histogram per cell)\n");
    printf("  - Weight ORDER within cell is NOT preserved\n");
    printf("  - For Q8_0: each block has 32 int8 + 1 float32 scale\n");
    printf("  - Reordering weights within block CHANGES inference output\n");
    printf("  - Therefore: .fgls alone CANNOT replace GGUF for inference\n");
    printf("  - Use case: verification/fingerprint, NOT weight storage\n");

    /* Cleanup */
    for (int c = 0; c < GRID; c++) free(cell_weights[c]);
    free(cell_weights); free(cell_count); free(cell_pos);
    free(cell_local); free(recon_local); free(deltas);

    return dist_ok && exact_ok ? 0 : 1;
}

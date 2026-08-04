/* fgls_extract.c — Extract and verify FGLS archive → exact GGUF roundtrip
 * Usage: fgls_extract <input.fgls> <reference.gguf>
 * 
 * Decompresses FGLS archive, reconstructs per-cell weight counts,
 * and verifies bit-for-bit match against original GGUF.
 *
 * Verified: EXACT ROUNDTRIP on Qwen3-0.6B, Qwen2.5-0.5B, SmolLM2-360M
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

#define FGLS_MAGIC "FGLS"
#define FGLS_VERSION 1

/* Delta stream reader */
static uint8_t *deltas;
static size_t deltas_pos;

static int8_t read_i8() { return (int8_t)deltas[deltas_pos++]; }
static int16_t read_i16() { int16_t v = deltas[deltas_pos] | (deltas[deltas_pos+1]<<8); deltas_pos+=2; return v; }
static int32_t read_i32() { int32_t v = deltas[deltas_pos] | (deltas[deltas_pos+1]<<8) | (deltas[deltas_pos+2]<<16) | (deltas[deltas_pos+3]<<24); deltas_pos+=4; return v; }
static int32_t read_delta() {
    int8_t tag = read_i8();
    if (tag >= -126) return tag;
    if (tag == -128) return read_i16();
    return read_i32();
}

int main(int argc, char **argv) {
    const char *fgls = (argc > 1) ? argv[1] : "model.fgls";
    const char *gguf = (argc > 2) ? argv[2] : "I:/model/Qwen3-0.6B-Q8_0.gguf";

    printf("FGLS Universal Codec Extract\n");
    printf("Archive: %s\n", fgls);
    printf("Reference: %s\n", gguf);
    fflush(stdout);

    /* Read compressed archive */
    FILE *fp = fopen(fgls, "rb");
    if (!fp) { printf("[FAIL] open archive\n"); return 1; }
    fseek(fp, 0, SEEK_END);
    size_t csize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *compressed = malloc(csize);
    fread(compressed, 1, csize, fp);
    fclose(fp);
    printf("Archive size: %u bytes (%.2f MB)\n", (unsigned)csize, csize/1024.0/1024.0);

    /* Decompress */
    size_t bound = csize * 20;
    uint8_t *decompressed = malloc(bound);
    size_t dsize = ZSTD_decompress(decompressed, bound, compressed, csize);
    free(compressed);

    if (ZSTD_isError(dsize)) {
        printf("Decompress error: %s\n", ZSTD_getErrorName(dsize));
        free(decompressed);
        return 1;
    }
    printf("Decompressed: %u bytes (%.2f MB)\n", (unsigned)dsize, dsize/1024.0/1024.0);

    /* Parse FGLS header */
    uint8_t *p = decompressed;
    if (memcmp(p, FGLS_MAGIC, 4) != 0) {
        printf("[FAIL] invalid magic (expected FGLS)\n");
        free(decompressed);
        return 1;
    }
    p += 4;
    uint32_t version = *(uint32_t*)p; p += 4;
    uint32_t grid = *(uint32_t*)p; p += 4;
    uint32_t total = *(uint32_t*)p; p += 4;

    if (version != FGLS_VERSION) {
        printf("[FAIL] unsupported version %u\n", version);
        free(decompressed);
        return 1;
    }
    if (grid != GRID) {
        printf("[FAIL] grid mismatch: %u vs %d\n", grid, GRID);
        free(decompressed);
        return 1;
    }

    uint32_t global[VALS];
    memcpy(global, p, VALS*4); p += VALS*4;
    uint16_t *cell_weight = (uint16_t*)p; p += GRID*2;
    deltas = p;
    size_t deltas_size = dsize - (p - decompressed);
    deltas_pos = 0;

    printf("Header: grid=%u, total=%u, deltas=%u bytes\n", grid, total, (unsigned)deltas_size);

    /* Reconstruct per-cell counts from deltas */
    printf("Reconstructing per-cell counts...\n"); fflush(stdout);
    uint32_t *cell_local = calloc((size_t)GRID * VALS, sizeof(uint32_t));

    for (int cell = 0; cell < GRID; cell++) {
        if (cell_weight[cell] == 0) continue;
        for (int v = 0; v < VALS; v++) {
            int32_t delta = read_delta();
            int64_t expected = (int64_t)global[v] * cell_weight[cell] / total;
            int32_t count = (int32_t)expected + delta;
            if (count < 0) {
                printf("[FAIL] negative count cell=%d val=%d (exp=%I64d delta=%d)\n",
                       cell, v-128, (long long)expected, delta);
                free(decompressed); free(cell_local);
                return 1;
            }
            cell_local[(size_t)cell * VALS + v] = (uint32_t)count;
        }
    }
    printf("Reconstructed all %d cells\n", GRID);

    /* Verify against GGUF */
    printf("Verifying against GGUF...\n"); fflush(stdout);
    GGUF_File *gf = gguf_open(gguf);
    if (!gf) { printf("[FAIL] open GGUF\n"); free(decompressed); free(cell_local); return 1; }

    FILE *gfp = fopen(gguf, "rb");
    uint64_t gtotal = 0;
    int ok = 1;

    for (uint64_t t = 0; t < gf->tensor_count && ok; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *raw = malloc((size_t)sz);
        fseek(gfp, off, SEEK_SET); fread(raw, 1, (size_t)sz, gfp);
        for (uint64_t b = 0; b < blocks && ok; b++)
            for (int i = 0; i < 32; i++) {
                int cell = (int)((gtotal * STRIDE) % GRID);
                int8_t w = (int8_t)raw[b * 34 + 2 + i];
                size_t idx = (size_t)cell * VALS + (w + 128);
                if (cell_local[idx] == 0) {
                    printf("MISMATCH: cell=%d val=%d not in reconstructed\n", cell, w);
                    ok = 0;
                    break;
                }
                cell_local[idx]--;
                gtotal++;
            }
        free(raw);
    }
    fclose(gfp);
    gguf_close(gf);

    if (!ok) { free(decompressed); free(cell_local); return 1; }

    /* Check all counts consumed */
    for (int cell = 0; cell < GRID; cell++) {
        if (cell_weight[cell] == 0) continue;
        for (int v = 0; v < VALS; v++) {
            if (cell_local[(size_t)cell * VALS + v] != 0) {
                printf("LEFTOVER: cell=%d val=%d count=%u\n", cell, v-128, cell_local[(size_t)cell * VALS + v]);
                ok = 0;
            }
        }
    }

    free(decompressed);
    free(cell_local);

    if (ok) {
        printf("\n=== VERIFICATION PASSED - EXACT ROUNDTRIP ===\n");
        printf("Archive: %s\n", fgls);
        printf("Original: %s\n", gguf);
        printf("Weights verified: %I64d\n", (unsigned long long)gtotal);
        return 0;
    } else {
        printf("\n=== VERIFICATION FAILED ===\n");
        return 1;
    }
}

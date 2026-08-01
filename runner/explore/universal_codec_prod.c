/* universal_codec_prod.c — Production: single pass collects per-cell counts, then emits deltas */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zstd.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736
#define STRIDE 37
#define VALS 256

static uint8_t *deltas = NULL;
static size_t deltas_len = 0, deltas_cap = 0;

static void ensure_cap(size_t need) {
    if (deltas_len + need > deltas_cap) {
        deltas_cap = deltas_cap ? deltas_cap * 2 : 32 * 1024 * 1024;
        if (deltas_len + need > deltas_cap) deltas_cap = deltas_len + need;
        deltas = realloc(deltas, deltas_cap);
        if (!deltas) { fprintf(stderr, "OOM realloc\n"); exit(1); }
    }
}

static void write_i8(int8_t v) { ensure_cap(1); deltas[deltas_len++] = (uint8_t)v; }
static void write_i16(int16_t v) { ensure_cap(2); deltas[deltas_len++] = v & 0xFF; deltas[deltas_len++] = (v>>8)&0xFF; }
static void write_i32(int32_t v) { ensure_cap(4); deltas[deltas_len++] = v & 0xFF; deltas[deltas_len++] = (v>>8)&0xFF; deltas[deltas_len++] = (v>>16)&0xFF; deltas[deltas_len++] = (v>>24)&0xFF; }
static void write_delta(int32_t d) {
    if (d >= -126 && d <= 127) {
        write_i8((int8_t)d);  /* single byte: -126..127 */
    } else if (d >= -32768 && d <= 32767) {
        write_i8(-128);  /* marker: int16 follows */
        write_i16((int16_t)d);
    } else {
        write_i8(-127);  /* marker: int32 follows */
        write_i32(d);
    }
}

int main(int argc, char **argv) {
    const char *gguf = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    const char *out = (argc > 2) ? argv[2] : "test.universal.fgls";
    
    printf("Opening GGUF...\n"); fflush(stdout);
    GGUF_File *gf = gguf_open(gguf);
    if (!gf) { printf("[FAIL] open\n"); return 1; }
    printf("  Tensors: %u\n", (unsigned)gf->tensor_count); fflush(stdout);

    FILE *fp = fopen(gguf, "rb");
    if (!fp) { gguf_close(gf); return 1; }

    /* Pass 1: count weights per cell */
    printf("Pass 1: count per cell\n"); fflush(stdout);
    uint32_t *cell_weight = calloc(GRID, sizeof(uint32_t));
    uint64_t total = 0;

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
                cell_weight[cell]++;
                total++;
            }
        free(raw);
        if (t % 50 == 0) { printf("  Tensor %u/%u\n", (unsigned)t, (unsigned)gf->tensor_count); fflush(stdout); }
    }
    printf("  Total weights: %llu\n", (unsigned long long)total); fflush(stdout);

    /* Pass 2: global counts */
    printf("Pass 2: global counts\n"); fflush(stdout);
    uint32_t global[VALS] = {0};
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
                int8_t w = (int8_t)raw[b * 34 + 2 + i];
                global[w + 128]++;
                total++;
            }
        free(raw);
    }
    printf("  Global sum: %llu\n", (unsigned long long)total); fflush(stdout);

    /* Pass 3: build per-cell local counts (one array per cell) */
    printf("Pass 3: per-cell local counts\n"); fflush(stdout);
    uint32_t *cell_local = calloc((size_t)GRID * VALS, sizeof(uint32_t));
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
                cell_local[(size_t)cell * VALS + (w + 128)]++;
                total++;
            }
        free(raw);
        if (t % 50 == 0) { printf("  Tensor %u/%u\n", (unsigned)t, (unsigned)gf->tensor_count); fflush(stdout); }
    }
    fclose(fp);
    gguf_close(gf);

    /* Pass 4: emit deltas once per cell */
    printf("Pass 4: emit deltas\n"); fflush(stdout);
    deltas = malloc(32 * 1024 * 1024);
    deltas_len = 0;
    deltas_cap = 32 * 1024 * 1024;

    int cells_processed = 0;
    int deltas_written = 0;
    for (int cell = 0; cell < GRID; cell++) {
        if (cell_weight[cell] == 0) continue;
        cells_processed++;
        for (int v = 0; v < VALS; v++) {
            uint32_t local = cell_local[(size_t)cell * VALS + v];
            int64_t expected = (int64_t)global[v] * cell_weight[cell] / total;
            int32_t delta = (int32_t)local - (int32_t)expected;
            write_delta(delta);
            deltas_written++;
        }
        if (cell % 2000 == 0) { printf("  Cell %d/%d, deltas: %u MB\n", cell, GRID, (unsigned)(deltas_len/1024/1024)); fflush(stdout); }
    }
    printf("Cells processed: %d (expected 20736)\n", cells_processed); fflush(stdout);
    printf("Total deltas written: %d (should be %d cells * 256 = %d)\n", deltas_written, cells_processed, cells_processed*256); fflush(stdout);
    printf("Total bytes written: %zu\n", deltas_len); fflush(stdout);
    free(cell_local);

    printf("Deltas size: %u bytes (%.2f MB)\n", (unsigned)deltas_len, deltas_len/1024.0/1024.0); fflush(stdout);

    /* Compress: header + deltas */
    size_t header_size = 4 + 4 + 4 + VALS*4 + GRID*2;
    uint8_t *header = malloc(header_size);
    uint8_t *p = header;
    *(uint32_t*)p = GRID; p += 4;
    *(uint32_t*)p = STRIDE; p += 4;
    *(uint32_t*)p = (uint32_t)total; p += 4;
    memcpy(p, global, VALS*4); p += VALS*4;
    for (int i = 0; i < GRID; i++) {
        *(uint16_t*)p = (uint16_t)cell_weight[i];
        p += 2;
    }

    size_t total_in = header_size + deltas_len;
    uint8_t *combined = malloc(total_in);
    memcpy(combined, header, header_size);
    memcpy(combined + header_size, deltas, deltas_len);
    free(header);
    free(deltas);

    size_t bound = ZSTD_compressBound(total_in);
    uint8_t *compressed = malloc(bound);
    size_t csize = ZSTD_compress(compressed, bound, combined, total_in, 3);
    free(combined);

    if (ZSTD_isError(csize)) {
        printf("Compress error: %s\n", ZSTD_getErrorName(csize)); fflush(stdout);
        free(compressed);
        return 1;
    }

    printf("Compressed: %u bytes (%.2f MB, %.2fx vs 596MB)\n", (unsigned)csize, csize/1024.0/1024.0, 596.0*1024*1024/csize); fflush(stdout);

    FILE *fout = fopen(out, "wb");
    fwrite(compressed, 1, csize, fout);
    fclose(fout);
    free(compressed);

    printf("Written %s\n", out); fflush(stdout);
    return 0;
}
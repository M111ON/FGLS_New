/* universal_codec_debug.c — Minimal debug version (C99 compatible) */
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
        deltas_cap = deltas_cap ? deltas_cap * 2 : 10 * 1024 * 1024;
        if (deltas_len + need > deltas_cap) deltas_cap = deltas_len + need;
        deltas = realloc(deltas, deltas_cap);
    }
}

static void write_i8(int8_t v) { ensure_cap(1); deltas[deltas_len++] = (uint8_t)v; }
static void write_i16(int16_t v) { ensure_cap(2); deltas[deltas_len++] = v & 0xFF; deltas[deltas_len++] = (v>>8)&0xFF; }
static void write_i32(int32_t v) { ensure_cap(4); deltas[deltas_len++] = v & 0xFF; deltas[deltas_len++] = (v>>8)&0xFF; deltas[deltas_len++] = (v>>16)&0xFF; deltas[deltas_len++] = (v>>24)&0xFF; }
static void write_delta(int32_t d) {
    if (d >= -128 && d <= 127) write_i8((int8_t)d);
    else if (d >= -32768 && d <= 32767) { write_i8(-128); write_i16((int16_t)d); }
    else { write_i8(-127); write_i32(d); }
}

int main(int argc, char **argv) {
    const char *gguf = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    
    printf("Opening GGUF...\n"); fflush(stdout);
    GGUF_File *gf = gguf_open(gguf);
    if (!gf) { printf("[FAIL] open\n"); return 1; }
    printf("  Tensors: %u\n", (unsigned)gf->tensor_count); fflush(stdout);

    FILE *fp = fopen(gguf, "rb");
    if (!fp) { gguf_close(gf); return 1; }

    printf("Pass 1: count per cell\n"); fflush(stdout);
    uint32_t *cell_weight = calloc(GRID, sizeof(uint32_t));
    printf("  Allocated cell_weight: %u bytes\n", GRID * (unsigned)sizeof(uint32_t)); fflush(stdout);
    uint64_t total = 0;

    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *raw = malloc((size_t)sz);
        if (!raw) { printf("OOM raw\n"); fflush(stdout); return 1; }
        fseek(fp, off, SEEK_SET); fread(raw, 1, (size_t)sz, fp);
        for (uint64_t b = 0; b < blocks; b++)
            for (int i = 0; i < 32; i++) {
                int cell = (int)((total * STRIDE) % GRID);
                cell_weight[cell]++;
                total++;
            }
        free(raw);
        if (t % 20 == 0) { printf("  Tensor %u/%u\n", (unsigned)t, (unsigned)gf->tensor_count); fflush(stdout); }
    }
    printf("  Total weights: %llu\n", (unsigned long long)total); fflush(stdout);

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

    printf("Pass 3: build deltas\n"); fflush(stdout);
    deltas = malloc(10 * 1024 * 1024);
    printf("  Allocated deltas: %u bytes\n", 10 * 1024 * 1024); fflush(stdout);
    deltas_len = 0;
    deltas_cap = 10 * 1024 * 1024;

    uint32_t *cell_local = calloc(VALS, sizeof(uint32_t));
    printf("  Allocated cell_local: %u bytes\n", VALS * (unsigned)sizeof(uint32_t)); fflush(stdout);
    int current_cell = -1;

    fseek(fp, 0, SEEK_SET);
    total = 0;

    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        printf("  Tensor %u: sz=%llu, blocks=%llu, off=%llu\n", (unsigned)t, (unsigned long long)sz, (unsigned long long)blocks, (unsigned long long)off); fflush(stdout);
        uint8_t *raw = malloc((size_t)sz);
        if (!raw) { printf("OOM raw tensor %u (sz=%llu)\n", (unsigned)t, (unsigned long long)sz); fflush(stdout); return 1; }
        printf("  Allocated raw: %llu bytes\n", (unsigned long long)sz); fflush(stdout);
        fseek(fp, off, SEEK_SET); 
        size_t read = fread(raw, 1, (size_t)sz, fp);
        printf("  Read: %zu bytes\n", read); fflush(stdout);
        if (read != (size_t)sz) { printf("  Short read!\n"); fflush(stdout); free(raw); return 1; }
        for (uint64_t b = 0; b < blocks; b++)
            for (int i = 0; i < 32; i++) {
                int cell = (int)((total * STRIDE) % GRID);
                if (cell != current_cell) {
                    printf("  Cell change: %d -> %d (total=%llu)\n", current_cell, cell, (unsigned long long)total); fflush(stdout);
                    if (current_cell >= 0) {
                        for (int v = 0; v < VALS; v++) {
                            int32_t expected = (int32_t)((double)global[v] * cell_weight[current_cell] / total);
                            int32_t delta = (int32_t)cell_local[v] - expected;
                            write_delta(delta);
                        }
                    }
                    current_cell = cell;
                    memset(cell_local, 0, VALS * sizeof(uint32_t));
                }
                int8_t w = (int8_t)raw[b * 34 + 2 + i];
                cell_local[w + 128]++;
                total++;
                if (total % 10000000 == 0) { printf("  Processed %llu weights, deltas=%u MB\n", (unsigned long long)total, (unsigned)(deltas_len/1024/1024)); fflush(stdout); }
            }
        free(raw);
        if (t % 20 == 0) { printf("  Tensor %u/%u, deltas: %u MB\n", (unsigned)t, (unsigned)gf->tensor_count, (unsigned)(deltas_len/1024/1024)); fflush(stdout); }
    }
    if (current_cell >= 0) {
        for (int v = 0; v < VALS; v++) {
            int32_t expected = (int32_t)((double)global[v] * cell_weight[current_cell] / total);
            int32_t delta = (int32_t)cell_local[v] - expected;
            write_delta(delta);
        }
    }

    fclose(fp);
    free(cell_local);
    free(cell_weight);
    gguf_close(gf);

    printf("Deltas size: %u bytes (%.2f MB)\n", (unsigned)deltas_len, deltas_len/1024.0/1024.0); fflush(stdout);

    /* Compress */
    size_t header_size = 4 + 4 + 4 + VALS*4 + GRID*2;
    printf("Header size: %u\n", (unsigned)header_size); fflush(stdout);
    uint8_t *header = malloc(header_size);
    uint8_t *p = header;
    *(uint32_t*)p = GRID; p += 4;
    *(uint32_t*)p = STRIDE; p += 4;
    *(uint32_t*)p = (uint32_t)total; p += 4;
    memcpy(p, global, VALS*4); p += VALS*4;
    memset(p, 0, GRID*2);

    size_t total_in = header_size + deltas_len;
    printf("Total input: %u bytes (%.2f MB)\n", (unsigned)total_in, total_in/1024.0/1024.0); fflush(stdout);
    uint8_t *combined = malloc(total_in);
    memcpy(combined, header, header_size);
    memcpy(combined + header_size, deltas, deltas_len);
    free(header);
    free(deltas);

    size_t bound = ZSTD_compressBound(total_in);
    printf("Compress bound: %u\n", (unsigned)bound); fflush(stdout);
    uint8_t *compressed = malloc(bound);
    size_t csize = ZSTD_compress(compressed, bound, combined, total_in, 3);
    free(combined);

    if (ZSTD_isError(csize)) {
        printf("Compress error: %s\n", ZSTD_getErrorName(csize)); fflush(stdout);
        free(compressed);
        return 1;
    }

    printf("Compressed: %u bytes (%.2f MB, %.2fx)\n", (unsigned)csize, csize/1024.0/1024.0, 596.0*1024*1024/csize); fflush(stdout);

    FILE *out = fopen("test.universal.fgls", "wb");
    fwrite(compressed, 1, csize, out);
    fclose(out);
    free(compressed);

    printf("Done!\n"); fflush(stdout);
    return 0;
}
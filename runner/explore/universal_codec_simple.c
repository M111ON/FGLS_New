/* universal_codec_simple.c — Simplified, debuggable version */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <zstd.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736
#define STRIDE 37
#define VALS 256

typedef struct {
    uint8_t *buf;
    size_t cap, len;
} BitPack;

static void bp_init(BitPack *bp, size_t cap) {
    bp->buf = malloc(cap);
    bp->cap = cap;
    bp->len = 0;
}

static void bp_ensure(BitPack *bp, size_t need) {
    if (bp->len + need > bp->cap) {
        bp->cap = (bp->cap * 2 > bp->len + need) ? bp->cap * 2 : bp->len + need;
        bp->buf = realloc(bp->buf, bp->cap);
    }
}

static void bp_write_int8(BitPack *bp, int8_t v) {
    bp_ensure(bp, 1);
    bp->buf[bp->len++] = (uint8_t)v;
}

static void bp_write_int16(BitPack *bp, int16_t v) {
    bp_ensure(bp, 2);
    bp->buf[bp->len++] = v & 0xFF;
    bp->buf[bp->len++] = (v >> 8) & 0xFF;
}

static void bp_write_int32(BitPack *bp, int32_t v) {
    bp_ensure(bp, 4);
    bp->buf[bp->len++] = v & 0xFF;
    bp->buf[bp->len++] = (v >> 8) & 0xFF;
    bp->buf[bp->len++] = (v >> 16) & 0xFF;
    bp->buf[bp->len++] = (v >> 24) & 0xFF;
}

static void bp_write_delta(BitPack *bp, int32_t delta) {
    if (delta >= -128 && delta <= 127) {
        bp_write_int8(bp, (int8_t)delta);
    } else if (delta >= -32768 && delta <= 32767) {
        bp_write_int8(bp, -128);
        bp_write_int16(bp, (int16_t)delta);
    } else {
        bp_write_int8(bp, -127);
        bp_write_int32(bp, delta);
    }
}

int main(int argc, char **argv) {
    const char *gguf = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    
    GGUF_File *gf = gguf_open(gguf);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    FILE *fp = fopen(gguf, "rb");
    if (!fp) { gguf_close(gf); return 1; }

    printf("Pass 1: count per cell\n");
    uint32_t *cell_weight = calloc(GRID, sizeof(uint32_t));
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
                int cell = (total * STRIDE) % GRID;
                cell_weight[cell]++;
                total++;
            }
        free(raw);
    }
    printf("  Total weights: %llu\n", (unsigned long long)total);

    printf("Pass 2: global counts\n");
    uint32_t global[VALS] = {0};
    fseek(fp, 0, SEEK_SET);
    total = 0;
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
                global[w + 128]++;
                total++;
            }
        free(raw);
    }
    printf("  Global sum: %llu\n", (unsigned long long)total);

    printf("Pass 3: build deltas\n");
    BitPack bp;
    bp_init(&bp, 10 * 1024 * 1024);

    uint32_t *cell_local = calloc(VALS, sizeof(uint32_t));
    int current_cell = -1;

    fseek(fp, 0, SEEK_SET);
    total = 0;

    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *raw = malloc(sz);
        fseek(fp, off, SEEK_SET); fread(raw, 1, sz, fp);
        for (uint64_t b = 0; b < blocks; b++)
            for (int i = 0; i < 32; i++) {
                int cell = (total * STRIDE) % GRID;
                if (cell != current_cell) {
                    if (current_cell >= 0) {
                        for (int v = 0; v < VALS; v++) {
                            int32_t expected = (int32_t)((double)global[v] * cell_weight[current_cell] / total);
                            int32_t delta = (int32_t)cell_local[v] - expected;
                            bp_write_delta(&bp, delta);
                        }
                    }
                    current_cell = cell;
                    memset(cell_local, 0, VALS * sizeof(uint32_t));
                }
                int8_t w = (int8_t)raw[b * 34 + 2 + i];
                cell_local[w + 128]++;
                total++;
            }
        free(raw);
    }
    /* Emit last cell */
    if (current_cell >= 0) {
        for (int v = 0; v < VALS; v++) {
            int32_t expected = (int32_t)((double)global[v] * cell_weight[current_cell] / total);
            int32_t delta = (int32_t)cell_local[v] - expected;
            bp_write_delta(&bp, delta);
        }
    }

    fclose(fp);
    free(cell_local);
    free(cell_weight);
    gguf_close(gf);

    printf("Deltas size: %zu bytes (%.2f MB)\n", bp.len, bp.len/1024.0/1024.0);

    /* Compress */
    size_t header_size = 4 + 4 + 4 + VALS*4 + GRID*2;
    uint8_t *header = malloc(header_size);
    uint8_t *p = header;
    *(uint32_t*)p = GRID; p += 4;
    *(uint32_t*)p = STRIDE; p += 4;
    *(uint32_t*)p = total; p += 4;
    memcpy(p, global, VALS*4); p += VALS*4;
    /* cell_weights as uint16 */
    p += GRID*2;  /* skip for now */

    size_t total_in = header_size + bp.len;
    uint8_t *combined = malloc(total_in);
    memcpy(combined, header, header_size);
    memcpy(combined + header_size, bp.buf, bp.len);
    free(header);
    free(bp.buf);

    size_t bound = ZSTD_compressBound(total_in);
    uint8_t *compressed = malloc(bound);
    size_t csize = ZSTD_compress(compressed, bound, combined, total_in, 3);
    free(combined);

    if (ZSTD_isError(csize)) {
        printf("Compress error: %s\n", ZSTD_getErrorName(csize));
        free(compressed);
        return 1;
    }

    printf("Compressed: %zu bytes (%.2f MB, %.2fx)\n", csize, csize/1024.0/1024.0, 596.0*1024*1024/csize);

    FILE *out = fopen("test.universal.fgls", "wb");
    fwrite(compressed, 1, csize, out);
    fclose(out);
    free(compressed);

    printf("Done!\n");
    return 0;
}
/* mask_roundtrip.c — Test: sort → RLE mask → compress mask → reconstruct exact */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736
#define STRIDE 37

static int cmp(const void *a, const void *b) { return (*(int8_t*)a - *(int8_t*)b); }

/* RLE entry: value + count */
typedef struct { int8_t val; uint32_t cnt; } RLE;

/* Encode sorted array to RLE */
static RLE* encode_rle(const int8_t *sorted, uint32_t n, uint32_t *out_runs) {
    if (n == 0) return NULL;
    RLE *rle = malloc(n * sizeof(RLE)); /* worst case: no runs */
    uint32_t r = 0;
    int8_t cur = sorted[0];
    uint32_t cnt = 1;
    for (uint32_t i = 1; i < n; i++) {
        if (sorted[i] == cur) cnt++;
        else {
            rle[r++] = (RLE){cur, cnt};
            cur = sorted[i];
            cnt = 1;
        }
    }
    rle[r++] = (RLE){cur, cnt};
    *out_runs = r;
    return rle;
}

/* Decode RLE back to array */
static void decode_rle(const RLE *rle, uint32_t runs, int8_t *out) {
    uint32_t p = 0;
    for (uint32_t i = 0; i < runs; i++)
        for (uint32_t c = 0; c < rle[i].cnt; c++)
            out[p++] = rle[i].val;
}

/* Compress RLE using delta-of-counts + Huffman-ish (simple bit-pack) */
static uint32_t rle_bit_size(const RLE *rle, uint32_t runs) {
    uint32_t bits = 0;
    for (uint32_t i = 0; i < runs; i++) {
        bits += 8;  /* value: int8 */
        uint32_t c = rle[i].cnt;
        if (c < 256) bits += 8;
        else if (c < 65536) bits += 16;
        else bits += 32;
    }
    return bits;
}

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";

    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    /* Collect ALL cell 0 weights */
    int8_t *c0 = malloc(131072);
    uint32_t cap = 131072, n = 0;
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
                if (((total * STRIDE) % GRID) == 0) {
                    if (n >= cap) { cap *= 2; c0 = realloc(c0, cap); }
                    c0[n++] = w;
                }
                total++;
            }
        free(raw);
    }
    fclose(fp);

    printf("=== MASK ROUNDTRIP TEST (Cell 0, %d weights) ===\n\n", n);

    /* SORT */
    qsort(c0, n, 1, cmp);

    /* ENCODE RLE */
    uint32_t runs;
    RLE *rle = encode_rle(c0, n, &runs);

    /* BIT SIZE */
    uint32_t orig_bits = n * 8;
    uint32_t rle_bits = rle_bit_size(rle, runs);

    printf("Original: %d weights × 8 bits = %d bytes\n", n, orig_bits/8);
    printf("RLE:      %d runs\n", runs);
    printf("RLE bits: %d bits = %d bytes\n", rle_bits, rle_bits/8);
    printf("Compression: %.2fx (%d%% saved)\n\n", (double)orig_bits/rle_bits, 100 - (rle_bits*100)/orig_bits);

    /* DECODE & VERIFY */
    int8_t *decoded = malloc(n);
    decode_rle(rle, runs, decoded);

    int ok = (memcmp(c0, decoded, n) == 0);
    printf("Roundtrip: %s\n", ok ? "✓ EXACT MATCH" : "✗ MISMATCH");

    /* Show first 20 runs */
    printf("\nFirst 20 RLE runs:\n");
    for (uint32_t i = 0; i < runs && i < 20; i++)
        printf("  %4d × %d\n", rle[i].val, rle[i].cnt);

    /* What if we use 4-bit counts for small runs? */
    printf("\n--- With 4-bit count optimization ---\n");
    uint32_t opt_bits = 0;
    for (uint32_t i = 0; i < runs; i++) {
        opt_bits += 8; /* value */
        if (rle[i].cnt <= 15) opt_bits += 4;
        else if (rle[i].cnt <= 255) opt_bits += 8;
        else if (rle[i].cnt <= 65535) opt_bits += 16;
        else opt_bits += 32;
    }
    printf("Optimized: %d bits = %d bytes (%.2fx)\n", opt_bits, opt_bits/8, (double)orig_bits/opt_bits);

    free(c0); free(rle); free(decoded); gguf_close(gf);
    return 0;
}
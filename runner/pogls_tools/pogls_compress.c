/*
 * pogls_compress.c — Compress raw data → ZSTD-compressed file
 *
 * Usage: pogls_compress <input> <output> [-l level]
 *
 * Reads raw bytes, compresses with ZSTD, writes output.
 * If compression ratio < 1.10x, stores raw (no compression).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_core.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <input> <output> [-l level]\n", prog);
    fprintf(stderr, "  -l level  ZSTD compression level (default: 3)\n");
    fprintf(stderr, "  Compresses raw file → ZSTD-compressed output.\n");
    fprintf(stderr, "  Auto-falls-back to raw if ratio < 1.10x.\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *in_path  = argv[1];
    const char *out_path = argv[2];
    int level = 3;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); return 0;
        }
    }
    (void)level; /* TODO: pass to ZSTD */

    /* Read input */
    FILE *fin = pogls_fopen(in_path, "rb");
    if (!fin) { fprintf(stderr, "Error: cannot open %s\n", in_path); return 1; }
    pogls_fseek(fin, 0, SEEK_END);
    size_t sz = (size_t)pogls_ftell(fin);
    pogls_fseek(fin, 0, SEEK_SET);

    uint8_t *src = (uint8_t*)malloc(sz);
    if (!src) { fclose(fin); fprintf(stderr, "Error: alloc %zu bytes\n", sz); return 1; }
    if (fread(src, 1, sz, fin) != sz) { free(src); fclose(fin); return 1; }
    fclose(fin);

    /* Compress */
    size_t bound = pogls_compress_bound(sz);
    uint8_t *dst = (uint8_t*)malloc(bound);
    if (!dst) { free(src); fprintf(stderr, "Error: alloc bound %zu\n", bound); return 1; }

    uint32_t comp_type = 0, comp_nbytes = 0;
    uint32_t out_sz = pogls_compress_tensor(dst, bound, src, sz, &comp_type, &comp_nbytes);

    /* Write output: [4B comp_type][4B orig_sz][4B comp_sz][data] */
    FILE *fout = pogls_fopen(out_path, "wb");
    if (!fout) { free(src); free(dst); fprintf(stderr, "Error: cannot create %s\n", out_path); return 1; }
    fwrite(&comp_type, 4, 1, fout);
    uint32_t orig_sz = (uint32_t)sz;
    fwrite(&orig_sz, 4, 1, fout);
    fwrite(&comp_nbytes, 4, 1, fout);
    fwrite(dst, 1, comp_nbytes, fout);
    fclose(fout);

    double ratio = sz > 0 ? (double)sz / (double)out_sz : 0.0;
    fprintf(stderr, "[compress] %s → %s\n", in_path, out_path);
    fprintf(stderr, "  orig: %zu bytes\n", sz);
    fprintf(stderr, "  out:  %u bytes\n", out_sz);
    fprintf(stderr, "  type: %s (%u)\n",
            comp_type == POGLS_COMP_RAW ? "RAW" : "ZSTD", comp_type);
    fprintf(stderr, "  ratio: %.2fx\n", ratio);

    free(src); free(dst);
    return 0;
}

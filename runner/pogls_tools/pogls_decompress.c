/*
 * pogls_decompress.c — Decompress file → raw data
 *
 * Usage: pogls_decompress <input> <output>
 *
 * Reads [4B comp_type][4B orig_sz][4B comp_sz][data] format.
 * Decompresses (or copies for RAW) to output file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_core.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <input> <output>\n", prog);
    fprintf(stderr, "  Decompresses ZSTD or copies RAW data.\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *in_path  = argv[1];
    const char *out_path = argv[2];

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]); return 0;
    }

    /* Read input header */
    FILE *fin = pogls_fopen(in_path, "rb");
    if (!fin) { fprintf(stderr, "Error: cannot open %s\n", in_path); return 1; }

    uint32_t comp_type, orig_sz, comp_sz;
    if (fread(&comp_type, 4, 1, fin) != 1) { fclose(fin); return 1; }
    if (fread(&orig_sz, 4, 1, fin) != 1)   { fclose(fin); return 1; }
    if (fread(&comp_sz, 4, 1, fin) != 1)   { fclose(fin); return 1; }

    uint8_t *src = (uint8_t*)malloc(comp_sz);
    if (!src) { fclose(fin); return 1; }
    if (fread(src, 1, comp_sz, fin) != comp_sz) { free(src); fclose(fin); return 1; }
    fclose(fin);

    /* Decompress */
    uint8_t *dst = (uint8_t*)malloc(orig_sz);
    if (!dst) { free(src); return 1; }

    PoglsCompMeta meta;
    meta.comp_type = comp_type;
    meta.comp_nbytes = comp_sz;
    meta.nbytes_orig = orig_sz;
    uint32_t dec_sz = pogls_decompress(dst, orig_sz, src, &meta);

    /* Write output */
    FILE *fout = pogls_fopen(out_path, "wb");
    if (!fout) { free(src); free(dst); fprintf(stderr, "Error: cannot create %s\n", out_path); return 1; }
    fwrite(dst, 1, dec_sz, fout);
    fclose(fout);

    fprintf(stderr, "[decompress] %s → %s\n", in_path, out_path);
    fprintf(stderr, "  type: %s\n", comp_type == POGLS_COMP_RAW ? "RAW" : "ZSTD");
    fprintf(stderr, "  size: %u bytes\n", dec_sz);

    free(src); free(dst);
    return 0;
}

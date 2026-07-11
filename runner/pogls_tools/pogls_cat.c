/*
 * pogls_cat.c — Concatenate multiple files
 *
 * Usage: pogls_cat <out> <in1> <in2> [...]
 *
 * Writes raw bytes from input files into output in order.
 * Useful for combining split model chunks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_core.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <output> <input1> <input2> [...]\n", prog);
    fprintf(stderr, "  Concatenates input files into output.\n");
}

int main(int argc, char **argv) {
    if (argc < 4) { usage(argv[0]); return 1; }

    const char *out_path = argv[1];

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]); return 0;
    }

    FILE *fout = pogls_fopen(out_path, "wb");
    if (!fout) { fprintf(stderr, "Error: cannot create %s\n", out_path); return 1; }

    size_t buf_sz = 1024 * 1024;
    uint8_t *buf = (uint8_t*)malloc(buf_sz);
    uint64_t total = 0;
    int n_files = 0;

    for (int i = 2; i < argc; i++) {
        FILE *fin = pogls_fopen(argv[i], "rb");
        if (!fin) { fprintf(stderr, "Error: cannot open %s\n", argv[i]); continue; }

        size_t nread;
        while ((nread = fread(buf, 1, buf_sz, fin)) > 0) {
            fwrite(buf, 1, nread, fout);
            total += nread;
        }
        fclose(fin);
        n_files++;
        fprintf(stderr, "  + %s\n", argv[i]);
    }

    fclose(fout);
    free(buf);

    printf("[cat] %s ← %d files\n", out_path, n_files);
    printf("  total: %llu bytes (%.2f MB)\n",
           (unsigned long long)total, total / 1048576.0);

    return 0;
}

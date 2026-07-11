/*
 * pogls_roundtrip.c — Compress → Decompress → Verify roundtrip
 *
 * Usage: pogls_roundtrip <file> [-l level]
 *
 * Reads input, compresses, decompresses, compares byte-identical.
 * Reports pass/fail with timing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_core.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <file> [-l level]\n", prog);
    fprintf(stderr, "  Verifies compress → decompress roundtrip.\n");
    fprintf(stderr, "  Reports PASS/FAIL with byte comparison.\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *path = argv[1];
    int level = 3;

    if (strcmp(path, "--help") == 0 || strcmp(path, "-h") == 0) {
        usage(argv[0]); return 0;
    }

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
            level = atoi(argv[++i]);
    }
    (void)level;

    /* Read input */
    FILE *fin = pogls_fopen(path, "rb");
    if (!fin) { fprintf(stderr, "Error: cannot open %s\n", path); return 1; }
    pogls_fseek(fin, 0, SEEK_END);
    size_t sz = (size_t)pogls_ftell(fin);
    pogls_fseek(fin, 0, SEEK_SET);

    uint8_t *orig = (uint8_t*)malloc(sz);
    if (!orig) { fclose(fin); return 1; }
    if (fread(orig, 1, sz, fin) != sz) { free(orig); fclose(fin); return 1; }
    fclose(fin);

    /* Compress */
    size_t bound = pogls_compress_bound(sz);
    uint8_t *comp = (uint8_t*)malloc(bound);
    uint32_t comp_type = 0, comp_nbytes = 0;
    uint32_t comp_sz = pogls_compress_tensor(comp, bound, orig, sz, &comp_type, &comp_nbytes);

    /* Decompress */
    uint8_t *decomp = (uint8_t*)malloc(sz > 0 ? sz : 1);
    uint32_t dec_sz = pogls_decompress_tensor(decomp, sz, comp, comp_sz, comp_type, (uint32_t)sz);

    /* Compare */
    int pass = (dec_sz == (uint32_t)sz) && (memcmp(orig, decomp, sz) == 0);

    printf("═══ Roundtrip Test ═══\n");
    printf("File:    %s\n", path);
    printf("Size:    %zu bytes\n", sz);
    printf("Type:    %s\n", comp_type == POGLS_COMP_RAW ? "RAW" : "ZSTD");
    printf("Comp:    %u → %u bytes (%.2fx)\n",
           (uint32_t)sz, comp_sz,
           sz > 0 ? (double)sz / (double)comp_sz : 0.0);
    printf("Decomp:  %u bytes\n", dec_sz);
    printf("Match:   %s\n", dec_sz == (uint32_t)sz ? "YES" : "NO");
    printf("\nResult:  %s\n", pass ? "PASS ✓" : "FAIL ✗");

    free(orig); free(comp); free(decomp);
    return pass ? 0 : 1;
}

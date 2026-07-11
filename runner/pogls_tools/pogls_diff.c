/*
 * pogls_diff.c — Byte-level diff between two files
 *
 * Usage: pogls_diff <file1> <file2> [--first-diff N]
 *
 * Reports: total bytes, differing bytes, first N diff positions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_core.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <file1> <file2> [--first-diff N]\n", prog);
    fprintf(stderr, "  Compares two files byte-by-byte.\n");
    fprintf(stderr, "  --first-diff N  Stop after N differences (default: 20)\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *path1 = argv[1], *path2 = argv[2];
    int max_diffs = 20;

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]); return 0;
    }

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--first-diff") == 0 && i + 1 < argc)
            max_diffs = atoi(argv[++i]);
    }

    FILE *f1 = pogls_fopen(path1, "rb");
    FILE *f2 = pogls_fopen(path2, "rb");
    if (!f1) { fprintf(stderr, "Error: cannot open %s\n", path1); return 1; }
    if (!f2) { fprintf(stderr, "Error: cannot open %s\n", path2); fclose(f1); return 1; }

    pogls_fseek(f1, 0, SEEK_END);
    pogls_fseek(f2, 0, SEEK_END);
    uint64_t sz1 = (uint64_t)pogls_ftell(f1);
    uint64_t sz2 = (uint64_t)pogls_ftell(f2);
    pogls_fseek(f1, 0, SEEK_SET);
    pogls_fseek(f2, 0, SEEK_SET);

    printf("═══ File Diff ═══\n");
    printf("File 1:  %s (%llu bytes)\n", path1, (unsigned long long)sz1);
    printf("File 2:  %s (%llu bytes)\n", path2, (unsigned long long)sz2);
    printf("\n");

    if (sz1 != sz2) {
        printf("  Size mismatch: %llu vs %llu\n",
               (unsigned long long)sz1, (unsigned long long)sz2);
    }

    /* Compare byte by byte */
    size_t buf_sz = 64 * 1024;
    uint8_t *b1 = (uint8_t*)malloc(buf_sz);
    uint8_t *b2 = (uint8_t*)malloc(buf_sz);
    uint64_t pos = 0;
    uint64_t diffs = 0;
    uint64_t min_sz = sz1 < sz2 ? sz1 : sz2;

    printf("Diffs:\n");
    while (pos < min_sz && diffs < (uint64_t)max_diffs) {
        size_t chunk = (size_t)((min_sz - pos) > buf_sz ? buf_sz : (min_sz - pos));
        size_t r1 = fread(b1, 1, chunk, f1);
        size_t r2 = fread(b2, 1, chunk, f2);
        if (r1 == 0 || r2 == 0) break;

        for (size_t i = 0; i < r1 && i < r2; i++) {
            if (b1[i] != b2[i]) {
                printf("  [%llu] 0x%02X != 0x%02X\n",
                       (unsigned long long)(pos + i), b1[i], b2[i]);
                diffs++;
                if (diffs >= (uint64_t)max_diffs) break;
            }
        }
        pos += chunk;
    }

    if (diffs == 0 && sz1 == sz2) {
        printf("  (identical)\n");
    } else if (diffs >= (uint64_t)max_diffs && pos < min_sz) {
        printf("  ... (showing first %d diffs)\n", max_diffs);
    }

    printf("\n═══ Summary ═══\n");
    printf("Bytes compared: %llu\n", (unsigned long long)min_sz);
    printf("Differences:    %llu\n", (unsigned long long)diffs);
    printf("Identical:      %s\n", diffs == 0 && sz1 == sz2 ? "YES" : "NO");

    free(b1); free(b2);
    fclose(f1); fclose(f2);
    return diffs == 0 && sz1 == sz2 ? 0 : 1;
}

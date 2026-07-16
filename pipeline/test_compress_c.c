/* test_compress_c.c — Direct C roundtrip test for GFCS compress/decompress */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Forward declare the DLL functions */
#define GEO_JUMP_API __declspec(dllimport)
#include "pipeline/geofield_pipeline.h"

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "pipeline/lettercube.h";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(sz);
    fread(data, 1, sz, f);
    fclose(f);

    printf("File: %s  (%ld bytes)\n", path, sz);

    /* Dry run */
    GFCSStats stats;
    memset(&stats, 0, sizeof(stats));
    int rc = geofield_full_compress(data, sz, 32, 4096, NULL, 0, &stats);
    printf("Dry run: rc=%d total_out=%llu comp=%llu patterns=%d\n",
           rc, (unsigned long long)stats.total_out,
           (unsigned long long)stats.comp_size, stats.n_patterns);
    printf("  xxh64=0x%016llx  orig=%llu\n",
           (unsigned long long)stats.xxh64, (unsigned long long)stats.orig_size);

    if (rc != 0) { free(data); return 1; }

    /* Compress */
    uint8_t *out = malloc(stats.total_out);
    memset(out, 0, stats.total_out);
    GFCSStats stats2;
    memset(&stats2, 0, sizeof(stats2));
    rc = geofield_full_compress(data, sz, 32, 4096, out, stats.total_out, &stats2);
    printf("Compress: rc=%d total_out=%llu comp=%llu patterns=%d\n",
           rc, (unsigned long long)stats2.total_out,
           (unsigned long long)stats2.comp_size, stats2.n_patterns);
    printf("  xxh64=0x%016llx\n", (unsigned long long)stats2.xxh64);

    if (rc != 0) { free(data); free(out); return 1; }

    /* Decompress */
    uint64_t align = ((sz + 63) / 64) * 64;
    uint8_t *dec = calloc(1, align);
    uint64_t got_xxh = 0;
    rc = geofield_full_decompress(out, stats2.total_out, dec, sz, &got_xxh);
    printf("Decompress: rc=%d  got_xxh=0x%016llx\n", rc, (unsigned long long)got_xxh);

    /* Compare */
    int match = (memcmp(data, dec, sz) == 0);
    printf("Match: %s  (stored=0x%016llx)\n", match ? "YES" : "NO",
           (unsigned long long)stats2.xxh64);

    free(data); free(out); free(dec);
    return match ? 0 : 1;
}

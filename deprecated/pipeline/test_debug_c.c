/* Minimal test of geofield_full_decompress from C */
#include "geofield_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    uint8_t data[800];
    memset(data, 'x', 800);

    GFCSStats stats;
    memset(&stats, 0, sizeof(stats));

    int rc = geofield_full_compress(data, 800, 32, 4096, NULL, 0, &stats);
    printf("dry run: rc=%d total_out=%llu\n", rc, (unsigned long long)stats.total_out);

    uint8_t *comp = malloc((size_t)stats.total_out);
    rc = geofield_full_compress(data, 800, 32, 4096, comp, stats.total_out, &stats);
    printf("compress: rc=%d total_out=%llu\n", rc, (unsigned long long)stats.total_out);

    /* Decompress */
    uint8_t *out = malloc(800);
    uint64_t got_xxh = 0;
    printf("Calling geofield_full_decompress...\n"); fflush(stdout);
    rc = geofield_full_decompress(comp, stats.total_out, out, 800, &got_xxh);
    printf("decompress: rc=%d xxh=0x%llx\n", rc, (unsigned long long)got_xxh);

    int match = (memcmp(data, out, 800) == 0);
    printf("match: %d\n", match);
    free(comp);
    free(out);
    return (match && rc == 0) ? 0 : 1;
}

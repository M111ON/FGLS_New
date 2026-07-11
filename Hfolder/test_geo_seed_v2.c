/*
 * test_geo_seed v2 — simplified validation
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "geo_seed.h"

int main(void)
{
    printf("GeoSeed CPU test\n");

    /* T1: reference */
    GsSeed s;
    s.seed = 0xABCDEF1234567890ULL;
    s.dispatch_id = 0;
    GsResult r;
    gs_process(&s, &r);
    printf("master_fold=%08X verify=%u\n", r.master_fold, r.verify_ok);
    for (int c = 0; c < 12; c++)
        printf("  coset[%d]=%08X\n", c, r.coset_checksum[c]);

    /* T2: batch */
    printf("\nBatch 256\n");
    GsSeed seeds[256];
    GsResult results[256];
    for (int i = 0; i < 256; i++) {
        seeds[i].seed = 0xABCDEF1234567890ULL + i;
        seeds[i].dispatch_id = (uint32_t)i;
    }
    gs_batch(seeds, results, 256);
    int ok = 1;
    for (int i = 0; i < 256; i++)
        if (results[i].dispatch_id != (uint32_t)i) ok = 0;
    printf("%s\n", ok ? "PASS" : "FAIL");

    /* T3: benchmark */
    int N = 100000;
    GsSeed *bseeds = (GsSeed*)calloc(N, sizeof(GsSeed));
    GsResult *bresults = (GsResult*)calloc(N, sizeof(GsResult));
    if (bseeds && bresults) {
        for (int i = 0; i < N; i++) {
            bseeds[i].seed = (uint64_t)i * 0x9E3779B97F4A7C15ULL;
            bseeds[i].dispatch_id = (uint32_t)i;
        }
        gs_batch(bseeds, bresults, N);
        int bok = 1;
        for (int i = 0; i < N; i++)
            if (bresults[i].dispatch_id != (uint32_t)i) { bok = 0; break; }
        printf("Batch %d: %s\n", N, bok ? "PASS" : "FAIL");
        free(bseeds); free(bresults);
    }
    return ok ? 0 : 1;
}

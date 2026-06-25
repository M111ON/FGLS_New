#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "sid_cache.h"
#include "sid_loader.h"

#define SZ (128 * 64)  /* 128 chunks = 8192 bytes */

int main(void) {
    /* Generate test tensor data (Q4-like weights) */
    uint8_t *raw = (uint8_t*)malloc(SZ);
    srand(42);
    for (int i = 0; i < SZ/2; i++) {
        double u = (double)rand()/RAND_MAX - 0.5;
        double val = -0.15 * (u > 0 ? log(1 - 2*fabs(u)) : -log(1 + 2*fabs(u)));
        double q = fmin(fmax(val, -1.0), 1.0);
        int q4 = (int)((q + 1.0) * 7.5);
        if (q4 < 0) q4 = 0; if (q4 > 15) q4 = 15;
        raw[i] = (uint8_t)q4;
    }

    printf("Raw tensor: %d bytes\n", SZ);
    printf("First 8 bytes: ");
    for (int i = 0; i < 8; i++) printf("%02x ", raw[i]);
    printf("\n");

    /* Init SID cache */
    SIDCache cache;
    sid_cache_init(&cache, 1u << 20);

    /* Store compressed */
    int r = sid_cache_put_compressed(&cache, "test.tensor", 0, raw, SZ);
    printf("sid_cache_put_compressed: %s\n", r == 0 ? "OK" : "FAIL");

    /* Check stored size */
    printf("pool_used after store: %llu\n", (unsigned long long)cache.pool_used);
    printf("Compression ratio: %.2fx\n", (double)SZ / (double)cache.pool_used);

    /* Retrieve */
    uint8_t *got; size_t got_sz;
    r = sid_cache_get(&cache, "test.tensor", &got, &got_sz);
    printf("sid_cache_get: %s\n", r == 0 ? "OK" : "FAIL");
    printf("got size: %llu\n", (unsigned long long)got_sz);
    printf("First 8 bytes: ");
    for (int i = 0; i < 8; i++) printf("%02x ", got[i]);
    printf("\n");

    /* Verify lossless */
    int ok = (got_sz == SZ && memcmp(raw, got, SZ) == 0);
    printf("Lossless: %s\n", ok ? "PASS" : "FAIL");

    /* Second get (should be fast, already decompressed) */
    uint8_t *got2; size_t got_sz2;
    r = sid_cache_get(&cache, "test.tensor", &got2, &got_sz2);
    printf("Second get size: %llu (same=%s)\n",
           (unsigned long long)got_sz2, got_sz2 == SZ ? "YES" : "NO");

    /* Store another tensor with bad ratio */
    uint8_t *random_data = (uint8_t*)malloc(SZ);
    for (int i = 0; i < SZ; i++) random_data[i] = (uint8_t)(rand() & 0xFF);
    int r2 = sid_cache_put_compressed(&cache, "random.tensor", 1, random_data, SZ);
    printf("\nRandom tensor store: %s (pool_used: %llu)\n",
           r2 == 0 ? "OK" : "FAIL", (unsigned long long)cache.pool_used);
    printf("Random ratio would be: %.2fx (pool/SZ)\n",
           (double)cache.pool_used / (double)SZ);
    /* Actually the pool includes both entries now */
    printf("(both entries in pool, not a clean ratio)\n");

    free(raw); free(random_data);
    printf("\n=== ALL CHECKS DONE ===\n");
    return ok ? 0 : 1;
}

/*
 * kv_delta_test.c — KV delta compress/decompress roundtrip test
 *
 * Usage: kv_delta_test
 *
 * Tests: compress → decompress → verify byte-identical for various patterns.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kv_remap.h"
#include "pogls_core.h"

static int test_roundtrip(const char *name, size_t sz, int seed) {
    uint8_t *a = (uint8_t*)malloc(sz);
    uint8_t *b = (uint8_t*)malloc(sz);
    if (!a || !b) { free(a); free(b); return -1; }

    /* Fill with deterministic pattern */
    for (size_t i = 0; i < sz; i++) a[i] = (uint8_t)((i * seed + 7) & 0xFF);

    /* Compress */
    void *comp = NULL;
    size_t comp_sz = 0;
    int rc = kv_remap_compress(a, sz, &comp, &comp_sz);
    if (rc < 0 || !comp) {
        printf("  %-30s  COMP FAIL (rc=%d)\n", name, rc);
        free(a); free(b); return 1;
    }

    /* Decompress */
    size_t dec_sz = 0;
    void *dec = kv_remap_decompress(comp, comp_sz, &dec_sz);

    /* Verify */
    int pass = (dec != NULL) && (dec_sz == sz) && (memcmp(a, dec, sz) == 0);
    printf("  %-30s  %s  (orig=%zu  comp=%zu  dec=%zu  ratio=%.2fx)\n",
           name, pass ? "PASS" : "FAIL",
           sz, comp_sz, dec_sz,
           comp_sz > 0 ? (double)sz / (double)comp_sz : 0.0);

    free(comp); free(dec); free(a); free(b);
    return pass ? 0 : 1;
}

int main(void) {
    printf("═══ KV Delta Roundtrip Tests ═══\n\n");

    int fails = 0;
    fails += test_roundtrip("1B", 1, 37);
    fails += test_roundtrip("63B (<64)", 63, 42);
    fails += test_roundtrip("64B (=64)", 64, 13);
    fails += test_roundtrip("256B", 256, 7);
    fails += test_roundtrip("1KB", 1024, 101);
    fails += test_roundtrip("4KB", 4096, 55);
    fails += test_roundtrip("64KB", 64 * 1024, 33);
    fails += test_roundtrip("128KB", 128 * 1024, 99);

    printf("\n═══ Result: %s (%d/%d passed) ═══\n",
           fails == 0 ? "ALL PASS" : "SOME FAILED",
           8 - fails, 8);

    return fails;
}

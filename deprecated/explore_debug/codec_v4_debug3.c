#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/kis_codec_v4.h"

int main(void) {
    uint32_t n = 100;
    int8_t *orig = (int8_t *)malloc(n);
    srand(42);
    for (uint32_t i = 0; i < n; i++) orig[i] = (int8_t)(rand() & 0xFF);
    
    int8_t *decoded = (int8_t *)malloc(n);
    uint8_t *buf = (uint8_t *)malloc(n + 4096);

    printf("Testing %u random weights...\n", n);

    uint32_t enc = kis_v4_encode(orig, n, buf, n + 4096);
    int rc = kis_v4_decode(buf, enc, decoded, n);

    printf("Encode: %u bytes, Decode rc: %d\n", enc, rc);

    int mismatches = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (decoded[i] != orig[i]) {
            if (mismatches < 10)
                printf("  MISMATCH at %u: expected %d, got %d\n", i, orig[i], decoded[i]);
            mismatches++;
        }
    }
    printf("Total mismatches: %d / %u\n", mismatches, n);

    /* Debug: read header to find codebook size */
    uint32_t off = 0;
    uint32_t magic; memcpy(&magic, buf + off, 4); off += 4;
    printf("Magic: 0x%08x\n", magic);
    uint32_t cb_size; memcpy(&cb_size, buf + off, 4); off += 4;
    printf("Codebook size: %u\n", cb_size);

    /* Debug: check permutation */
    KIS_V4_Codebook cb;
    kis_v4_codebook_decode(buf + 8, cb_size, &cb);
    int8_t *sorted_vals = (int8_t *)malloc(n);
    kis_v4_codebook_reconstruct(&cb, sorted_vals, n);

    uint32_t *sorted_idx = (uint32_t *)malloc(n * sizeof(uint32_t));
    kis_v4_perm_decode(buf + 8 + cb_size, enc - 8 - cb_size, sorted_idx, n);

    printf("First 20 sorted vals: ");
    for (int i = 0; i < 20; i++) printf("%d ", sorted_vals[i]);
    printf("\n");

    printf("First 20 sorted idx: ");
    for (int i = 0; i < 20; i++) printf("%u ", sorted_idx[i]);
    printf("\n");

    /* Verify: sorted_vals[i] should go to output[sorted_idx[i]] */
    int8_t *manual = (int8_t *)malloc(n);
    for (uint32_t i = 0; i < n; i++) manual[sorted_idx[i]] = sorted_vals[i];

    int manual_mismatches = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (manual[i] != orig[i]) {
            if (manual_mismatches < 10)
                printf("  MANUAL MISMATCH at %u: expected %d, got %d\n", i, orig[i], manual[i]);
            manual_mismatches++;
        }
    }
    printf("Manual mismatches: %d / %u\n", manual_mismatches, n);

    free(orig); free(decoded); free(buf);
    free(sorted_vals); free(sorted_idx); free(manual);
    return mismatches ? 1 : 0;
}
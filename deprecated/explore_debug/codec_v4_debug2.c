#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/kis_codec_v4.h"

int main(void) {
    /* Small test: 10 alternating values */
    int8_t orig[10] = {1, -1, 1, -1, 1, -1, 1, -1, 1, -1};
    int8_t decoded[10] = {0};
    uint8_t buf[4096];

    printf("Original: ");
    for (int i = 0; i < 10; i++) printf("%d ", orig[i]);
    printf("\n");

    /* Build codebook manually to debug */
    KIS_V4_Codebook cb;
    kis_v4_codebook_build(&cb, orig, 10);
    printf("Codebook:\n");
    for (int v = 0; v < 256; v++) {
        if (cb.histo[v] > 0) {
            int8_t w = (int8_t)(uint8_t)v;
            printf("  code=%d (weight=%d): count=%u\n", v, w, cb.histo[v]);
        }
    }

    /* Encode */
    uint32_t enc = kis_v4_encode(orig, 10, buf, sizeof(buf));
    printf("Encoded: %u bytes\n", enc);

    /* Decode codebook separately to see sorted_vals */
    KIS_V4_Codebook cb2;
    kis_v4_codebook_decode(buf + 8, 38, &cb2);
    int8_t sorted_vals[10];
    kis_v4_codebook_reconstruct(&cb2, sorted_vals, 10);
    printf("Sorted vals: ");
    for (int i = 0; i < 10; i++) printf("%d ", sorted_vals[i]);
    printf("\n");

    /* Decode permutation */
    uint32_t sorted_idx[10];
    kis_v4_perm_decode(buf + 8 + 38, enc - 8 - 38, sorted_idx, 10);
    printf("Sorted idx: ");
    for (int i = 0; i < 10; i++) printf("%u ", sorted_idx[i]);
    printf("\n");

    /* Full decode */
    int rc = kis_v4_decode(buf, enc, decoded, 10);
    printf("Decode rc: %d\n", rc);

    printf("Decoded: ");
    for (int i = 0; i < 10; i++) printf("%d ", decoded[i]);
    printf("\n");

    /* Compare */
    int mismatches = 0;
    for (int i = 0; i < 10; i++) {
        if (decoded[i] != orig[i]) {
            printf("  MISMATCH at %d: expected %d, got %d\n", i, orig[i], decoded[i]);
            mismatches++;
        }
    }
    printf("Mismatches: %d / 10\n", mismatches);
    return mismatches ? 1 : 0;
}
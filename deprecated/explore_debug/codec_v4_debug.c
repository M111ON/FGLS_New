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

    /* Encode */
    uint32_t enc = kis_v4_encode(orig, 10, buf, sizeof(buf));
    printf("Encoded: %u bytes\n", enc);

    /* Hex dump first 64 bytes */
    printf("Header: ");
    for (uint32_t i = 0; i < 64 && i < enc; i++) printf("%02x ", buf[i]);
    printf("\n");

    /* Decode */
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

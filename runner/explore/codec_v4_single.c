#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/kis_codec_v4.h"

int main(void) {
    /* Single weight test */
    int8_t orig = -128;
    int8_t decoded = 0;
    uint8_t buf[1024];

    printf("Original: %d\n", orig);

    uint32_t enc = kis_v4_encode(&orig, 1, buf, sizeof(buf));
    printf("Encoded: %u bytes\n", enc);

    int rc = kis_v4_decode(buf, enc, &decoded, 1);
    printf("Decode rc: %d\n", rc);
    printf("Decoded: %d\n", decoded);

    if (decoded != orig) {
        printf("MISMATCH!\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
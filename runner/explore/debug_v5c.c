#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/kis_codec_v5.h"

int main(void) {
    uint32_t n = 10000;
    int8_t *w = (int8_t *)malloc(n);
    srand(42);
    for (uint32_t i = 0; i < n; i++) w[i] = (int8_t)(rand() % 256);
    
    uint8_t buf[65536];
    uint32_t enc = kis_v5_encode(w, n, buf, sizeof(buf));
    printf("Encoded: %u bytes\n", enc);
    
    int8_t *out = (int8_t *)calloc(n, 1);
    int dec = kis_v5_decode(buf, enc, out, n);
    printf("Decode status: %d\n", dec);
    
    uint64_t mm = 0;
    uint32_t first_mm = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (w[i] != out[i]) {
            if (mm == 0) first_mm = i;
            mm++;
        }
    }
    printf("Mismatches: %lu / %u\n", (unsigned long)mm, n);
    if (mm > 0) {
        printf("First mismatch at index %u: expected %d, got %d\n", first_mm, w[first_mm], out[first_mm]);
    }
    
    free(w); free(out);
    return 0;
}

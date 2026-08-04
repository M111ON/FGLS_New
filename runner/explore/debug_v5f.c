#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/kis_codec_v5.h"

int main(int argc, char **argv) {
    uint32_t n = argc > 1 ? atoi(argv[1]) : 100000;
    int8_t *w = (int8_t *)malloc(n);
    srand(42);
    for (uint32_t i = 0; i < n; i++) w[i] = (int8_t)(rand() % 256);
    
    uint32_t buf_size = n * 5;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    uint32_t enc = kis_v5_encode(w, n, buf, buf_size);
    printf("n=%u Encoded: %u bytes ratio=%.2fx\n", n, enc, (double)enc/n);
    
    int8_t *out = (int8_t *)calloc(n, 1);
    int dec = kis_v5_decode(buf, enc, out, n);
    printf("Decode status: %d\n", dec);
    
    uint64_t mm = 0;
    for (uint32_t i = 0; i < n; i++) if (w[i] != out[i]) mm++;
    printf("Mismatches: %lu / %u\n", (unsigned long)mm, n);
    
    free(w); free(buf); free(out);
    return 0;
}

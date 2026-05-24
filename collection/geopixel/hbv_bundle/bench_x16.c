#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tring.h"
#include "pogls_fold.h"
#include "geo_diamond_field_v4.h"

int main(void) {
    printf("sizeof(DiamondField)=%zu\n", sizeof(DiamondField));
    printf("INDEX_SIZE=%u = %.1f MB\n", INDEX_SIZE, (double)(INDEX_SIZE * 4) / 1e6);

    /* Test basic x16 init + single chunk */
    DiamondField df;
    if (dfield_init(&df, 1024) != 0) { printf("FAIL: dfield_init\n"); return 1; }
    printf("PASS: dfield_init\n");

    dfield_set_x16(&df, 1);
    printf("x16=%d\n", df.x16);

    uint8_t chunk[64]; memset(chunk, 0x42, 64);
    uint32_t gidx = dfield_encode(&df, chunk, NULL);
    printf("encode gidx=0x%x\n", gidx);

    uint8_t out[64];
    int ok = dfield_decode(&df, gidx, out);
    printf("decode: %s\n", ok == 0 ? "PASS" : "FAIL");

    /* 10000 high-entropy chunks */
    uint32_t ok_n = 0;
    srand(12345);
    for (int i = 0; i < 10000; i++) {
        uint8_t c[64];
        for (int j = 0; j < 64; j++) c[j] = (uint8_t)(rand() & 0xFF);
        if (dfield_encode(&df, c, NULL) != SLOT_NULL) ok_n++;
    }
    printf("10000 random chunks: %u encoded\n", ok_n);
    dfield_free(&df);
    return 0;
}

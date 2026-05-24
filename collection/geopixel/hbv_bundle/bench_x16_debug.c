#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tring.h"
#include "pogls_fold.h"
#include "geo_diamond_field_v4.h"

int main(void) {
    DiamondField df; dfield_init(&df, 65536); dfield_set_x16(&df, 1);
    uint32_t encoded = 0; uint32_t collided = 0;
    srand(12345);
    for (int i = 0; i < 500; i++) {
        uint8_t c[64]; for (int j = 0; j < 64; j++) c[j] = (uint8_t)(rand() & 0xFF);
        uint8_t n = shell_classify_level(c);
        uint32_t base = (uint32_t)(_fnv64(c, 64) % df.shell[n].slot_count);
        uint8_t slope = _x16_slope(c);
        int free_cnt = 0;
        for (int s = 0; s < 16; s++) {
            uint32_t eff = base * 16u + ((uint32_t)(slope + (uint8_t)s) & 0xFu);
            uint32_t gp = slot_global(n, (uint16_t)eff);
            if (sidx_get(&df.sidx, gp) == SLOT_NULL) free_cnt++;
        }
        uint32_t gidx = dfield_encode(&df, c, NULL);
        if (gidx != SLOT_NULL) { if (free_cnt <= 1) collided++; encoded++; }
        if (i < 10) printf("chunk %d: lv=%d base=%u slope=%u free=%d gidx=0x%x\n", i, n, base, slope, free_cnt, gidx);
    }
    printf("encoded=%u collided_last=%u\n", encoded, collided);
    dfield_free(&df);
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Minimal Binary Shell test for 48B blocks */
#define BS_CHUNK_SZ 48u
#define BS_ROT_STATES 6u

static void bs_rotate48(uint8_t out[48], const uint8_t in[48], uint8_t rot) {
    for (uint32_t z = 0; z < 3; z++) {
        for (uint32_t y = 0; y < 4; y++) {
            for (uint32_t x = 0; x < 4; x++) {
                uint32_t sx, sy;
                switch (rot % BS_ROT_STATES) {
                    case 0: sx=x;   sy=y;   break;
                    case 1: sx=y;   sy=x;   break;
                    case 2: sx=3-x; sy=y;   break;
                    case 3: sx=x;   sy=3-y; break;
                    case 4: sx=3-x; sy=3-y; break;
                    case 5: sx=3-y; sy=3-x; break;
                    default: sx=x;  sy=y;   break;
                }
                out[z*16 + y*4 + x] = in[z*16 + sy*4 + sx];
            }
        }
    }
}

static void bs_inv_rotate48(uint8_t out[48], const uint8_t in[48], uint8_t rot) {
    bs_rotate48(out, in, rot % BS_ROT_STATES);
}

int main() {
    /* Test: encode then decode a 48B block through each rotation */
    uint8_t original[48], rotated[48], restored[48];
    for (int i = 0; i < 48; i++) original[i] = (uint8_t)(i + 0x41);
    
    printf("Original first 10: ");
    for (int i = 0; i < 10; i++) printf("%02x ", original[i]);
    printf("\n");
    
    for (uint8_t rot = 0; rot < 6; rot++) {
        bs_rotate48(rotated, original, rot);
        bs_inv_rotate48(restored, rotated, rot);
        
        int match = (memcmp(original, restored, 48) == 0);
        printf("rot=%d: %s  rotated[0..3]=%02x%02x%02x%02x  restored[0..3]=%02x%02x%02x%02x\n",
               rot, match ? "PASS" : "FAIL",
               rotated[0], rotated[1], rotated[2], rotated[3],
               restored[0], restored[1], restored[2], restored[3]);
    }
    return 0;
}

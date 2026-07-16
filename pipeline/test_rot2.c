#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define BS_CHUNK_SZ 48u

static void bs_rotate48(uint8_t out[48], const uint8_t in[48], uint8_t rot) {
    for (uint32_t z = 0; z < 3; z++)
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++) {
                uint32_t sx, sy;
                switch (rot % 6) {
                    case 0: sx=x; sy=y; break;
                    case 1: sx=y; sy=x; break;
                    case 2: sx=3-x; sy=y; break;
                    case 3: sx=x; sy=3-y; break;
                    case 4: sx=3-x; sy=3-y; break;
                    case 5: sx=3-y; sy=3-x; break;
                    default: sx=x; sy=y; break;
                }
                out[z*16 + y*4 + x] = in[z*16 + sy*4 + sx];
            }
}

int main() {
    uint8_t orig[48], enc[50], dec[48];
    for (int i = 0; i < 48; i++) orig[i] = (uint8_t)(i + 0x41);
    
    /* Simulate encode: best_rot=2 */
    uint8_t rot = 2;
    uint8_t rotbuf[48];
    bs_rotate48(rotbuf, orig, rot);
    int nz = 0;
    for (int i = 0; i < 48; i++) if (rotbuf[i]) nz++;
    printf("rot=%d nz=%d\n", rot, nz);
    
    /* Store as RAW */
    enc[0] = 4; enc[1] = rot;
    memcpy(enc + 2, rotbuf, 48);
    
    /* Decode: read rot, read data, inverse-rotate */
    uint8_t dec_rot = enc[1];
    uint8_t dec_rotbuf[48];
    memcpy(dec_rotbuf, enc + 2, 48);
    bs_rotate48(dec, dec_rotbuf, dec_rot); /* self-inverse */
    
    printf("orig[0..3]: %02x %02x %02x %02x\n", orig[0], orig[1], orig[2], orig[3]);
    printf("rotbuf[0..3]: %02x %02x %02x %02x\n", rotbuf[0], rotbuf[1], rotbuf[2], rotbuf[3]);
    printf("dec_rotbuf[0..3]: %02x %02x %02x %02x\n", dec_rotbuf[0], dec_rotbuf[1], dec_rotbuf[2], dec_rotbuf[3]);
    printf("dec[0..3]: %02x %02x %02x %02x\n", dec[0], dec[1], dec[2], dec[3]);
    printf("Match: %s\n", memcmp(orig, dec, 48) == 0 ? "PASS" : "FAIL");
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define BS_FLAG_FLAT   0u
#define BS_FLAG_SPARSE 1u
#define BS_FLAG_DENSE  2u
#define BS_FLAG_PARTIAL 3u
#define BS_SPARSE_THRESH 16u
#define BS_ROT_STATES 6u
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

static uint32_t bs_encode_block(uint8_t *out, const uint8_t chunk48[48]) {
    int is_zero = 1;
    for (uint32_t i = 0; i < 48; i++) { if (chunk48[i]) { is_zero = 0; break; } }
    if (is_zero) { out[0] = 0; out[1] = 0; return 2; }

    uint8_t best_rot = 0;
    int best_nz = 48;
    uint8_t best_buf[48];
    for (uint8_t rot = 0; rot < 6; rot++) {
        uint8_t rotbuf[48];
        bs_rotate48(rotbuf, chunk48, rot);
        int nz = 0;
        for (uint32_t i = 0; i < 48; i++) if (rotbuf[i]) nz++;
        if (nz < best_nz) { best_nz = nz; best_rot = rot; memcpy(best_buf, rotbuf, 48); }
    }

    if ((uint32_t)best_nz <= 16) {
        out[0] = 1; out[1] = best_rot; out[2] = (uint8_t)best_nz;
        uint32_t pos = 3;
        for (uint32_t i = 0; i < 48; i++) {
            if (best_buf[i]) { out[pos] = (uint8_t)i; out[pos + best_nz] = best_buf[i]; pos++; if (pos >= 3 + (uint32_t)best_nz) break; }
        }
        return 3 + (uint32_t)best_nz * 2;
    }

    out[0] = 4; out[1] = best_rot;
    memcpy(out + 2, best_buf, 48);
    return 50;
}

static uint32_t bs_decode_block(uint8_t out48[48], const uint8_t *in, uint32_t *bytes_read) {
    uint8_t flag = in[0];
    if (flag == 0) { memset(out48, 0, 48); *bytes_read = 2; return 48; }
    if (flag == 1) {
        uint8_t rot = in[1], nz = in[2];
        uint8_t rotbuf[48] = {0};
        for (uint32_t i = 0; i < nz; i++) { uint8_t idx = in[3+i], val = in[3+nz+i]; rotbuf[idx] = val; }
        bs_rotate48(out48, rotbuf, rot); /* self-inverse */
        *bytes_read = 3 + (uint32_t)nz * 2; return 48;
    }
    if (flag == 4) {
        uint8_t rot = in[1];
        uint8_t rotbuf[48];
        memcpy(rotbuf, in + 2, 48);
        bs_rotate48(out48, rotbuf, rot); /* self-inverse */
        *bytes_read = 50; return 48;
    }
    *bytes_read = 0; return 0;
}

int main() {
    /* Test: create a small dataset, encode all blocks, decode all blocks, compare */
    uint8_t data[480]; /* 10 blocks of 48B */
    for (int i = 0; i < 480; i++) data[i] = (uint8_t)((i * 7 + 13) & 0xFF);
    
    /* Encode all blocks */
    uint8_t enc_buf[5120];
    uint32_t enc_pos = 0;
    uint32_t blk_sizes[10];
    for (int b = 0; b < 10; b++) {
        uint8_t enc[51];
        uint32_t sz = bs_encode_block(enc, data + b * 48);
        blk_sizes[b] = sz;
        memcpy(enc_buf + enc_pos, enc, sz);
        enc_pos += sz;
    }
    printf("Encoded: 10 blocks, total %u bytes (from 480 raw)\n", enc_pos);
    
    /* Decode all blocks */
    uint8_t decoded[480];
    memset(decoded, 0, sizeof(decoded));
    uint32_t dec_pos = 0;
    for (int b = 0; b < 10; b++) {
        uint8_t out48[48];
        uint32_t bytes_read = 0;
        uint32_t ret = bs_decode_block(out48, enc_buf + dec_pos, &bytes_read);
        printf("Block %d: flag=%d, bytes_read=%u, ret=%u\n", b, enc_buf[dec_pos], bytes_read, ret);
        if (bytes_read == 0) { printf("  STALL at offset %u\n", dec_pos); break; }
        memcpy(decoded + b * 48, out48, 48);
        dec_pos += bytes_read;
    }
    
    /* Compare */
    int match = (memcmp(data, decoded, 480) == 0);
    printf("Roundtrip: %s\n", match ? "PASS" : "FAIL");
    if (!match) {
        for (int i = 0; i < 480; i++) {
            if (data[i] != decoded[i]) {
                printf("  First diff at byte %d: orig=%02x decoded=%02x\n", i, data[i], decoded[i]);
                break;
            }
        }
    }
    return 0;
}

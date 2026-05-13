/* test_header_frame.c — verify HbHeaderFrame write/read/crc */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "gpx5_container.h"
#include "fibo_shell_walk.h"
#include "hamburger_classify.h"
#include "hamburger_pipe.h"
#include "hamburger_encode.h"
#include "hb_header_frame.h"

int main(void) {
    int pass = 0, fail = 0;

    /* T1: write → read roundtrip */
    HbHeaderFrame f1 = {
        .magic = HBHF_MAGIC, .version = HBHF_VERSION,
        .n_cycles = 2, .n_layers = 3,
        .total_tiles = 8192, .img_w = 1280, .img_h = 720,
        .tile_w = 16, .tile_h = 16, .global_seed = 0xDEADBEEFu,
        .tick_period = 1440, .codec_map = {4,4,255,4,4,255,4,4},
        .layer_stride = 512
    };
    uint8_t buf[HBHF_SZ];
    hbhf_write(buf, &f1);

    HbHeaderFrame f2;
    int rc = hbhf_read(buf, &f2);
    if (rc != 0) { printf("T1 FAIL read rc=%d\n", rc); fail++; }
    else if (f2.n_cycles != 2 || f2.total_tiles != 8192 ||
             f2.img_w != 1280  || f2.global_seed != 0xDEADBEEFu) {
        printf("T1 FAIL field mismatch\n"); fail++;
    } else { printf("T1 PASS roundtrip\n"); pass++; }

    /* T2: corrupt byte → crc fail */
    buf[10] ^= 0xFF;
    rc = hbhf_read(buf, &f2);
    if (rc != -3) { printf("T2 FAIL expected crc error, got %d\n", rc); fail++; }
    else { printf("T2 PASS crc detect\n"); pass++; }
    buf[10] ^= 0xFF;  /* restore */

    /* T3: wrong magic */
    buf[0] = 'X';
    rc = hbhf_read(buf, &f2);
    if (rc != -1) { printf("T3 FAIL expected magic error\n"); fail++; }
    else { printf("T3 PASS magic detect\n"); pass++; }
    buf[0] = 'H';  /* restore */

    /* T4: codec_map roundtrip */
    hbhf_read(buf, &f2);
    int codec_ok = (memcmp(f2.codec_map, f1.codec_map, 8) == 0);
    if (!codec_ok) { printf("T4 FAIL codec_map\n"); fail++; }
    else { printf("T4 PASS codec_map\n"); pass++; }

    /* T5: size constant */
    if (HBHF_SZ != 64u) { printf("T5 FAIL size=%u\n", HBHF_SZ); fail++; }
    else { printf("T5 PASS size=64\n"); pass++; }

    /* T6: hbhf_from_gpx5 with a real file */
    Gpx5File gf;
    if (gpx5_open("/tmp/ts_test.gpx5", &gf) == 0) {
        HbHeaderFrame fgpx;
        hbhf_from_gpx5(&fgpx, &gf, 1, gf.hdr.n_tiles, 32, 32, 16, 16);
        gpx5_close(&gf);
        uint8_t b2[HBHF_SZ];
        hbhf_write(b2, &fgpx);
        HbHeaderFrame fback;
        rc = hbhf_read(b2, &fback);
        if (rc != 0 || fback.n_cycles != 1 || fback.img_w != 32) {
            printf("T6 FAIL from_gpx5 rc=%d\n", rc); fail++;
        } else { printf("T6 PASS from_gpx5  seed=0x%08X  codec0=%u\n",
                        fback.global_seed, fback.codec_map[0]); pass++; }
    } else {
        printf("T6 SKIP (no test file)\n");
    }

    printf("\n%d/%d PASS\n", pass, pass+fail);
    return (fail == 0) ? 0 : 1;
}

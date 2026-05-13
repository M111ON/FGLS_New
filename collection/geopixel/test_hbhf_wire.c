/*
 * test_hbhf_wire.c — verify HBHF auto-append in hamburger_encode_image
 *
 * 1. encode 32x32 image → /tmp/hbhf_wire.gpx5
 * 2. gpx5_read_hbhf() → HbHeaderFrame
 * 3. verify all fields match img params
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gpx5_container.h"
#include "fibo_shell_walk.h"
#include "hamburger_classify.h"
#include "hamburger_pipe.h"
#include "hb_header_frame.h"
#include "gpx5_hbhf.h"
#include "hamburger_encode.h"

#define IMG_W 32
#define IMG_H 32
#define TILE  16
#define PATH  "/tmp/hbhf_wire.gpx5"

int main(void)
{
    int pass = 0, fail = 0;

    /* ── encode ── */
    int Y[IMG_W*IMG_H], Cg[IMG_W*IMG_H], Co[IMG_W*IMG_H];
    for (int i = 0; i < IMG_W*IMG_H; i++) {
        Y[i]  = (i * 3) & 0xFF;
        Cg[i] = (i * 7) & 0x7F;
        Co[i] = (i * 5) & 0x7F;
    }
    HbImageIn img = { Y, Cg, Co, IMG_W, IMG_H, TILE, TILE };
    uint32_t seed = 0xC0FFEE42u;

    if (hamburger_encode_image(PATH, &img, seed, 0) != HB_OK) {
        fprintf(stderr, "FAIL: encode\n"); return 1;
    }
    printf("encode OK\n");

    /* ── read HBHF ── */
    HbHeaderFrame hf;
    memset(&hf, 0, sizeof(hf));
    int rc = gpx5_read_hbhf(PATH, &hf);
    if (rc != 0) {
        printf("FAIL: gpx5_read_hbhf rc=%d\n", rc); return 1;
    }
    printf("read_hbhf OK\n");

#define CHK(label, got, exp) \
    if ((got) == (exp)) { printf("  PASS %-20s = %u\n", label, (unsigned)(exp)); pass++; } \
    else { printf("  FAIL %-20s got=%u exp=%u\n", label, (unsigned)(got), (unsigned)(exp)); fail++; }

    CHK("magic",       hf.magic,       HBHF_MAGIC);
    CHK("version",     hf.version,     HBHF_VERSION);
    CHK("n_cycles",    hf.n_cycles,    1u);
    CHK("n_layers",    hf.n_layers,    3u);
    CHK("total_tiles", hf.total_tiles, (IMG_W/TILE)*(IMG_H/TILE));
    CHK("img_w",       hf.img_w,       IMG_W);
    CHK("img_h",       hf.img_h,       IMG_H);
    CHK("tile_w",      hf.tile_w,      TILE);
    CHK("tile_h",      hf.tile_h,      TILE);
    CHK("global_seed", hf.global_seed, seed);
    CHK("tick_period", hf.tick_period, GPX5_TICK_PERIOD);

    printf("\n%d/%d PASS\n", pass, pass+fail);
    return (fail == 0) ? 0 : 1;
}

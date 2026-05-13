/*
 * test_tile_stream.c — verify hb_tile_stream() single-tile decode
 *
 * Strategy:
 *   1. Pack a small BMP via hamburger_encode_image()
 *   2. Full decode via hamburger_decode()  → reference pixels
 *   3. For each tile: fibo_lut_build → lut_seek → hb_tile_stream → compare
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gpx5_container.h"
#include "fibo_shell_walk.h"
#include "hamburger_classify.h"
#include "hamburger_pipe.h"
#include "hamburger_encode.h"
#include "hb_tile_stream.h"

/* ── tiny synthetic image (32x32 px, YCgCo constant) ── */
#define IMG_W 32
#define IMG_H 32
#define TILE  16
#define N_TILES ((IMG_W/TILE)*(IMG_H/TILE))  /* = 4 */

static void make_test_image(int Y[IMG_W*IMG_H],
                             int Cg[IMG_W*IMG_H],
                             int Co[IMG_W*IMG_H])
{
    for (int i = 0; i < IMG_W*IMG_H; i++) {
        Y[i]  = (i * 3) & 0xFF;
        Cg[i] = (i * 7) & 0x7F;
        Co[i] = (i * 5) & 0x7F;
    }
}

int main(void)
{
    int pass = 0, fail = 0;
    const char *path = "/tmp/ts_test.gpx5";

    /* ── 1. encode ── */
    int Y[IMG_W*IMG_H], Cg[IMG_W*IMG_H], Co[IMG_W*IMG_H];
    make_test_image(Y, Cg, Co);

    HbImageIn img = { Y, Cg, Co, IMG_W, IMG_H, TILE, TILE };
    if (hamburger_encode_image(path, &img, 0xABCD1234u, 0) != HB_OK) {
        fprintf(stderr, "FAIL: encode\n"); return 1;
    }
    printf("encode OK\n");

    /* ── 2. full decode (reference) ── */
    uint8_t *ref[N_TILES];
    for (int i = 0; i < N_TILES; i++)
        ref[i] = (uint8_t *)calloc(1, HB_TILE_SZ_MAX);

    uint32_t n_tiles_out = 0, tile_sz_out = 0;
    if (hamburger_decode(path, ref, &n_tiles_out, &tile_sz_out) != HB_OK) {
        fprintf(stderr, "FAIL: unpack\n"); return 1;
    }
    printf("unpack OK  (n=%u  sz=%u)\n", n_tiles_out, tile_sz_out);

    /* ── 3. build FiboLut ── */
    Gpx5File gf;
    if (gpx5_open(path, &gf) != 0) { fprintf(stderr, "FAIL: gpx5_open\n"); return 1; }

    HbFiboLutEntry *lut = (HbFiboLutEntry *)calloc(gf.hdr.n_tiles, sizeof(HbFiboLutEntry));
    hb_fibo_lut_build(0, &gf, lut);
    gpx5_close(&gf);
    printf("fibo_lut_build OK  (n=%u)\n", (unsigned)n_tiles_out);

    /* ── 4. stream each tile and compare ── */
    uint8_t *got = (uint8_t *)calloc(1, HB_TILE_SZ_MAX);
    for (uint32_t ti = 0; ti < n_tiles_out; ti++) {
        uint8_t tick  = lut[ti].tick;
        uint16_t cyc  = lut[ti].cycle_id;
        const HbFiboLutEntry *e = hb_fibo_lut_seek(lut, n_tiles_out, tick, cyc);

        if (!e) { printf("[T%u] FAIL lut_seek\n", ti); fail++; continue; }

        uint32_t got_bytes = 0;
        memset(got, 0, HB_TILE_SZ_MAX);
        int rc = hb_tile_stream(path, e, got, HB_TILE_SZ_MAX, &got_bytes);

        if (rc == HBT_ERR_ENTRY) {
            /* clear tile — try clear path */
            rc = hb_tile_stream_clear(path, e, got, HB_TILE_SZ_MAX, &got_bytes);
        }

        if (rc != HBT_OK) {
            printf("[T%u] FAIL stream rc=%d\n", ti, rc); fail++; continue;
        }

        /* compare against reference (up to got_bytes) */
        int mismatch = memcmp(got, ref[ti], got_bytes);
        if (mismatch) {
            printf("[T%u] FAIL pixel mismatch (bytes=%u)\n", ti, got_bytes); fail++;
        } else {
            printf("[T%u] PASS  tick=%u  codec=%u  bytes=%u\n",
                   ti, tick, e->codec_id, got_bytes);
            pass++;
        }
    }

    free(got);
    free(lut);
    for (int i = 0; i < N_TILES; i++) free(ref[i]);

    printf("\n%d/%d PASS\n", pass, pass+fail);
    return (fail == 0) ? 0 : 1;
}

/*
 * test_multicycle_stream.c
 *
 * Strategy:
 *   1. Encode cycle-0 (32x32) → /tmp/mc_cycle0.gpx5
 *   2. Encode cycle-1 (32x32, different pixels) → /tmp/mc_cycle1.gpx5
 *   3. Build combined LUT (cycle0 entries + cycle1 entries)
 *   4. Register both in HbManifest
 *   5. For every tile in combined LUT: hb_tile_stream_mc → compare vs ref
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
#include "hb_manifest.h"
#include "hb_tile_stream.h"

#define IMG_W   32
#define IMG_H   32
#define TILE    16
#define N_TILES ((IMG_W/TILE)*(IMG_H/TILE))   /* 4 per cycle */
#define N_CYCLES 2
#define TOTAL_TILES (N_TILES * N_CYCLES)

static void make_pixels(int Y[], int Cg[], int Co[], int seed)
{
    for (int i = 0; i < IMG_W*IMG_H; i++) {
        Y[i]  = (i * 3 + seed)      & 0xFF;
        Cg[i] = (i * 7 + seed * 3)  & 0x7F;
        Co[i] = (i * 5 + seed * 7)  & 0x7F;
    }
}

int main(void)
{
    int pass = 0, fail = 0;

    const char *paths[N_CYCLES] = {
        "/tmp/mc_cycle0.gpx5",
        "/tmp/mc_cycle1.gpx5"
    };
    const uint32_t seeds[N_CYCLES] = { 0xABCD1234u, 0xDEADBEEFu };

    /* ── 1-2. encode both cycles ── */
    for (int c = 0; c < N_CYCLES; c++) {
        int Y[IMG_W*IMG_H], Cg[IMG_W*IMG_H], Co[IMG_W*IMG_H];
        make_pixels(Y, Cg, Co, c * 0x42);
        HbImageIn img = { Y, Cg, Co, IMG_W, IMG_H, TILE, TILE };
        if (hamburger_encode_image(paths[c], &img, seeds[c], 0) != HB_OK) {
            fprintf(stderr, "FAIL: encode cycle%d\n", c); return 1;
        }
        printf("encode cycle%d OK\n", c);
    }

    /* ── 3. build combined LUT ── */
    HbFiboLutEntry lut[TOTAL_TILES];
    memset(lut, 0, sizeof(lut));

    for (int c = 0; c < N_CYCLES; c++) {
        Gpx5File gf;
        if (gpx5_open(paths[c], &gf) != 0) {
            fprintf(stderr, "FAIL: gpx5_open cycle%d\n", c); return 1;
        }
        hb_fibo_lut_build((uint16_t)c, &gf,
                          lut + (uint32_t)c * N_TILES);
        gpx5_close(&gf);
    }
    printf("fibo_lut_build OK  (total=%d)\n", TOTAL_TILES);

    /* ── 4. manifest ── */
    HbManifest manifest;
    hb_manifest_init(&manifest);
    for (int c = 0; c < N_CYCLES; c++)
        hb_manifest_add(&manifest, (uint16_t)c, paths[c]);

    /* ── 5. full reference decode per cycle ── */
    uint8_t *ref[TOTAL_TILES];
    for (int i = 0; i < TOTAL_TILES; i++)
        ref[i] = (uint8_t *)calloc(1, HB_TILE_SZ_MAX);

    for (int c = 0; c < N_CYCLES; c++) {
        uint8_t *cyc_ref[N_TILES];
        for (int i = 0; i < N_TILES; i++)
            cyc_ref[i] = ref[c * N_TILES + i];

        uint32_t nt = 0, tsz = 0;
        if (hamburger_decode(paths[c], cyc_ref, &nt, &tsz) != HB_OK) {
            fprintf(stderr, "FAIL: decode cycle%d\n", c); return 1;
        }
        printf("decode cycle%d OK  (n=%u)\n", c, nt);
    }

    /* ── 6. stream via manifest + compare ── */
    uint8_t *got = (uint8_t *)calloc(1, HB_TILE_SZ_MAX);

    for (int ti = 0; ti < TOTAL_TILES; ti++) {
        const HbFiboLutEntry *e = &lut[ti];

        memset(got, 0, HB_TILE_SZ_MAX);
        uint32_t got_bytes = 0;

        int rc = hb_tile_stream_mc(&manifest, e, got, HB_TILE_SZ_MAX, &got_bytes);
        if (rc != HBT_OK) {
            printf("[C%u T%u] FAIL stream_mc rc=%d\n",
                   e->cycle_id, e->tile_id, rc);
            fail++; continue;
        }

        int mismatch = memcmp(got, ref[ti], got_bytes);
        if (mismatch) {
            printf("[C%u T%u] FAIL pixel mismatch\n", e->cycle_id, e->tile_id);
            fail++;
        } else {
            printf("[C%u T%u] PASS  tick=%u codec=%u bytes=%u\n",
                   e->cycle_id, e->tile_id, e->tick, e->codec_id, got_bytes);
            pass++;
        }
    }

    free(got);
    for (int i = 0; i < TOTAL_TILES; i++) free(ref[i]);

    printf("\n%d/%d PASS\n", pass, pass+fail);
    return (fail == 0) ? 0 : 1;
}

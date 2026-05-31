#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "hex_codec.h"

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); _fail++; } \
} while (0)

int main(void) {
    printf("=== hex_codec.h v3 verification ===\n");

    /* T01: FLAT tile roundtrip */
    printf("\nT01: FLAT tile\n");
    {
        HexTile in  = {{100,100,100,100,100,100,100}};
        uint8_t buf[16];
        int n = hex_tile_encode(&in, buf);
        HexTile out;
        int m = hex_tile_decode(buf, sizeof(buf), &out);
        CHECK(n == 2 && m == 2, "FLAT: 2B each way");
        CHECK(memcmp(in.c, out.c, 7) == 0, "FLAT: roundtrip exact");
    }

    /* T02: SMOOTH tile (avg XOR diff < 8) */
    printf("\nT02: SMOOTH tile\n");
    {
        uint8_t v[7] = {100,101,100,101,100,101,100};
        HexTile in; memcpy(in.c, v, 7);
        uint8_t buf[16];
        int n = hex_tile_encode(&in, buf);
        CHECK(n == 8, "SMOOTH: 8B");
        HexTile out;
        int m = hex_tile_decode(buf, sizeof(buf), &out);
        CHECK(m == 8, "SMOOTH: decode 8B");
        CHECK(memcmp(in.c, out.c, 7) == 0, "SMOOTH: roundtrip exact");
    }

    /* T03: GRADIENT tile (avg 8..63) */
    printf("\nT03: GRADIENT tile\n");
    {
        uint8_t v[7] = {10,30,50,70,90,110,60};
        HexTile in; memcpy(in.c, v, 7);
        uint8_t buf[16];
        int n = hex_tile_encode(&in, buf);
        CHECK(n == 8, "GRADIENT: 8B");
        HexTile out;
        int m = hex_tile_decode(buf, sizeof(buf), &out);
        CHECK(m == 8, "GRADIENT: decode 8B");
        CHECK(memcmp(in.c, out.c, 7) == 0, "GRADIENT: roundtrip exact");
    }

    /* T04: EDGE tile (avg >= 64) */
    printf("\nT04: EDGE tile\n");
    {
        uint8_t v[7] = {5,200,8,210,12,205,100};
        HexTile in; memcpy(in.c, v, 7);
        uint8_t buf[16];
        int n = hex_tile_encode(&in, buf);
        CHECK(n == 8, "EDGE: 8B");
        HexTile out;
        int m = hex_tile_decode(buf, sizeof(buf), &out);
        CHECK(m == 8, "EDGE: decode 8B");
        CHECK(memcmp(in.c, out.c, 7) == 0, "EDGE: roundtrip exact");
    }

    /* T05: tile classify */
    printf("\nT05: classify\n");
    {
        HexTile flat = {{42,42,42,42,42,42,42}};
        CHECK(hex_tile_classify(&flat) == HENC_FLAT, "all same -> FLAT");
        uint8_t sv[7] = {100,101,100,101,100,101,100};
        HexTile sm; memcpy(sm.c, sv, 7);
        CHECK(hex_tile_classify(&sm) == HENC_SMOOTH, "near-center -> SMOOTH");
        uint8_t gv[7] = {10,30,50,70,90,110,60};
        HexTile grad; memcpy(grad.c, gv, 7);
        CHECK(hex_tile_classify(&grad) == HENC_GRADIENT, "moderate var -> GRADIENT");
        uint8_t ev[7] = {5,250,5,250,5,250,128};
        HexTile edge; memcpy(edge.c, ev, 7);
        CHECK(hex_tile_classify(&edge) == HENC_EDGE, "extreme var -> EDGE");
    }

    /* T06: L2Block all-FLAT roundtrip */
    printf("\nT06: L2Block all-FLAT\n");
    {
        L2Block blk;
        for (int i = 0; i < 7; i++)
            memset(blk.tiles[i].c, 50 + i, 7);
        uint8_t buf[L2_MAXBYTES];
        int n = l2_encode(&blk, buf);
        CHECK(n > 0 && n <= L2_MAXBYTES, "L2 all-FLAT: encode OK");
        L2Block out;
        int r = l2_decode(buf, n, &out);
        CHECK(r == 0, "L2 all-FLAT: decode OK");
        CHECK(memcmp(&blk, &out, sizeof(L2Block)) == 0, "L2 all-FLAT: roundtrip exact");
    }

    /* T07: L2Block mixed tiles roundtrip */
    printf("\nT07: L2Block mixed tiles\n");
    {
        uint8_t patterns[7][7] = {
            {100,100,100,100,100,100,100},  /* FLAT */
            {100,101,100,101,100,101,100},  /* SMOOTH */
            {10,30,50,70,90,110,60},        /* GRADIENT */
            {5,200,8,210,12,205,100},       /* EDGE */
            {42,187,63,220,11,155,88},      /* random */
            {0,0,0,0,0,0,0},                /* FLAT 0 */
            {255,255,255,255,255,255,255},  /* FLAT 255 */
        };
        L2Block blk;
        for (int i = 0; i < 7; i++)
            memcpy(blk.tiles[i].c, patterns[i], 7);
        uint8_t buf[L2_MAXBYTES];
        int n = l2_encode(&blk, buf);
        CHECK(n > 0 && n <= L2_MAXBYTES, "L2 mixed: encode OK");
        L2Block out;
        int r = l2_decode(buf, n, &out);
        CHECK(r == 0, "L2 mixed: decode OK");
        CHECK(memcmp(&blk, &out, sizeof(L2Block)) == 0, "L2 mixed: roundtrip exact");
    }

    /* T08: L2 stats */
    printf("\nT08: L2 stats\n");
    {
        uint8_t patterns[7][7] = {
            {42,42,42,42,42,42,42},           /* FLAT */
            {100,101,100,101,100,101,100},    /* SMOOTH */
            {10,30,50,70,90,110,60},          /* GRADIENT */
            {5,250,5,250,5,250,128},          /* EDGE */
            {0,0,0,0,0,0,0},                 /* FLAT */
            {10,32,50,72,90,112,62},          /* GRADIENT */
            {200,201,200,201,200,201,200},    /* SMOOTH */
        };
        L2Block blk;
        for (int i = 0; i < 7; i++)
            memcpy(blk.tiles[i].c, patterns[i], 7);
        HexTileStats s = l2_stats(&blk);
        CHECK(s.flat == 2, "stats flat=2");
        CHECK(s.smooth == 2, "stats smooth=2");
        CHECK(s.grad == 2, "stats grad=2");
        CHECK(s.edge == 1, "stats edge=1");
    }

    /* T09: L2 random data */
    printf("\nT09: L2 random data\n");
    {
        L2Block blk;
        for (int i = 0; i < 7; i++)
            for (int j = 0; j < 7; j++)
                blk.tiles[i].c[j] = (uint8_t)(i * 37 + j * 53);
        uint8_t buf[L2_MAXBYTES];
        int n = l2_encode(&blk, buf);
        CHECK(n > 0, "L2 random: encode OK");
        L2Block out;
        int r = l2_decode(buf, n, &out);
        CHECK(r == 0, "L2 random: decode OK");
        CHECK(memcmp(&blk, &out, sizeof(L2Block)) == 0, "L2 random: roundtrip exact");
    }

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    return _fail ? 1 : 0;
}

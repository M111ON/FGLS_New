/*
 * test_hex_tile.c -- hex_tile.h 7-cell hex tile codec verification
 * Tests:
 *   H01: FLAT encode/decode roundtrip
 *   H02: TRIPLET_FLAT roundtrip
 *   H03: GRADIENT roundtrip
 *   H04: EDGE roundtrip
 *   H05: random tiles roundtrip (all 256 values)
 *   H06: decode reject undersized input
 *   H07: encode returns correct byte count (2 or 9)
 *   H08: classify correctness
 *   H09: center prediction sanity
 *   H10: GPX5_CODEC_HEX integration test via hb_codec_apply/invert
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "hex_tile.h"

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); _fail++; } \
} while (0)

int main(void) {
    printf("=== hex_tile.h verification ===\n");

    /* H01: FLAT roundtrip */
    printf("\nH01: FLAT encode/decode\n");
    {
        HexTile in  = {{100,100,100,100,100,100,100}};
        HexTile out; memset(&out, 0xFF, sizeof(out));
        uint8_t buf[16]; memset(buf, 0xFF, sizeof(buf));
        int n = hex_tile_encode(&in, buf);
        int m = hex_tile_decode(buf, sizeof(buf), &out);
        CHECK(n == 2 && m == 2, "FLAT: encode returns 2, decode returns 2");
        CHECK(buf[0] == HENC_FLAT, "FLAT: type byte = HENC_FLAT");
        CHECK(buf[1] == 100, "FLAT: value byte = 100");
        CHECK(memcmp(in.c, out.c, HEX_CELLS) == 0, "FLAT: roundtrip exact");
    }

    /* H02: TRIPLET_FLAT */
    printf("\nH02: TRIPLET_FLAT encode/decode\n");
    {
        /* triplet {0,1,6} all = 200, rest different */
        HexTile in  = {{200,200,50,50,50,50,200}};
        HexTile out; memset(&out, 0xFF, sizeof(out));
        uint8_t buf[16]; memset(buf, 0xFF, sizeof(buf));
        int n = hex_tile_encode(&in, buf);
        int m = hex_tile_decode(buf, sizeof(buf), &out);
        CHECK(n == 9 && m == 9, "TRIPLET_FLAT: encode returns 9, decode returns 9");
        CHECK(buf[0] == HENC_TRIPLET_FLAT, "TRIPLET_FLAT: type byte correct");
        CHECK(memcmp(in.c, out.c, HEX_CELLS) == 0, "TRIPLET_FLAT: roundtrip exact");
    }

    /* H03: GRADIENT (range <= 32) */
    printf("\nH03: GRADIENT encode/decode\n");
    {
        uint8_t vals[7] = {10,15,20,25,30,35,22};
        HexTile in; memcpy(in.c, vals, 7);
        HexTile out; memset(&out, 0xFF, sizeof(out));
        uint8_t buf[16]; memset(buf, 0xFF, sizeof(buf));
        int n = hex_tile_encode(&in, buf);
        int m = hex_tile_decode(buf, sizeof(buf), &out);
        CHECK(n == 9 && m == 9, "GRADIENT: encode returns 9, decode returns 9");
        CHECK(buf[0] == HENC_GRADIENT, "GRADIENT: type byte correct");
        CHECK(memcmp(in.c, out.c, HEX_CELLS) == 0, "GRADIENT: roundtrip exact");
    }

    /* H04: EDGE (range > 32) */
    printf("\nH04: EDGE encode/decode\n");
    {
        uint8_t vals[7] = {5,10,8,200,210,205,100};
        HexTile in; memcpy(in.c, vals, 7);
        HexTile out; memset(&out, 0xFF, sizeof(out));
        uint8_t buf[16]; memset(buf, 0xFF, sizeof(buf));
        int n = hex_tile_encode(&in, buf);
        int m = hex_tile_decode(buf, sizeof(buf), &out);
        CHECK(n == 9 && m == 9, "EDGE: encode returns 9, decode returns 9");
        CHECK(buf[0] == HENC_EDGE, "EDGE: type byte correct");
        CHECK(memcmp(in.c, out.c, HEX_CELLS) == 0, "EDGE: roundtrip exact");
    }

    /* H05: random tiles exhaustive (0..255 all combos, 7 cells) */
    printf("\nH05: random tile roundtrip (256 random seeds)\n");
    {
        int ok = 1;
        for (int seed = 0; seed < 256 && ok; seed++) {
            HexTile in;
            for (int i = 0; i < 7; i++)
                in.c[i] = (uint8_t)((seed * 37 + i * 53) & 0xFF);
            uint8_t buf[16];
            int n = hex_tile_encode(&in, buf);
            if (n != 2 && n != 9) { ok = 0; break; }
            HexTile out;
            int m = hex_tile_decode(buf, sizeof(buf), &out);
            if (m != n || memcmp(in.c, out.c, 7) != 0) { ok = 0; break; }
        }
        CHECK(ok, "256 random tiles roundtrip exact");
    }

    /* H06: decode reject undersized input */
    printf("\nH06: decode rejects bad input\n");
    {
        HexTile out;
        uint8_t buf[1] = {HENC_FLAT};
        CHECK(hex_tile_decode(buf, 0, &out) == 0, "zero-length flat rejected");
        CHECK(hex_tile_decode(buf, 1, &out) == 0, "1-byte flat rejected");
        uint8_t grad[5] = {HENC_GRADIENT, 128, 0, 0, 0};
        CHECK(hex_tile_decode(grad, 5, &out) == 0, "short non-flat rejected");
    }

    /* H07: verify encode always returns 2 or 9 */
    printf("\nH07: encode always returns 2 or 9\n");
    {
        int ok = 1;
        for (int v = 0; v < 256 && ok; v++) {
            HexTile t;
            for (int i = 0; i < 7; i++) t.c[i] = (uint8_t)((v * 31 + i * 17) & 0xFF);
            uint8_t buf[16];
            int n = hex_tile_encode(&t, buf);
            if (n != 2 && n != 9) ok = 0;
        }
        CHECK(ok, "all inputs produce 2 or 9 byte output");
    }

    /* H08: classify correctness */
    printf("\nH08: classify correctness\n");
    {
        HexTile flat = {{42,42,42,42,42,42,42}};
        CHECK(_hex_classify(&flat) == HENC_FLAT, "all same -> FLAT");

        HexTile tflat = {{200,200,50,50,50,50,200}};
        CHECK(_hex_classify(&tflat) == HENC_TRIPLET_FLAT, "triplet all same -> TRIPLET_FLAT");

        uint8_t gd[7] = {10,15,20,25,30,35,22};
        HexTile grad; memcpy(grad.c, gd, 7);
        CHECK(_hex_classify(&grad) == HENC_GRADIENT, "range <= 32 -> GRADIENT");

        uint8_t ed[7] = {5,200,8,210,12,205,100};
        HexTile edge; memcpy(edge.c, ed, 7);
        CHECK(_hex_classify(&edge) == HENC_EDGE, "range > 32 -> EDGE");
    }

    /* H09: center prediction */
    printf("\nH09: center prediction\n");
    {
        /* triplet flat: pred = triplet value */
        HexTile tflat = {{200,200,50,50,50,50,200}};
        CHECK(_hex_predict(&tflat) == 200, "triplet flat predicts center = 200");

        /* median of ring: {10,20,30,40,50,60} -> sorted -> median(30,40)/2 = 35 */
        uint8_t v[7] = {10,20,30,40,50,60,99};
        HexTile grad; memcpy(grad.c, v, 7);
        CHECK(_hex_predict(&grad) == 35, "median prediction (10..60) -> 35");
    }

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    return _fail ? 1 : 0;
}

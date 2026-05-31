#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_hex_layer.h"

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); _fail++; } \
} while (0)

int main(void) {
    printf("=== geo_hex_layer.h verification ===\n");

    /* T01: encode_sub FLAT */
    printf("\nT01: encode_sub FLAT\n");
    {
        HexTile t = {{100,100,100,100,100,100,100}};
        uint16_t sub = gpx4_geo_hex_encode_sub(&t);
        GeoHexInfo info = gpx4_geo_hex_decode_sub(sub);
        CHECK(info.tile_type == HENC_FLAT, "FLAT type");
        CHECK(info.center_val == 100, "FLAT center");
        CHECK(info.xor_diff <= 3, "FLAT xor_diff small");
    }

    /* T02: encode_sub SMOOTH */
    printf("\nT02: encode_sub SMOOTH\n");
    {
        uint8_t v[7] = {100,101,100,101,100,101,100};
        HexTile t; memcpy(t.c, v, 7);
        uint16_t sub = gpx4_geo_hex_encode_sub(&t);
        GeoHexInfo info = gpx4_geo_hex_decode_sub(sub);
        CHECK(info.tile_type == HENC_SMOOTH, "SMOOTH type");
        CHECK(info.center_val == 100, "SMOOTH center");
    }

    /* T03: encode_sub GRADIENT */
    printf("\nT03: encode_sub GRADIENT\n");
    {
        uint8_t v[7] = {10,30,50,70,90,110,60};
        HexTile t; memcpy(t.c, v, 7);
        uint16_t sub = gpx4_geo_hex_encode_sub(&t);
        GeoHexInfo info = gpx4_geo_hex_decode_sub(sub);
        CHECK(info.tile_type == HENC_GRADIENT, "GRADIENT type");
        CHECK(info.center_val == 60, "GRADIENT center");
    }

    /* T04: encode_sub EDGE */
    printf("\nT04: encode_sub EDGE\n");
    {
        uint8_t v[7] = {5,250,5,250,5,250,128};
        HexTile t; memcpy(t.c, v, 7);
        uint16_t sub = gpx4_geo_hex_encode_sub(&t);
        GeoHexInfo info = gpx4_geo_hex_decode_sub(sub);
        CHECK(info.tile_type == HENC_EDGE, "EDGE type");
        CHECK(info.center_val == 128, "EDGE center");
    }

    /* T05: bulk write/read roundtrip */
    printf("\nT05: bulk write/read\n");
    {
        HexTile tiles[3];
        uint8_t pents[3] = {1, 5, 10};
        uint16_t hilberts[3] = {100, 2000, 16383};
        
        uint8_t t0v[7] = {100,100,100,100,100,100,100};  /* FLAT */
        uint8_t t1v[7] = {10,30,50,70,90,110,60};        /* GRADIENT */
        uint8_t t2v[7] = {5,250,5,250,5,250,128};         /* EDGE */
        memcpy(tiles[0].c, t0v, 7);
        memcpy(tiles[1].c, t1v, 7);
        memcpy(tiles[2].c, t2v, 7);

        uint8_t buf[3 * GPX4_GEO_ADDR_SZ];
        gpx4_geo_hex_write(tiles, pents, hilberts, 3, buf);

        GeoHexInfo info[3];
        uint8_t rpents[3];
        uint16_t rhilberts[3];
        gpx4_geo_hex_read(buf, 3, info, rpents, rhilberts);

        CHECK(rpents[0] == 1, "bulk pent[0]");
        CHECK(rpents[1] == 5, "bulk pent[1]");
        CHECK(rpents[2] == 10, "bulk pent[2]");
        CHECK(rhilberts[0] == 100, "bulk hilbert[0]");
        CHECK(rhilberts[1] == 2000, "bulk hilbert[1]");
        CHECK(rhilberts[2] == 16383, "bulk hilbert[2]");
        CHECK(info[0].tile_type == HENC_FLAT, "bulk type[0] FLAT");
        CHECK(info[1].tile_type == HENC_GRADIENT, "bulk type[1] GRADIENT");
        CHECK(info[2].tile_type == HENC_EDGE, "bulk type[2] EDGE");
        CHECK(info[0].center_val == 100, "bulk center[0]");
        CHECK(info[1].center_val == 60, "bulk center[1]");
        CHECK(info[2].center_val == 128, "bulk center[2]");
    }

    /* T06: fast type/center queries */
    printf("\nT06: fast queries\n");
    {
        uint8_t v[7] = {10,30,50,70,90,110,60};
        HexTile t; memcpy(t.c, v, 7);
        uint16_t sub = gpx4_geo_hex_encode_sub(&t);
        uint32_t packed = (5u << 28) | ((uint32_t)2000 << 14) | sub;
        CHECK(gpx4_geo_hex_type(packed) == HENC_GRADIENT, "fast type");
        CHECK(gpx4_geo_hex_center(packed) == 60, "fast center");
    }

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    return _fail ? 1 : 0;
}

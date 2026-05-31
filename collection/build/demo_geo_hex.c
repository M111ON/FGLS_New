#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_hex_layer.h"

static void dump_tile(const char *label, const HexTile *t) {
    printf("  %s: [%3d %3d %3d %3d %3d %3d %3d]\n",
        label, t->c[0],t->c[1],t->c[2],t->c[3],t->c[4],t->c[5],t->c[6]);
}

static void dump_packed(uint32_t packed) {
    printf("  packed = 0x%08X\n", packed);
    printf("    pent_id  = %u  (bits 31..28)\n", GPX4_GEO_PENT(packed));
    printf("    hilbert  = %u  (bits 27..14)\n", GPX4_GEO_HILBERT(packed));
    printf("    sub      = 0x%04X\n", (unsigned)(packed & 0x3FFFu));
    GeoHexInfo info = gpx4_geo_hex_decode_sub((uint16_t)(packed & 0x3FFFu));
    const char *names[] = {"FLAT","SMOOTH","GRADIENT","EDGE"};
    printf("      type      = %s\n", names[info.tile_type]);
    printf("      xor_diff  = %u  (0-15)\n", (unsigned)info.xor_diff);
    printf("      center    = %u\n", (unsigned)info.center_val);
}

int main(void) {
    printf("=== geo_hex_layer.h :: real-world demo ===\n\n");

    /* -------- Tile 1: FLAT (constant value 128) -------- */
    printf("[Tile 1] FLAT — solid 128\n");
    HexTile t1 = {{128,128,128,128,128,128,128}};
    uint8_t t1_enc[16];
    int n1 = hex_tile_encode(&t1, t1_enc);
    printf("  hex_codec: %d bytes (type=0x%02X)\n", n1, t1_enc[0]);
    uint16_t sub1 = gpx4_geo_hex_encode_sub(&t1);
    uint32_t p1 = (0u << 28) | ((uint32_t)0 << 14) | sub1;
    dump_packed(p1);

    /* -------- Tile 2: real gradient (photo-like) -------- */
    printf("\n[Tile 2] GRADIENT — smooth ramp\n");
    HexTile t2 = {{20,40,60,80,100,120,70}};
    uint8_t t2_enc[16];
    int n2 = hex_tile_encode(&t2, t2_enc);
    dump_tile("input", &t2);
    printf("  hex_codec: %d bytes (type=0x%02X)\n", n2, t2_enc[0]);
    HexTile t2_dec;
    hex_tile_decode(t2_enc, sizeof(t2_enc), &t2_dec);
    dump_tile("decode", &t2_dec);
    printf("  roundtrip %s\n", memcmp(&t2, &t2_dec, 7) == 0 ? "OK" : "FAIL");
    uint16_t sub2 = gpx4_geo_hex_encode_sub(&t2);
    uint32_t p2 = (3u << 28) | ((uint32_t)512 << 14) | sub2;
    dump_packed(p2);

    /* -------- Tile 3: EDGE (high contrast edges) -------- */
    printf("\n[Tile 3] EDGE — sharp contrast\n");
    HexTile t3 = {{5,245,10,240,15,235,128}};
    uint8_t t3_enc[16];
    int n3 = hex_tile_encode(&t3, t3_enc);
    dump_tile("input", &t3);
    printf("  hex_codec: %d bytes (type=0x%02X)\n", n3, t3_enc[0]);
    HexTile t3_dec;
    hex_tile_decode(t3_enc, sizeof(t3_enc), &t3_dec);
    dump_tile("decode", &t3_dec);
    printf("  roundtrip %s\n", memcmp(&t3, &t3_dec, 7) == 0 ? "OK" : "FAIL");
    uint16_t sub3 = gpx4_geo_hex_encode_sub(&t3);
    uint32_t p3 = (11u << 28) | ((uint32_t)9999 << 14) | sub3;
    dump_packed(p3);

    /* -------- Bulk write/read demo (simulate real GEOA layer) -------- */
    printf("\n--- Bulk write: 5 tiles hex → GEOA layer buffer ---\n");
    uint8_t pents[5]    = {0, 1, 2, 3, 4};
    uint16_t hilbs[5]   = {50, 200, 1500, 8000, 16383};
    HexTile tiles[5] = {
        {{128,128,128,128,128,128,128}},        /* FLAT */
        {{100,101,100,101,100,101,100}},         /* SMOOTH */
        {{20,40,60,80,100,120,70}},              /* GRADIENT */
        {{5,245,10,240,15,235,128}},             /* EDGE */
        {{42,187,63,220,11,155,88}},             /* random */
    };
    uint8_t buf[5 * GPX4_GEO_ADDR_SZ];
    gpx4_geo_hex_write(tiles, pents, hilbs, 5, buf);

    printf("  GEOA layer size: %zu bytes (%d tiles)\n", sizeof(buf), 5);
    printf("  raw hex:");
    for (int i = 0; i < 20; i++) printf(" %02X", buf[i]);
    printf("\n");

    GeoHexInfo info[5];
    uint8_t rp[5]; uint16_t rh[5];
    gpx4_geo_hex_read(buf, 5, info, rp, rh);

    const char *names[] = {"FLAT","SMOOTH","GRADIENT","EDGE"};
    printf("\n  decode results:\n");
    for (int i = 0; i < 5; i++) {
        printf("    [%d] pent=%u hilbert=%u type=%-8s xor_diff=%u center=%3d\n",
            i, (unsigned)rp[i], (unsigned)rh[i],
            names[info[i].tile_type],
            (unsigned)info[i].xor_diff, (unsigned)info[i].center_val);
    }
    printf("\n  bulk roundtrip %s\n",
        memcmp(pents, rp, 5) == 0 && memcmp(hilbs, rh, 10) == 0 ? "OK" : "FAIL");

    return 0;
}

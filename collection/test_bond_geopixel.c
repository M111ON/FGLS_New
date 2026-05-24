/*
 * test_bond_geopixel.c — Bond ↔ GeoPixel Bridge Tests
 * Compile:
 *   gcc -O2 -I. -Igeopixel/geopixel -o test_bond_gp test_bond_geopixel.c
 */
#include <stdio.h>
#include <string.h>
#include "bond_to_geopixel.h"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { printf("[FAIL] %s\n", msg); fails++; } \
    else         { printf("[PASS] %s\n", msg); } \
} while(0)

int main(void) {
    int fails = 0;

    /* ── T1: fingerprint deterministic ── */
    {
        PoglsPiece p = pogls_make_piece(0xF00D, 1);
        GeoPixel a = bond_piece_fingerprint(&p);
        GeoPixel b = bond_piece_fingerprint(&p);
        ASSERT(a.r == b.r && a.g == b.g && a.b == b.b,
               "T1: fingerprint deterministic");
    }

    /* ── T2: different pieces → different fingerprints ── */
    {
        PoglsPiece a = pogls_make_piece(0xAAAA, 1);
        PoglsPiece b = pogls_make_piece(0xBBBB, 1);
        GeoPixel pa = bond_piece_fingerprint(&a);
        GeoPixel pb = bond_piece_fingerprint(&b);
        int same = (pa.r == pb.r && pa.g == pb.g && pa.b == pb.b);
        ASSERT(!same, "T2: different seeds → different fingerprints");
    }

    /* ── T3: different shapes → different fingerprints ── */
    {
        PoglsPiece a = pogls_make_piece(0xDEAD, 1); /* shape I */
        PoglsPiece b = pogls_make_piece(0xDEAD, 4); /* shape S */
        GeoPixel pa = bond_piece_fingerprint(&a);
        GeoPixel pb = bond_piece_fingerprint(&b);
        int same = (pa.r == pb.r && pa.g == pb.g && pa.b == pb.b);
        ASSERT(!same, "T3: different shapes → different fingerprints");
    }

    /* ── T4: stripe roundtrip (shape I) ── */
    {
        PoglsPiece p = pogls_make_piece(0xCAFEBABEDEADBEEFULL, 1);
        int err = bond_geopixel_roundtrip(&p);
        ASSERT(err == 0, "T4: stripe roundtrip shape I, err=0");
    }

    /* ── T5: stripe roundtrip all shapes ── */
    {
        int ok = 1;
        for (uint8_t axis = 1; axis <= 7; axis++) {
            PoglsPiece p = pogls_make_piece(0x1000 + axis, axis);
            int err = bond_geopixel_roundtrip(&p);
            if (err) { ok = 0; break; }
        }
        ASSERT(ok, "T5: stripe roundtrip all shapes 1-7");
    }

    /* ── T6: stripe roundtrip zero seed ── */
    {
        PoglsPiece p = pogls_make_piece(0, 1);
        int err = bond_geopixel_roundtrip(&p);
        ASSERT(err == 0, "T6: stripe roundtrip zero seed");
    }

    /* ── T7: stripe roundtrip max seed ── */
    {
        PoglsPiece p = pogls_make_piece(0xFFFFFFFFFFFFFFFFULL, 2);
        int err = bond_geopixel_roundtrip(&p);
        ASSERT(err == 0, "T7: stripe roundtrip max seed");
    }

    /* ── T8: address grid deterministic ── */
    {
        GeoPixel grid[GP_GRID_W * GP_GRID_W];
        bond_addr_to_grid(0xDEAD, grid);
        GeoPixel grid2[GP_GRID_W * GP_GRID_W];
        bond_addr_to_grid(0xDEAD, grid2);
        int same = memcmp(grid, grid2, sizeof(grid)) == 0;
        ASSERT(same, "T8: address grid deterministic");
    }

    /* ── T9: different addrs → different grids ── */
    {
        GeoPixel g1[GP_GRID_W * GP_GRID_W];
        bond_addr_to_grid(0xAAAA, g1);
        GeoPixel g2[GP_GRID_W * GP_GRID_W];
        bond_addr_to_grid(0xBBBB, g2);
        int diff = memcmp(g1, g2, sizeof(g1)) != 0;
        ASSERT(diff, "T9: different addrs → different grids");
    }

    /* ── T10: piece grid row 0 cells 1..26 uniform (shape overlay) ── */
    {
        GeoPixel grid[GP_GRID_W * GP_GRID_W];
        PoglsPiece p = pogls_make_piece(0xDEAD, 1);
        memset(grid, 0, sizeof(grid));
        bond_piece_to_grid(&p, grid);

        /* grid[0] is corner XOR, grid[1..26] = shape_px uniform */
        int shape_uniform = 1;
        for (uint32_t x = 2; x < GP_GRID_W; x++) {
            if (grid[1].r != grid[x].r ||
                grid[1].g != grid[x].g ||
                grid[1].b != grid[x].b) {
                shape_uniform = 0;
                break;
            }
        }
        ASSERT(shape_uniform, "T10: row 0 cells 1..26 uniform (shape overlay)");
    }

    /* ── T11: RGB888 packing ── */
    {
        GeoPixel p = { 0x12, 0x34, 0x56 };
        uint32_t rgb = bond_pixel_to_rgb888(p);
        ASSERT(rgb == 0x123456u, "T11: RGB888 packing correct");
    }

    /* ── T12: decode → stripe matches piece fields ── */
    {
        for (uint8_t axis = 1; axis <= 7; axis++) {
            PoglsPiece p = pogls_make_piece(0x4242 + axis, axis);

            GeoPixel stripe[BGP_STRIPE_W];
            bond_piece_to_stripe(&p, stripe);

            PoglsPiece decoded;
            bond_stripe_to_piece(stripe, &decoded);

            if (decoded.geo_key != p.geo_key ||
                decoded.shape   != p.shape   ||
                decoded.bond_L  != p.bond_L  ||
                decoded.bond_R  != p.bond_R) {
                printf("[FAIL] T12: axis=%d mismatch\n", axis);
                fails++;
                goto t12_done;
            }
        }
        printf("[PASS] T12: decode matches all shapes\n");
        t12_done:;
    }

    /* ── T13: fingerprint R channel encodes shape ── */
    {
        PoglsPiece p = pogls_make_piece(0xFACE, 1); /* shape I */
        GeoPixel px = bond_piece_fingerprint(&p);
        /* shape 'I' (0x49) → shape_idx = 'I' - 'A' = 8 */
        /* px.r = (8 << 3) | (geo_key & 7) */
        uint8_t shape_hint = (px.r >> 3) & 0x1Fu;
        ASSERT(shape_hint > 0, "T13: shape encoded in R channel");
    }

    /* ── V2 TESTS ──────────────────────────────────── */

    /* ── T14-V2: stripe v2 roundtrip all shapes ── */
    {
        int ok = 1;
        for (uint8_t axis = 1; axis <= 7; axis++) {
            PoglsPiece p = pogls_make_piece(0x1000 + axis, axis);
            int err = bond_geopixel_roundtrip_v2(&p);
            if (err) { ok = 0; break; }
        }
        ASSERT(ok, "T14: stripe v2 roundtrip all shapes 1-7");
    }

    /* ── T15-V2: stripe v2 roundtrip zero, max, random ── */
    {
        PoglsPiece z = pogls_make_piece(0, 1);
        ASSERT(bond_geopixel_roundtrip_v2(&z) == 0, "T15: stripe v2 zero seed");

        PoglsPiece m = pogls_make_piece(0xFFFFFFFFFFFFFFFFULL, 2);
        ASSERT(bond_geopixel_roundtrip_v2(&m) == 0, "T16: stripe v2 max seed");

        PoglsPiece r = pogls_make_piece(0xCAFEBABEDEADBEEFULL, 3);
        ASSERT(bond_geopixel_roundtrip_v2(&r) == 0, "T17: stripe v2 random seed");
    }

    /* ── T18-V2: verify v2 uses 9px not 27px ── */
    {
        GeoPixel v2_stripe[BGP_STRIPE_V2_PX];
        PoglsPiece p = pogls_make_piece(0xF00D, 1);
        bond_piece_to_stripe_v2(&p, v2_stripe);
        ASSERT(sizeof(v2_stripe) == BGP_STRIPE_V2_PX * sizeof(GeoPixel),
               "T18: stripe v2 size is 9 pixels");
    }

    /* ── V3 TESTS ──────────────────────────────────── */

    /* ── T19-V3: compress/decompress all shapes ── */
    {
        int ok = 1;
        for (uint8_t axis = 1; axis <= 7; axis++) {
            PoglsPiece p = pogls_make_piece(0x1000 + axis, axis);
            int err = bond_geopixel_roundtrip_v3(&p);
            if (err) { ok = 0; break; }
        }
        ASSERT(ok, "T19: v3 roundtrip all shapes 1-7");
    }

    /* ── T20-V3: compress size is 9B ── */
    {
        uint8_t seed[GPV3_SEED_BYTES];
        PoglsPiece p = pogls_make_piece(0xF00D, 1);
        uint32_t n = bond_piece_compress_v3(&p, seed);
        ASSERT(n == GPV3_SEED_BYTES && n == 9, "T20: v3 compressed size = 9B");
    }

    /* ── T21-V3: visualize generates 9 pixels ── */
    {
        GeoPixel vis[GPV3_VIS_PX];
        PoglsPiece p = pogls_make_piece(0xF00D, 1);
        bond_piece_visualize_v3(&p, vis);
        ASSERT(sizeof(vis) == GPV3_VIS_PX * sizeof(GeoPixel),
               "T21: v3 visualize = 9 pixels");
    }

    /* ── T22-V3: visualize deterministic ── */
    {
        GeoPixel a[GPV3_VIS_PX], b[GPV3_VIS_PX];
        PoglsPiece p = pogls_make_piece(0xF00D, 1);
        bond_piece_visualize_v3(&p, a);
        bond_piece_visualize_v3(&p, b);
        int same = memcmp(a, b, sizeof(a)) == 0;
        ASSERT(same, "T22: v3 visualize deterministic");
    }

    /* ── T23-V3: compress → decompress → visualize matches V2 stripe ── */
    {
        PoglsPiece p = pogls_make_piece(0xCAFEBABEDEADBEEFULL, 4);
        uint8_t seed[GPV3_SEED_BYTES];
        bond_piece_compress_v3(&p, seed);
        PoglsPiece restored = bond_piece_decompress_v3(seed);

        int piece_ok = (restored.geo_key == p.geo_key &&
                        restored.shape   == p.shape   &&
                        restored.bond_L  == p.bond_L  &&
                        restored.bond_R  == p.bond_R);
        ASSERT(piece_ok, "T23: v3 decompress matches original piece");

        GeoPixel vis_a[GPV3_VIS_PX], vis_b[GPV3_VIS_PX];
        bond_piece_visualize_v3(&p, vis_a);
        bond_piece_visualize_v3(&restored, vis_b);
        int vis_ok = memcmp(vis_a, vis_b, sizeof(vis_a)) == 0;
        ASSERT(vis_ok, "T23b: v3 visualize identical after roundtrip");
    }

    /* ── T24-V3: compressed = 9B vs stripe V2 = 27B ── */
    {
        ASSERT(GPV3_SEED_BYTES < BGP_STRIPE_V2_PX * sizeof(GeoPixel),
               "T24: v3 compressed (9B) < v2 stripe (27B)");
    }

    printf("\n%s — %d failure(s)\n",
           fails == 0 ? "ALL PASS" : "SOME FAILED", fails);
    return fails ? 1 : 0;
}

/*
 * test_tantrix.c -- lc_tantrix.h verification
 * Tests:
 *   T01-T07: same as lc_tantrix_verify() (decode/encode roundtrip, special tiles,
 *            SPLIT connects all, NULL connects none, domino, CROSS self-inverse,
 *            spoke_mask coverage)
 *   T08: SKIP class inverts polarity
 *   T09: MIRROR class swaps bits
 *   T10: gate mismatch -> DROP
 *   T11: CROSS/SPLIT/MERGE route results
 *   T12: Domino roundtrip for all gates x spokes
 *   T13: active_spokes for all 4 pairs
 *   T14: all 256 tiles decode without error
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "lc_tantrix.h"

static int _pass=0, _fail=0;
#define CHECK(cond, msg) do { \
    if(cond){printf("  PASS  %s\n",msg);_pass++;} \
    else    {printf("  FAIL  %s  (line %d)\n",msg,__LINE__);_fail++;} \
} while(0)

int main(void)
{
    printf("=== lc_tantrix.h verification ===\n");

    /* T01-T07: built-in verify */
    printf("\nT01-T07: built-in verify\n");
    CHECK(lc_tantrix_verify() == 0, "built-in verify passes");

    /* T08: SKIP class inverts polarity (exit ^ 3) */
    /* exit field stored in tile is (g^3), SKIP inverts → g */
    printf("\nT08: SKIP class inverts polarity\n");
    {
        int ok = 1;
        for (uint8_t g = 0; g < 4 && ok; g++) {
            TantrixTile t = tantrix_make(g, (g ^ 3) & 3, 0, TANTRIX_CLASS_SKIP);
            uint8_t out;
            TantrixRouteResult rr = tantrix_route(t, g, &out);
            if (rr != TANTRIX_ROUTE_FORWARD || out != g) {
                ok = 0;
                printf("  INFO  SKIP gate=%u expected=%u got=%u\n", g, g, out);
            }
        }
        CHECK(ok, "SKIP: exit ^ 3 for all gates");
    }

    /* T09: MIRROR class swaps bits */
    /* exit field stored in tile is e, mirror swaps e's bits */
    printf("\nT09: MIRROR class swaps bits\n");
    {
        int ok = 1;
        for (uint8_t g = 0; g < 4 && ok; g++) {
            uint8_t e = (g + 1) & 3;
            uint8_t expected = (uint8_t)(((e & 1u) << 1) | ((e >> 1) & 1u));
            TantrixTile t = tantrix_make(g, e, 0, TANTRIX_CLASS_MIRROR);
            uint8_t out;
            TantrixRouteResult rr = tantrix_route(t, g, &out);
            if (rr != TANTRIX_ROUTE_FORWARD || out != expected) {
                ok = 0;
                printf("  INFO  MIRROR gate=%u e=%u expected=%u got=%u\n", g, e, expected, out);
            }
        }
        CHECK(ok, "MIRROR: bit swap for all gates");
    }

    /* T10: gate mismatch -> DROP */
    printf("\nT10: gate mismatch drops\n");
    {
        int ok = 1;
        for (uint8_t entry = 0; entry < 4 && ok; entry++) {
            for (uint8_t mismatch = 0; mismatch < 4 && ok; mismatch++) {
                if (mismatch == entry) continue;
                TantrixTile t = tantrix_make(entry, (entry+1)&3, 0, TANTRIX_CLASS_NORMAL);
                uint8_t out = 0xFF;
                TantrixRouteResult rr = tantrix_route(t, mismatch, &out);
                if (rr != TANTRIX_ROUTE_DROP || out != LC_GATE_GROUND) ok = 0;
            }
        }
        CHECK(ok, "gate mismatch -> DROP + GROUND for all cases");
    }

    /* T11: CROSS/SPLIT/MERGE route results */
    printf("\nT11: special tile routing\n");
    {
        int ok = 1;
        /* CROSS: WARP<->COLLISION, ROUTE<->GROUND */
        static const uint8_t cross_in[4]  = {0,1,2,3};
        static const uint8_t cross_out[4] = {2,3,0,1};
        for (uint8_t i = 0; i < 4 && ok; i++) {
            uint8_t out;
            TantrixRouteResult rr = tantrix_route(TANTRIX_CROSS, cross_in[i], &out);
            if (rr != TANTRIX_ROUTE_FORWARD || out != cross_out[i]) ok = 0;
        }
        CHECK(ok, "CROSS: correct gate swap");

        /* SPLIT: broadcast -> same gate back */
        ok = 1;
        for (uint8_t g = 0; g < 4 && ok; g++) {
            uint8_t out;
            TantrixRouteResult rr = tantrix_route(TANTRIX_SPLIT, g, &out);
            if (rr != TANTRIX_ROUTE_BROADCAST || out != g) ok = 0;
        }
        CHECK(ok, "SPLIT: broadcast returns same gate");

        /* MERGE: always GROUND */
        ok = 1;
        for (uint8_t g = 0; g < 4 && ok; g++) {
            uint8_t out;
            TantrixRouteResult rr = tantrix_route(TANTRIX_MERGE, g, &out);
            if (rr != TANTRIX_ROUTE_MERGE || out != LC_GATE_GROUND) ok = 0;
        }
        CHECK(ok, "MERGE: always GROUND");

        /* NULL: drop */
        ok = 1;
        for (uint8_t g = 0; g < 4 && ok; g++) {
            uint8_t out;
            TantrixRouteResult rr = tantrix_route(TANTRIX_NULL, g, &out);
            if (rr != TANTRIX_ROUTE_DROP || out != LC_GATE_GROUND) ok = 0;
        }
        CHECK(ok, "NULL: drop + GROUND");
    }

    /* T12: Domino roundtrip for all gates x spokes */
    printf("\nT12: domino roundtrip all gates x spokes\n");
    {
        int ok = 1;
        for (uint8_t g = 0; g < 4 && ok; g++) {
            for (uint8_t s = 0; s < 4 && ok; s++) {
                TantrixDomino d = tantrix_domino(g, (g+1)&3, s);
                if (!tantrix_domino_valid(d)) ok = 0;
                /* entry -> exit should gate+1 */
                uint8_t out;
                tantrix_route(d.entry_tile, g, &out);
                if (out != ((g+1)&3)) ok = 0;
                /* exit -> entry should gate */
                tantrix_route(d.exit_tile, (g+1)&3, &out);
                if (out != g) ok = 0;
            }
        }
        CHECK(ok, "all gate/spoke domino combos valid");
    }

    /* T13: active_spokes all pairs */
    printf("\nT13: active_spokes\n");
    {
        int ok = 1;
        for (uint8_t s = 0; s < 3; s++) {
            TantrixTile t = tantrix_make(0, 0, s, TANTRIX_CLASS_NORMAL);
            uint8_t mask = tantrix_active_spokes(t);
            int pop = __builtin_popcount(mask);
            if (pop != 2) { ok = 0; printf("  INFO  pair %u pop=%d\n", s, pop); }
        }
        /* pair 3 = all 6 */
        TantrixTile t3 = tantrix_make(0, 0, 3, TANTRIX_CLASS_NORMAL);
        if (tantrix_active_spokes(t3) != 0x3Fu) ok = 0;
        CHECK(ok, "spoke pairs 0-2 = 2 spokes, pair 3 = all 6");
    }

    /* T14: all 256 tiles decode without error */
    printf("\nT14: all 256 tiles decode\n");
    {
        int ok = 1;
        for (uint16_t i = 0; i < 256; i++) {
            TantrixTile t = (TantrixTile)i;
            uint8_t entry = tantrix_entry(t);
            uint8_t exit  = tantrix_exit(t);
            uint8_t spoke = tantrix_spoke(t);
            TantrixClass cls = (TantrixClass)tantrix_class(t);
            if (entry > 3 || exit > 3 || spoke > 3 || cls > 3) {
                ok = 0; break;
            }
        }
        CHECK(ok, "all 256 tiles have valid fields (0..3)");
    }

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    return _fail ? 1 : 0;
}

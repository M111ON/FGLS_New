/*
 * _cardioid_demo.c — standalone Cardioid Express visual demo
 * compile: gcc -O2 -Wall -o _cardioid_demo _cardioid_demo.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tgw_cardioid_express.h"

int main(void)
{
    cardioid_lut_init();
    printf("=== Cardioid Express Lane Demo ===\n\n");

    /* ── Part 1: Scan all 720 positions with fixed r_data=0xAB ── */
    printf("[Part 1] 720 positions, r=0xAB\n");
    printf("  m=7  pos*7%%720  bijective\n");
    printf("  GEO_MIN_Q8=%d\n\n", CARDIOID_GEO_MIN_Q8);
    uint32_t cusp=0, linear=0, express=0;
    for (uint16_t pos = 0; pos < 720; pos++) {
        int pass = cardioid_pass(pos, 0xAB);
        if (pass == CARDIOID_PASS_CUSP)    cusp++;
        else if (pass == CARDIOID_PASS_LINEAR)  linear++;
        else if (pass == CARDIOID_PASS_EXPRESS) express++;
    }
    uint32_t total = cusp + linear + express;
    printf("  CUSP:    %4u  (%5.1f%%)\n", cusp,   100.0*cusp/total);
    printf("  LINEAR:  %4u  (%5.1f%%)\n", linear,  100.0*linear/total);
    printf("  EXPRESS: %4u  (%5.1f%%)\n", express, 100.0*express/total);
    printf("  Express rate: %.1f%%\n\n", 100.0*express/(express+linear));

    /* ── Part 2: Sweep r values ── */
    printf("[Part 2] r_data sweep (0x00..0xFF) at pos=120\n");
    for (uint16_t r = 0; r <= 0xFF; r += 0x20) {
        int pass = cardioid_pass(120, r);
        const char *label = (pass==CARDIOID_PASS_EXPRESS)?"EXPRESS"
                          : (pass==CARDIOID_PASS_LINEAR)?"LINEAR ":"CUSP   ";
        printf("  r=0x%02X (%3u) → %s\n", r, r, label);
    }
    printf("\n");

    /* ── Part 3: Visual cardioid curve (ascii) ── */
    printf("[Part 3] Cardioid curve r(theta)=A(1+cos(theta))\n");
    printf("  '*' = express zone (r_geo >= %d)\n", CARDIOID_GEO_MIN_Q8);
    printf("  '.' = cusp zone\n\n");
    for (uint16_t pos = 0; pos < 720; pos += 12) {
        int32_t cos_v = _cardioid_cos_lut[pos];
        int32_t r_geo = CARDIOID_A * (CARDIOID_SCALE + cos_v);
        int32_t r_q8  = r_geo >> 8;
        char c = (r_q8 < CARDIOID_GEO_MIN_Q8) ? '.' : '*';
        printf("  pos=%3u  r_geo_q8=%3d %c\n", pos, r_q8, c);
    }

    /* ── Part 4: Route spread ── */
    printf("\n[Part 4] Route spread m=7 (first 20 positions)\n");
    printf("  pos -> next_pos (express)  vs  (pos+1)%720 (linear)\n");
    for (uint16_t pos = 0; pos < 20; pos++) {
        uint16_t expr = cardioid_route(pos, 7);
        uint16_t linr = (pos + 1) % 720;
        printf("  %3u -> %3u (express)  %3u (linear)\n", pos, expr, linr);
    }

    /* ── Part 5: m-value comparison ── */
    printf("\n[Part 5] m-value spread comparison\n");
    uint16_t m_vals[] = {2,3,5,7,11,13,17,19,23};
    for (int mi = 0; mi < 9; mi++) {
        uint16_t m = m_vals[mi];
        uint8_t seen[720] = {0};
        uint32_t unique = 0;
        for (uint16_t p = 0; p < 720; p++) {
            uint16_t nxt = cardioid_route(p, m);
            if (!seen[nxt]) { seen[nxt]=1; unique++; }
        }
        printf("  m=%2u  unique destinations=%u  bijective=%s\n",
               m, unique, (unique==720)?"YES":"NO");
    }

    return 0;
}

/*
 * demo_p5h_fibo.c — P5H Pipe Domain x FiboSpine Herringbone
 */

#include <stdio.h>
#include <stdint.h>
#include "src/fibo_spine.h"

static CTDCard make_card(uint8_t face, uint8_t chirality)
{
    CTDCard c;
    c.dodeca_face = face;
    c.chirality = chirality;
    return c;
}

int main(void)
{
    uint32_t max_ticks = 96;
    CTDCard card = make_card(0, CTD_CHIRAL_L);

    P5HHerringbone ph[3];
    ph[0] = p5h_hbone_init(&card, 1);
    ph[1] = p5h_hbone_init(&card, 3);
    ph[2] = p5h_hbone_init(&card, 5);

    printf("=== P5H Pipe Domain x FiboSpine Herringbone ===\n");
    printf("bars hit / barriers expected = ?/8 (96 ticks = 8 barriers)\n\n");

    uint32_t barriers_hit[3] = {0, 0, 0};

    for (uint32_t t = 1; t <= max_ticks; t++) {
        for (int i = 0; i < 3; i++) {
            SpineConvergence out;
            int bar = p5h_hbone_step_textured(&ph[i], &out);
            if (bar) {
                barriers_hit[i]++;
                uint32_t fid = ph[i].field.flower_now;
                printf("t=%3u  ph[%d] BARRIER #%u  flower=%u  face=%u\n",
                       t, i, barriers_hit[i], fid, ph[i].hb.L.face);
            }
        }
    }

    printf("\n=== Result ===\n");
    int all_good = 1;
    for (int i = 0; i < 3; i++) {
        uint32_t exp = max_ticks / 12;
        printf("  ph[%d]: %u/%u barriers hit (rate=%u,%u)\n",
               i, barriers_hit[i], exp, ph[i].hb.L.rate, ph[i].hb.R.rate);
        if (barriers_hit[i] != exp) all_good = 0;
    }
    printf("\n%s\n", all_good ? "PASS - all barriers convergent" : "FAIL");
    return all_good ? 0 : 1;
}

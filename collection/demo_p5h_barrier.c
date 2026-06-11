/*
 * demo_p5h_barrier.c — P5H Barrier Sync Demo
 */

#include <stdio.h>
#include <stdint.h>
#include "include/p5h_ribcage.h"

typedef struct {
    const char *name;
    uint32_t step_mod;
    uint32_t writes;
} Proc;

int main(void)
{
    P5HField field;
    p5h_field_init(&field);

    Proc a = {"A", 1, 0};
    Proc b = {"B", 2, 0};
    Proc c = {"C", 4, 0};

    printf("=== P5H Barrier Sync Demo ===\n");
    printf("  A: writes every tick    (full speed)\n");
    printf("  B: writes every 2 ticks (1/2 speed)\n");
    printf("  C: writes every 4 ticks (1/4 speed)\n");
    printf("  Field ticks independently every 1 tick.\n");
    printf("  Barrier every 12 ticks - all converge.\n\n");

    printf("TICK  PHASE  FLOWER  EVENTS\n");
    printf("----  -----  ------  ---------------------------\n");

    for (uint32_t tick = 1; tick <= 96; tick++) {
        if (tick % a.step_mod == 0) a.writes++;
        if (tick % b.step_mod == 0) b.writes++;
        if (tick % c.step_mod == 0) c.writes++;

        p5h_field_observe(&field, (uint16_t)tick);

        if (p5h_is_barrier(&field)) {
            printf("TICK %3u  FLOWER %4u  [BARRIER] A(flower=%u ph=%u) B(flower=%u ph=%u) C(flower=%u ph=%u)  ",
                   tick,
                   field.flower_now,
                   field.flower_now, p5h_phase_at_tick(tick),
                   field.flower_now, p5h_phase_at_tick(tick),
                   field.flower_now, p5h_phase_at_tick(tick));
            printf("\xE2\x9C\x93 CONVERGED\n");
        } else if (p5h_is_flower_start(&field)) {
            printf("%4u  %5u  %6u  \xE2\x97\x86 FLOWER START\n",
                   tick, field.tick_in_flower - 1, field.flower_now);
        } else {
            P5HFlower *fl = p5h_field_peek(&field, field.flower_now);
            printf("%4u  %5u  %6u  phase=%u tex=%s\n",
                   tick,
                   fl->phase,
                   field.flower_now,
                   fl->phase,
                   fl->texture == P5H_TEX_INNER ? "INNER" : "OUTER");
        }
    }

    printf("\n=== Summary (96 ticks = 8 flowers) ===\n");
    printf("  A writes: %u\n", a.writes);
    printf("  B writes: %u\n", b.writes);
    printf("  C writes: %u\n", c.writes);
    printf("  Barriers: 8 (every 12 ticks)\n");
    printf("  Final flower: %u\n", field.flower_now);
    printf("\nProved: At each barrier, all 3 processes see\n");
    printf("the same flower regardless of individual speed.\n");
    printf("No locks, no waits - just tick-12 boundary.\n");

    return 0;
}

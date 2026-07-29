/*
 * test_tw_capture.c — Test integer triwheel capture vs GT2
 *
 * gcc -I. tests/test_tw_capture.c -o tests/test_tw_capture -lm && ./tests/test_tw_capture
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ctd_octa.h"
#include "ctd_goldberg_tri.h"
#include "tw_capture_int.h"

typedef struct { const char *name; float f[6]; } TSim;
static TSim T[] = {
    {"text_model.layers.0.attn.q_proj.weight",   {0.01f,0.02f,0.015f,0.5f,0.3f,0.2f}},
    {"text_model.layers.0.attn.k_proj.weight",   {0.01f,0.01f,0.012f,0.4f,0.4f,0.3f}},
    {"text_model.layers.1.ffn.gate_proj.weight", {0.05f,0.06f,0.04f, 1.2f,0.8f,0.6f}},
    {"text_model.layers.1.ffn.down_proj.weight", {0.08f,0.07f,0.09f, 2.1f,1.5f,1.2f}},
    {"vision_model.encoder.layers.0.attn.q",     {0.2f, 0.3f, 0.25f, 5.0f,4.0f,3.5f}},
    {"vision_model.encoder.layers.4.mlp.fc2",    {0.5f, 0.8f, 0.6f,  9.0f,7.0f,6.0f}},
    {"model.layers.0.attn.q_proj.weight",        {0.03f,0.04f,0.03f, 0.8f,0.6f,0.5f}},
    {"model.layers.20.ffn.down_proj.weight",     {0.1f, 0.12f,0.09f, 3.0f,2.5f,2.0f}},
    {"voice.encoder.layers.0.weight",            {0.02f,0.02f,0.02f, 0.3f,0.3f,0.3f}},
    {"voice.decoder.layers.5.weight",            {0.04f,0.05f,0.04f, 0.6f,0.5f,0.5f}},
};
#define NT 10

static int zone_counts[TW_N_SECTORS] = {0};
static int slot_counts[TW_N_SLOTS] = {0};

/* Scale a float feature to int32 * TW_SCALE */
static inline int32_t tw_scale(float f)
{
    return (int32_t)(f * TW_SCALE);
}

int main(void)
{
    printf("=== Integer Triwheel Capture vs Goldberg GT2 ===\n\n");
    printf("Scale: %d (12^5)\n\n", TW_SCALE);

    printf("%-44s Z/S  resid         drain    GT2_sh  GT2_f\n","Tensor");
    printf("%-44s ---- ------------  ------   ------  -----\n","------");

    for (int i = 0; i < NT; i++) {
        /* Use (f0,f1) as 2D capture — the z component (f2) is loss in plane */
        int32_t vx = tw_scale(T[i].f[0] + T[i].f[3]);  /* x from combined */
        int32_t vy = tw_scale(T[i].f[1] + T[i].f[4]);  /* y from combined */

        /* Actually try separate: core (f0,f1) and ring (f3,f4) both captured */
        /* For this test: combined vector */
        TWCaptureInt cap;
        tw_capture_int(vx, vy, &cap);

        zone_counts[cap.zone]++;
        slot_counts[cap.slot]++;

        /* GT2 */
        GTState gs; gt_init(&gs, T[i].name);
        gt_snapshot(&gs);
        gt2_encode(&gs, T[i].f[0],T[i].f[1],T[i].f[2],
                    T[i].f[3],T[i].f[4],T[i].f[5]);

        const char *nm = T[i].name;
        int nl = strlen(nm); if(nl>43) nm=nm+nl-43;

        printf("%-44s %d/%d  (%+5d,%+5d)  %s",
               nm,
               cap.zone, cap.slot,
               cap.resid_x, cap.resid_y,
               cap.drain ? "drain " : "      ");

        if (cap.drain) {
            printf("→Z%d/S%02d ", cap.drain_zone, cap.drain_slot);
        } else {
            printf("         ");
        }
        printf(" %7.4f  %d\n", gs.gt_shift, gs.face);
    }

    printf("\n--- Zone Distribution ---\n");
    for (int z = 0; z < TW_N_SECTORS; z++) {
        printf("  zone %d: %d tensors\n", z, zone_counts[z]);
    }

    printf("\n--- Reconstruction Test ---\n");
    int ok = 1;
    for (int t = 0; t < 5; t++) { /* test first 5 */
        int32_t vx = tw_scale(T[t].f[0] + T[t].f[3]);
                int32_t vy = tw_scale(T[t].f[1] + T[t].f[4]);
                TWCaptureInt cap;
                tw_capture_int(vx, vy, &cap);
                int64_t rx, ry;
                tw_reconstruct_int(&cap, &rx, &ry);
                if (rx != vx || ry != vy) {
            printf("  MISMATCH T%d: (%d,%d) ≠ (%d,%d)\n", t, vx, vy, rx, ry);
            ok = 0;
        }
    }
    if (ok) printf("  All 5 reconstruction OK (exact)\n");

    return 0;
}

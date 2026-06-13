/*
 * test_triwheel.c
 * Triangle Wheel vs Goldberg GT2: compare activation on 10 test tensors
 *
 * gcc test_triwheel.c -o test_triwheel -lm && ./test_triwheel
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "ctd_octa.h"
#include "ctd_goldberg_tri.h"
#include "ctd_triwheel.h"

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

int main(void)
{
    printf("=== Triangle Wheel vs Goldberg GT2 ===\n\n");

    /* Init triangle wheel */
    TriWheel tw;
    tw_init(&tw, 0.f, 1.f);

    printf("%-42s  %10s  %8s  %8s\n","Tensor","TW_zone","TW_shift","GT2_shift");
    printf("%-42s  %10s  %8s  %8s\n","------","-------","--------","---------");

    float sum_tw = 0, sum_gt2 = 0;
    int tw_faces[12] = {0};

    for (int i = 0; i < NT; i++) {
        /* Triangle wheel: core activation from (f0,f1,f2), ring from (f3,f4,f5) */
        int    tw_idx[3];
        float  tw_conf[3];

        /* Combined activation */
        float dir[3] = { T[i].f[0] + T[i].f[3],
                         T[i].f[1] + T[i].f[4],
                         T[i].f[2] + T[i].f[5] };
        int n = tw_activate(&tw, dir, 2, tw_idx, tw_conf);

        /* Best = closest triangle */
        int best = tw_idx[0];
        float shift = sqrtf(tw.centroids[best][0]*tw.centroids[best][0] +
                            tw.centroids[best][1]*tw.centroids[best][1]);

        /* Goldberg GT2 */
        GTState gs; gt_init(&gs, T[i].name);
        gt_snapshot(&gs);
        gt2_encode(&gs, T[i].f[0],T[i].f[1],T[i].f[2],
                    T[i].f[3],T[i].f[4],T[i].f[5]);

        printf("%-42s  Δ%-3d(%-4s)  %8.4f  %8.4f%s\n",
               T[i].name,
               best,
               best < TW_CORE ? "core" : "ring",
               shift, gs.gt_shift,
               shift < gs.gt_shift ? "  <-- TW wins" : "");

        sum_tw += shift;
        sum_gt2 += gs.gt_shift;

        /* Track face coverage (zone → face) */
        int face_idx = best % 12;
        if (face_idx < 12) tw_faces[face_idx]++;
    }

    printf("\n");
    printf("%-42s  %10s  %8.4f  %8.4f\n","AVERAGE","", sum_tw/NT, sum_gt2/NT);

    int nf = 0;
    for (int i = 0; i < 12; i++) if (tw_faces[i]) nf++;
    printf("TW face coverage: %d/12\n", nf);

    printf("\n--- Per-tensor detail ---\n");
    for (int i = 0; i < NT; i++) {
        GTState gs; gt_init(&gs, T[i].name);
        gt_snapshot(&gs);
        gt2_encode(&gs, T[i].f[0],T[i].f[1],T[i].f[2],
                    T[i].f[3],T[i].f[4],T[i].f[5]);

        float dir[3] = { T[i].f[0] + T[i].f[3],
                         T[i].f[1] + T[i].f[4],
                         T[i].f[2] + T[i].f[5] };
        int idx[3]; float cf[3];
        int n = tw_activate(&tw, dir, 3, idx, cf);

        float shift = sqrtf(tw.centroids[idx[0]][0]*tw.centroids[idx[0]][0] +
                            tw.centroids[idx[0]][1]*tw.centroids[idx[0]][1]);

        printf("%s\n", T[i].name);
        printf("  TW best=Δ%d shift=%.4f zone=%s\n",
               idx[0], shift,
               tw.hex_group[idx[0]] < 0 ? "core" : "ring");
        printf("  GT2    shift=%.4f face=%d\n", gs.gt_shift, gs.face);
        if (n > 1)
            printf("  TW runner-up=Δ%d conf=%.4f\n", idx[1], cf[1]);
    }

    return 0;
}

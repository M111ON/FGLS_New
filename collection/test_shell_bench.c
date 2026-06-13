/*
 * test_shell_bench.c — Shell vs Goldberg vs P5H vs Y6
 *
 * gcc test_shell_bench.c -o test_shell -lm && ./test_shell
 */
#include <stdio.h>
#include <string.h>
#include "ctd_octa.h"
#include "ctd_pent5hex.h"
#include "ctd_goldberg.h"
#include "ctd_goldberg_tri.h"
#include "ctd_g10.h"
#include "ctd_shell.h"

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
    printf("=== G10 vs GT2 vs GB vs SH ===\n\n");
    printf("%-42s   G10m1   G10m2   GT2    GB     SH   Best\n","Tensor");
    printf("%-42s  ------  ------  -----  -----  -----  ----\n","------");

    float sum[5] = {0};
    int win[5] = {0};

    for (int i = 0; i < NT; i++) {
        G10State g10m1; g10_init(&g10m1,T[i].name); g10_snapshot(&g10m1);
        g10m1_encode(&g10m1,T[i].f[0],T[i].f[1],T[i].f[2],T[i].f[3],T[i].f[4],T[i].f[5]);

        G10State g10m2; g10_init(&g10m2,T[i].name); g10_snapshot(&g10m2);
        g10m2_encode(&g10m2,T[i].f[0],T[i].f[1],T[i].f[2],T[i].f[3],T[i].f[4],T[i].f[5]);

        GTState gt2; gt_init(&gt2,T[i].name); gt_snapshot(&gt2);
        gt2_encode(&gt2,T[i].f[0],T[i].f[1],T[i].f[2],T[i].f[3],T[i].f[4],T[i].f[5]);

        GBState gb; gb_init(&gb,T[i].name); gb_snapshot(&gb);
        gb_encode(&gb,T[i].f[0],T[i].f[1],T[i].f[2],T[i].f[3],T[i].f[4],T[i].f[5]);

        ShellState sh; shell_init(&sh,T[i].name); shell_snapshot(&sh);
        shell_encode(&sh,T[i].f[0],T[i].f[1],T[i].f[2],T[i].f[3],T[i].f[4],T[i].f[5]);

        float vals[5] = {g10m1.g10_shift, g10m2.g10_shift, gt2.gt_shift, gb.gb_shift, sh.shift};
        float min_v = vals[0];
        int best = 0;
        for (int k = 1; k < 5; k++) {
            if (vals[k] < min_v) { min_v = vals[k]; best = k; }
        }
        win[best]++;

        const char *names[5] = {"G10m1","G10m2","GT2","GB ","SH "};
        const char *nm = T[i].name;
        int nl = (int)strlen(nm); if (nl > 41) nm = nm + nl - 41;
        printf("%-42s  %6.3f  %6.3f  %5.3f  %5.3f  %5.3f  %s\n",
               nm, vals[0], vals[1], vals[2], vals[3], vals[4], names[best]);

        for (int k = 0; k < 5; k++) sum[k] += vals[k];
    }

    printf("\n--- AVERAGE ---\n");
    printf("G10m1: %.4f  (10 tri centroids)\n", sum[0]/NT);
    printf("G10m2: %.4f  (10 tri centroids + 10 tips)\n", sum[1]/NT);
    printf("GT2  : %.4f\n",                     sum[2]/NT);
    printf("GB   : %.4f\n",                     sum[3]/NT);
    printf("SH   : %.4f\n",                     sum[4]/NT);

    printf("\n--- WINS ---\n");
    printf("G10m1: %d/%d\n", win[0], NT);
    printf("G10m2: %d/%d\n", win[1], NT);
    printf("GT2  : %d/%d\n", win[2], NT);
    printf("GB   : %d/%d\n", win[3], NT);
    printf("SH   : %d/%d\n", win[4], NT);

    return 0;
}

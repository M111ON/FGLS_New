/*
 * ses_cmp.c — Compare two session profiles
 * Usage: ses_cmp A.ses B.ses
 *
 * Compile: gcc -O2 -std=c11 -I. -Irunner -o runner/ses_cmp.exe runner/ses_cmp.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "session_profile.h"

static double hist_cosine(SessionProfile *a, SessionProfile *b) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < SES_N_BINS; i++) {
        double va = (double)a->bins[i], vb = (double)b->bins[i];
        dot += va * vb; na += va * va; nb += vb * vb;
    }
    return (na > 0 && nb > 0) ? dot / (sqrt(na) * sqrt(nb)) : 0;
}

static double hist_l2(SessionProfile *a, SessionProfile *b) {
    double sum = 0;
    for (int i = 0; i < SES_N_BINS; i++) {
        double d = (double)a->bins[i] - (double)b->bins[i];
        sum += d * d;
    }
    return sqrt(sum);
}

static double trans_frobenius(SessionProfile *a, SessionProfile *b) {
    double sum = 0;
    for (int f = 0; f < SES_N_FACES; f++)
        for (int t = 0; t < SES_N_FACES; t++) {
            double d = (double)a->trans[f][t] - (double)b->trans[f][t];
            sum += d * d;
        }
    return sqrt(sum);
}

static double trans_kl_div(SessionProfile *a, SessionProfile *b) {
    double total_kl = 0;
    int rows = 0;
    for (int f = 0; f < SES_N_FACES; f++) {
        double sa = 0, sb = 0;
        for (int t = 0; t < SES_N_FACES; t++) {
            sa += (double)a->trans[f][t];
            sb += (double)b->trans[f][t];
        }
        if (sa == 0 || sb == 0) continue;
        double kl = 0;
        for (int t = 0; t < SES_N_FACES; t++) {
            double pa = (double)a->trans[f][t] / sa;
            double pb = (double)b->trans[f][t] / sb;
            if (pa > 0 && pb > 0) kl += pa * log(pa / pb);
        }
        total_kl += kl;
        rows++;
    }
    return rows > 0 ? total_kl / rows : 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s A.ses B.ses\n", argv[0]);
        return 1;
    }
    SessionProfile a, b;
    if (ses_profile_load(argv[1], &a) != 0) {
        fprintf(stderr, "ERROR: cannot load %s\n", argv[1]); return 1;
    }
    if (ses_profile_load(argv[2], &b) != 0) {
        fprintf(stderr, "ERROR: cannot load %s\n", argv[2]); return 1;
    }

    double cos = hist_cosine(&a, &b);
    double l2 = hist_l2(&a, &b);

    printf("── Histogram ──\n");
    printf("  cosine similarity:  %f\n", cos);
    printf("  L2 distance:        %f\n", l2);

    /* Dominant face */
    int fa = 0, fb = 0;
    for (int f = 0; f < SES_N_FACES; f++) {
        double sa = 0, sb = 0;
        for (int s = 0; s < SES_N_SPOKES; s++)
            for (int l = 0; l < SES_N_SLOTS; l++) {
                sa += (double)a.bins[f * SES_N_SPOKES * SES_N_SLOTS + s * SES_N_SLOTS + l];
                sb += (double)b.bins[f * SES_N_SPOKES * SES_N_SLOTS + s * SES_N_SLOTS + l];
            }
        if (f == 0 || sa > (double)a.bins[fa * SES_N_SPOKES * SES_N_SLOTS]) fa = f;
        if (f == 0 || sb > (double)b.bins[fb * SES_N_SPOKES * SES_N_SLOTS]) fb = f;
    }
    printf("  dominant face: A=F%d B=F%d %s\n", fa, fb, fa == fb ? "(match)" : "(diff)");

    if (a.magic == SES_MAGIC && b.magic == SES_MAGIC) {
        double frob = trans_frobenius(&a, &b);
        double kl = trans_kl_div(&a, &b);

        printf("\n── Transition ──\n");
        printf("  Frobenius norm:     %f\n", frob);
        printf("  KL divergence (mean row): %f\n", kl);

        printf("\n  Matrix diff (A - B):\n");
        for (int f = 0; f < SES_N_FACES; f++) {
            printf("    F%d:", f);
            for (int t = 0; t < SES_N_FACES; t++)
                printf(" %+4d", (int)a.trans[f][t] - (int)b.trans[f][t]);
            printf("\n");
        }

        printf("\n── Summary ──\n");
        if (cos > 0.8) printf("  histogram: Similar topics (cos=%.4f)\n", cos);
        else if (cos > 0.4) printf("  histogram: Related topics (cos=%.4f)\n", cos);
        else printf("  histogram: Different topics (cos=%.4f)\n", cos);

        if (frob < 1.0) printf("  transition: Similar trajectory (Frob=%.4f)\n", frob);
        else if (frob < 5.0) printf("  transition: Somewhat different (Frob=%.4f)\n", frob);
        else printf("  transition: Very different trajectory (Frob=%.4f KL=%.4f)\n", frob, kl);
    }
    return 0;
}

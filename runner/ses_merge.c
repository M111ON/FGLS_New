/*
 * ses_merge.c — Merge 2+ session profiles into one
 * Usage: ses_merge A.ses B.ses [--out merged.ses] [--ratio A B]
 *
 * Compile: gcc -O2 -std=c11 -I. -Irunner -o runner/ses_merge.exe runner/ses_merge.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "session_profile.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s A.ses B.ses [--out merged.ses] [--ratio A B]\n", argv[0]);
        return 1;
    }

    const char *out_path = "merged.ses";
    double ratio_a = -1, ratio_b = -1;

    /* Parse args: collect .ses files, then flags */
    const char *ses_files[64];
    int n_ses = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i+1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--ratio") && i+2 < argc) {
            ratio_a = atof(argv[++i]);
            ratio_b = atof(argv[++i]);
        } else {
            if (n_ses < 64) ses_files[n_ses++] = argv[i];
        }
    }
    if (n_ses < 2) {
        fprintf(stderr, "ERROR: need at least 2 .ses files\n"); return 1;
    }

    SessionProfile profs[64];
    uint32_t total_frames[64];
    for (int i = 0; i < n_ses; i++) {
        if (ses_profile_load(ses_files[i], &profs[i]) != 0) {
            fprintf(stderr, "ERROR: cannot load %s\n", ses_files[i]); return 1;
        }
        total_frames[i] = profs[i].n_frames;
    }

    SessionProfile merged;
    ses_profile_init(&merged);

    if (n_ses == 2 && ratio_a >= 0 && ratio_b >= 0) {
        double total_weight = ratio_a + ratio_b;
        for (int b = 0; b < SES_N_BINS; b++) {
            merged.bins[b] = (uint32_t)((profs[0].bins[b] * ratio_a +
                                          profs[1].bins[b] * ratio_b) / total_weight + 0.5);
        }
        merged.n_frames = (uint32_t)((profs[0].n_frames * ratio_a +
                                       profs[1].n_frames * ratio_b) / total_weight + 0.5);
    } else {
        uint64_t sum_frames = 0;
        for (int i = 0; i < n_ses; i++) sum_frames += total_frames[i];
        if (sum_frames == 0) sum_frames = 1;
        for (int b = 0; b < SES_N_BINS; b++) {
            double w = 0;
            for (int i = 0; i < n_ses; i++)
                w += (double)profs[i].bins[b] * (double)total_frames[i] / (double)sum_frames;
            merged.bins[b] = (uint32_t)(w + 0.5);
        }
        merged.n_frames = (uint32_t)(sum_frames / n_ses);
    }

    /* Merge transition matrices */
    for (int f = 0; f < SES_N_FACES; f++)
        for (int t = 0; t < SES_N_FACES; t++) {
            uint32_t sum = 0;
            for (int i = 0; i < n_ses; i++)
                sum += profs[i].trans[f][t];
            merged.trans[f][t] = sum;
        }

    if (ses_profile_save(out_path, &merged) != 0) {
        fprintf(stderr, "ERROR: save failed\n"); return 1;
    }
    fprintf(stderr, "[merge] saved %s (%u frames, %d profiles)\n",
            out_path, merged.n_frames, n_ses);

    /* Verification */
    if (n_ses == 2 && ratio_a >= 0) {
        double total = ratio_a + ratio_b;
        fprintf(stderr, "[merge] ratio: A=%.2f (%.0f%%) B=%.2f (%.0f%%)\n",
                ratio_a, ratio_a/total*100, ratio_b, ratio_b/total*100);
    }
    return 0;
}

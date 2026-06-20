/*
 * ses_featurize.c — Convert SES2 profiles to CSV feature vectors (108 features)
 * Usage: ses_featurize <dir/*.ses> [--output features.csv] [--no-header]
 *
 * Compile: gcc -O2 -std=c11 -I. -Irunner -o runner/ses_featurize.exe runner/ses_featurize.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "session_profile.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#define MAX_FILES 4096

/* 108 features = 8(face) + 24(spoke) + 5(slot) + 64(trans) + 5(quality) + 2(meta) */
#define N_FEATURES 108

static void profile_to_features(SessionProfile *sp, double *feat) {
    int idx = 0;
    double total_bins = 0;
    for (int i = 0; i < SES_N_BINS; i++) total_bins += (double)sp->bins[i];
    if (total_bins == 0) total_bins = 1;

    /* 8 face frequencies */
    for (int f = 0; f < SES_N_FACES; f++) {
        double sum = 0;
        for (int s = 0; s < SES_N_SPOKES; s++)
            for (int l = 0; l < SES_N_SLOTS; l++)
                sum += (double)sp->bins[f * SES_N_SPOKES * SES_N_SLOTS + s * SES_N_SLOTS + l];
        feat[idx++] = sum / total_bins;
    }

    /* 24 spoke frequencies */
    for (int s = 0; s < SES_N_SPOKES; s++) {
        double sum = 0;
        for (int f = 0; f < SES_N_FACES; f++)
            for (int l = 0; l < SES_N_SLOTS; l++)
                sum += (double)sp->bins[f * SES_N_SPOKES * SES_N_SLOTS + s * SES_N_SLOTS + l];
        feat[idx++] = sum / total_bins;
    }

    /* 5 slot frequencies */
    for (int l = 0; l < SES_N_SLOTS; l++) {
        double sum = 0;
        for (int f = 0; f < SES_N_FACES; f++)
            for (int s = 0; s < SES_N_SPOKES; s++)
                sum += (double)sp->bins[f * SES_N_SPOKES * SES_N_SLOTS + s * SES_N_SLOTS + l];
        feat[idx++] = sum / total_bins;
    }

    /* 64 transition row-normalized probabilities */
    for (int f = 0; f < SES_N_FACES; f++) {
        double row_sum = 0;
        for (int t = 0; t < SES_N_FACES; t++)
            row_sum += (double)sp->trans[f][t];
        if (row_sum == 0) row_sum = 1;
        for (int t = 0; t < SES_N_FACES; t++)
            feat[idx++] = (double)sp->trans[f][t] / row_sum;
    }

    /* 5 quality metrics */
    feat[idx++] = ses_profile_face_entropy(sp);
    feat[idx++] = ses_profile_face_balance(sp);
    feat[idx++] = ses_profile_transition_entropy(sp);
    feat[idx++] = ses_profile_stability(sp);
    feat[idx++] = ses_profile_quality(sp);

    /* 2 metadata */
    feat[idx++] = (double)sp->n_frames;
    feat[idx++] = (double)sp->last_face;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dir/*.ses> [--output FILE] [--no-header]\n", argv[0]);
        return 1;
    }

    const char *out_path = "features.csv";
    int no_header = 0;

    char *file_args[MAX_FILES];
    int n_file_args = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--output") && i+1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--no-header")) no_header = 1;
        else if (n_file_args < MAX_FILES) file_args[n_file_args++] = argv[i];
    }

    char *paths[MAX_FILES];
    SessionProfile profs[MAX_FILES];
    int n = 0;

    for (int a = 0; a < n_file_args && n < MAX_FILES; a++) {
        const char *arg = file_args[a];

#ifdef _WIN32
        WIN32_FIND_DATA fd;
        char pattern[512];
        snprintf(pattern, sizeof(pattern), "%s/*.ses", arg);

        /* Check if it's a file directly */
        if (strstr(arg, ".ses")) {
            if (ses_profile_load(arg, &profs[n]) == 0) {
                paths[n] = strdup(arg);
                n++;
            }
        } else {
            HANDLE h = FindFirstFile(pattern, &fd);
            if (h == INVALID_HANDLE_VALUE) {
                /* Maybe it's a single file */
                if (ses_profile_load(arg, &profs[n]) == 0) {
                    paths[n] = strdup(arg);
                    n++;
                }
                continue;
            }
            do {
                if (n >= MAX_FILES) break;
                char full[512];
                snprintf(full, sizeof(full), "%s/%s", arg, fd.cFileName);
                if (ses_profile_load(full, &profs[n]) == 0) {
                    paths[n] = strdup(fd.cFileName);
                    n++;
                }
            } while (FindNextFile(h, &fd));
            FindClose(h);
        }
#else
        if (strstr(arg, ".ses")) {
            if (ses_profile_load(arg, &profs[n]) == 0) {
                paths[n] = strdup(arg);
                n++;
            }
        } else {
            DIR *d = opendir(arg);
            if (!d) continue;
            struct dirent *de;
            while ((de = readdir(d)) && n < MAX_FILES) {
                char *dot = strrchr(de->d_name, '.');
                if (!dot || strcmp(dot, ".ses") != 0) continue;
                char full[512];
                snprintf(full, sizeof(full), "%s/%s", arg, de->d_name);
                if (ses_profile_load(full, &profs[n]) == 0) {
                    paths[n] = strdup(de->d_name);
                    n++;
                }
            }
            closedir(d);
        }
#endif
    }

    if (n == 0) { fprintf(stderr, "No profiles loaded\n"); return 1; }
    fprintf(stderr, "[featurize] %d profiles → %s\n", n, out_path);

    FILE *f = fopen(out_path, "w");
    if (!f) { fprintf(stderr, "ERROR: cannot write %s\n", out_path); return 1; }

    const char *header_names[] = {
        "face_0","face_1","face_2","face_3","face_4","face_5","face_6","face_7",
        "spoke_0","spoke_1","spoke_2","spoke_3","spoke_4","spoke_5","spoke_6","spoke_7",
        "spoke_8","spoke_9","spoke_10","spoke_11","spoke_12","spoke_13","spoke_14","spoke_15",
        "spoke_16","spoke_17","spoke_18","spoke_19","spoke_20","spoke_21","spoke_22","spoke_23",
        "slot_0","slot_1","slot_2","slot_3","slot_4"
    };
    if (!no_header) {
        fprintf(f, "filename");
        for (int i = 0; i < 8; i++) fprintf(f, ",%s", header_names[i]);
        for (int i = 0; i < 24; i++) fprintf(f, ",%s", header_names[8+i]);
        for (int i = 0; i < 5; i++) fprintf(f, ",%s", header_names[8+24+i]);
        for (int f2 = 0; f2 < SES_N_FACES; f2++)
            for (int t = 0; t < SES_N_FACES; t++)
                fprintf(f, ",trans_%d_%d", f2, t);
        fprintf(f, ",entropy,face_balance,trans_entropy,stability,quality");
        fprintf(f, ",n_frames,last_face\n");
    }

    double feat[N_FEATURES];
    for (int i = 0; i < n; i++) {
        profile_to_features(&profs[i], feat);

        /* Extract basename */
        const char *base = paths[i];
        const char *slash = strrchr(base, '/');
        if (!slash) slash = strrchr(base, '\\');
        if (slash) base = slash + 1;
        fprintf(f, "%s", base);

        for (int j = 0; j < N_FEATURES; j++)
            fprintf(f, ",%.6f", feat[j]);
        fprintf(f, "\n");
    }

    fclose(f);
    fprintf(stderr, "[featurize] done (%d rows, %d columns)\n", n, N_FEATURES + 1);
    for (int i = 0; i < n; i++) free(paths[i]);
    return 0;
}

/*
 * ses_cluster.c — Unsupervised HAC clustering of session profiles
 * Usage: ses_cluster <dir> [--min-similarity F] [--min-clusters N] [--max-clusters N]
 *
 * Compile: gcc -O2 -std=c11 -I. -Irunner -o runner/ses_cluster.exe runner/ses_cluster.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "session_profile.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#define MAX_PROFILES 512

static double hist_cosine(SessionProfile *a, SessionProfile *b) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < SES_N_BINS; i++) {
        double va = (double)a->bins[i], vb = (double)b->bins[i];
        dot += va * vb; na += va * va; nb += vb * vb;
    }
    return (na > 0 && nb > 0) ? dot / (sqrt(na) * sqrt(nb)) : 0;
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

static double combined_dist(SessionProfile *a, SessionProfile *b) {
    double cos = hist_cosine(a, b);
    double frob = trans_frobenius(a, b);
    double frob_norm = frob / 10.0;
    if (frob_norm > 1.0) frob_norm = 1.0;
    return 0.5 * (1.0 - cos) + 0.5 * frob_norm;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dir> [--min-similarity F] [--min-clusters N] [--max-clusters N]\n", argv[0]);
        return 1;
    }
    const char *dir = argv[1];
    double min_sim = 0.0;
    int min_k = 2, max_k = 10;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--min-similarity") && i+1 < argc) min_sim = atof(argv[++i]);
        else if (!strcmp(argv[i], "--min-clusters") && i+1 < argc) min_k = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-clusters") && i+1 < argc) max_k = atoi(argv[++i]);
    }

    char *paths[MAX_PROFILES];
    SessionProfile profs[MAX_PROFILES];
    int n = 0;

#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s/*.ses", dir);
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: no .ses files in %s\n", dir); return 1;
    }
    do {
        if (n >= MAX_PROFILES) break;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, fd.cFileName);
        if (ses_profile_load(full, &profs[n]) == 0) {
            paths[n] = strdup(fd.cFileName);
            n++;
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "ERROR: cannot open %s\n", dir); return 1; }
    struct dirent *de;
    while ((de = readdir(d)) && n < MAX_PROFILES) {
        char *dot = strrchr(de->d_name, '.');
        if (!dot || strcmp(dot, ".ses") != 0) continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        if (ses_profile_load(full, &profs[n]) == 0) {
            paths[n] = strdup(de->d_name);
            n++;
        }
    }
    closedir(d);
#endif

    if (n == 0) { fprintf(stderr, "No profiles loaded\n"); return 1; }
    fprintf(stderr, "[cluster] loaded %d profiles\n", n);

    /* Distance matrix */
    double *dist = (double*)calloc((size_t)n * n, sizeof(double));
    for (int i = 0; i < n; i++) {
        dist[i * n + i] = 0;
        for (int j = i + 1; j < n; j++) {
            double d = combined_dist(&profs[i], &profs[j]);
            dist[i * n + j] = d;
            dist[j * n + i] = d;
        }
    }

    /* Print distance matrix (first 10) */
    printf("── Distance Matrix (first %d) ──\n", n < 10 ? n : 10);
    for (int i = 0; i < n && i < 10; i++) {
        printf("  %s:", paths[i]);
        for (int j = 0; j < n && j < 10; j++)
            printf(" %.3f", dist[i * n + j]);
        printf("\n");
    }

    /* Simple HAC: average linkage */
    int *cluster = (int*)malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) cluster[i] = i;

    int n_clusters = n;
    int *merge_a = NULL, *merge_b = NULL;
    int n_merges = 0;
    int max_merges = n - min_k;
    if (max_merges > 0) {
        merge_a = (int*)calloc((size_t)max_merges, sizeof(int));
        merge_b = (int*)calloc((size_t)max_merges, sizeof(int));
    }

    while (n_clusters > min_k) {
        double best = DBL_MAX;
        int bi = -1, bj = -1;
        for (int i = 0; i < n; i++) {
            if (cluster[i] < 0) continue;
            for (int j = i + 1; j < n; j++) {
                if (cluster[j] < 0) continue;
                double d = dist[i * n + j];
                if (d < best) { best = d; bi = i; bj = j; }
            }
        }
        if (bi < 0 || best > 1.0) break;

        if (n_merges < max_merges) {
            merge_a[n_merges] = bi;
            merge_b[n_merges] = bj;
            n_merges++;
        }

        int new_id = bi;
        int merged_id = cluster[bj];
        for (int i = 0; i < n; i++)
            if (cluster[i] == merged_id) cluster[i] = new_id;
        cluster[bj] = -1;
        n_clusters--;
    }

    /* Assign final cluster IDs 0..K-1 */
    int k = 0;
    int *cluster_id = (int*)malloc((size_t)n * sizeof(int));
    int *cluster_map = (int*)calloc((size_t)n, sizeof(int));
    for (int i = 0; i < n; i++) {
        if (cluster[i] < 0) continue;
        int found = 0;
        for (int c = 0; c < k; c++)
            if (cluster_map[c] == cluster[i]) { cluster_id[i] = c; found = 1; break; }
        if (!found) { cluster_map[k] = cluster[i]; cluster_id[i] = k; k++; }
    }
    for (int i = 0; i < n; i++)
        if (cluster[i] < 0) {
            for (int j = 0; j < n; j++)
                if (cluster[j] >= 0 && cluster[i] == -1) { }
            /* Find owner by nearest cluster center */
            double best_d = DBL_MAX;
            int best_c = 0;
            for (int c = 0; c < k; c++) {
                double d = dist[i * n + cluster_map[c]];
                if (d < best_d) { best_d = d; best_c = c; }
            }
            cluster_id[i] = best_c;
        }

    printf("\n── Clusters (K=%d) ──\n", k);
    for (int c = 0; c < k; c++) {
        printf("  C%d:", c);
        for (int i = 0; i < n; i++)
            if (cluster_id[i] == c) printf(" %s", paths[i]);
        printf("\n");
    }

    /* Intra/inter cluster distances */
    double *intra = (double*)calloc((size_t)k, sizeof(double));
    int *intra_n = (int*)calloc((size_t)k, sizeof(int));
    double inter_sum = 0;
    int inter_n = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            if (cluster_id[i] == cluster_id[j]) {
                intra[cluster_id[i]] += dist[i * n + j];
                intra_n[cluster_id[i]]++;
            } else {
                inter_sum += dist[i * n + j];
                inter_n++;
            }
        }

    printf("\n── Intra/Inter Cluster Distances ──\n");
    for (int c = 0; c < k; c++) {
        double avg = intra_n[c] > 0 ? intra[c] / intra_n[c] : 0;
        printf("  C%d: intra avg=%.4f (%d pairs)\n", c, avg, intra_n[c]);
    }
    printf("  inter avg=%.4f (%d pairs)\n", inter_n > 0 ? inter_sum / inter_n : 0, inter_n);

    /* Score: ratio of intra/inter */
    double avg_intra = 0;
    for (int c = 0; c < k; c++) avg_intra += intra[c];
    if (intra_n[0] + intra_n[1] > 0) avg_intra /= (double)(intra_n[0] + intra_n[1]);
    double avg_inter = inter_n > 0 ? inter_sum / inter_n : 1;
    double ratio = avg_inter > 0 ? avg_intra / avg_inter : 1;
    printf("\n  Ratio (avg_intra/avg_inter): %.4f%s\n", ratio, ratio < 1.0 ? " (good separation)" : " (poor separation)");

    free(dist); free(cluster); free(cluster_id); free(cluster_map);
    free(intra); free(intra_n); free(merge_a); free(merge_b);
    for (int i = 0; i < n; i++) free(paths[i]);
    return 0;
}

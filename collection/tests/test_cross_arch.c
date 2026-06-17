/* test_cross_arch.c — Cross-architecture SID comparison: SmolLM2 vs SmolVLM
 * ═══════════════════════════════════════════════════════════════
 * Build:
 *   gcc -DGEO_JUMP_INLINE -DTEST_WITH_TENSORS -I. -I../geo_jump_module/include
 *       -I../src -I../../src -o tests/test_cross_arch.exe
 *       tests/test_cross_arch.c -lm
 * Run:
 *   test_cross_arch.exe [smollm2_dir] [smolvlm_dir]
 *
 * Note: This is the node_id-migrated version.
 * See test_cross_arch_v2.c for the original reference.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#define SID_IMPLEMENTATION
#include "sid.h"

/* Per-tensor analysis struct (node_id based) */
typedef struct {
    char name[128];
    int   layer_idx;
    uint8_t layer_type; /* 0-11: ATTN_Q, ATTN_K, ... */
    uint32_t node_id;   /* 0..20735 */
    int64_t  resid_x;
    int64_t  resid_y;
} TensorAnalysis;

/* Layer type enum */
static const char *layer_names[] = {
    "UNKNOWN", "ATTN_Q", "ATTN_K", "ATTN_V", "ATTN_OUT",
    "FFN_GATE", "FFN_UP", "FFN_DOWN", "NORM", "EMBED", "ROPE", "OTHER"
};

static int classify_layer(const char *name) {
    if (!name) return 0;
    if (strstr(name, "attn_q"))    return 1;
    if (strstr(name, "attn_k"))    return 2;
    if (strstr(name, "attn_v"))    return 3;
    if (strstr(name, "attn_output") || strstr(name, "attn_o")) return 4;
    if (strstr(name, "ffn_gate") || strstr(name, "mlp.gate"))  return 5;
    if (strstr(name, "ffn_up")   || strstr(name, "mlp.up"))    return 6;
    if (strstr(name, "ffn_down") || strstr(name, "mlp.down"))  return 7;
    if (strstr(name, "norm")  || strstr(name, "ln")) return 8;
    if (strstr(name, "embd")  || strstr(name, "embed")) return 9;
    if (strstr(name, "head"))  return 10;
    if (strstr(name, "rope")  || strstr(name, "rotary")) return 10;
    return 11;
}

static int extract_layer_idx(const char *name) {
    if (!name) return -1;
    const char *p = name;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            int n = 0;
            while (*p >= '0' && *p <= '9') { n = n*10 + (*p-'0'); p++; }
            return n;
        }
        p++;
    }
    return -1;
}

/* Compare two models' node_id distributions */
typedef struct {
    int count;
    int pent_hist[12];    /* pentagon 1..12 */
    double node_mean, node_std;
    double pent_entropy;  /* Shannon entropy of pentagon distribution */
} ModelStats;

static void compute_model_stats(TensorAnalysis *tensors, int n,
                                 ModelStats *s) {
    memset(s, 0, sizeof(*s));
    s->count = n;
    for (int i = 0; i < n; i++) {
        uint32_t pent = geo_pentagon_id(tensors[i].node_id);
        if (pent >= 1 && pent <= 12) s->pent_hist[pent-1]++;
        s->node_mean += tensors[i].node_id;
    }
    s->node_mean /= n;
    double var = 0;
    for (int i = 0; i < n; i++) {
        double d = (double)tensors[i].node_id - s->node_mean;
        var += d*d;
    }
    s->node_std = sqrt(var / n);

    /* Pentagon entropy */
    s->pent_entropy = 0;
    for (int p = 0; p < 12; p++) {
        if (s->pent_hist[p] > 0) {
            double pr = (double)s->pent_hist[p] / n;
            s->pent_entropy -= pr * log2(pr);
        }
    }
}

/* ── Layer-type stats ── */
typedef struct {
    int count;
    int pent_hist[12];
    double node_mean, node_std;
} TypeStats;

static void print_type_stats(TensorAnalysis *tensors, int n) {
    TypeStats stats[12];
    memset(stats, 0, sizeof(stats));

    for (int i = 0; i < n; i++) {
        int t = tensors[i].layer_type;
        if (t < 0 || t >= 12) t = 0;
        stats[t].count++;
        uint32_t pent = geo_pentagon_id(tensors[i].node_id);
        if (pent >= 1 && pent <= 12) stats[t].pent_hist[pent-1]++;
        stats[t].node_mean += tensors[i].node_id;
    }
    for (int t = 0; t < 12; t++) {
        if (stats[t].count == 0) continue;
        stats[t].node_mean /= stats[t].count;
        double var = 0;
        for (int i = 0; i < n; i++) {
            if (tensors[i].layer_type == t) {
                double d = (double)tensors[i].node_id - stats[t].node_mean;
                var += d*d;
            }
        }
        stats[t].node_std = sqrt(var / stats[t].count);
    }

    printf("\n  Per-layer-type stats:\n");
    printf("  %-12s %5s %8s %8s   PENT_DIST\n", "TYPE", "COUNT",
           "N_μ", "N_σ");
    for (int t = 0; t < 12; t++) {
        if (stats[t].count == 0) continue;
        char pd[64] = {0};
        int pos = 0;
        for (int p = 0; p < 12 && pos < 60; p++) {
            if (stats[t].pent_hist[p] > 0)
                pos += snprintf(pd+pos, sizeof(pd)-pos, "p%d:%d ",
                                p+1, stats[t].pent_hist[p]);
        }
        printf("  %-12s %5d %8.0f %8.0f   %s\n",
               layer_names[t], stats[t].count,
               stats[t].node_mean, stats[t].node_std, pd);
    }
}

/* ── Capture all tensors in a directory ── */
static TensorAnalysis *capture_all(const char *tensors_dir, int *out_n) {
    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    if (rb_load(&rb, tensors_dir) != 0) {
        printf("ERROR: cannot load %s\n", tensors_dir);
        *out_n = 0;
        return NULL;
    }

    printf("Loaded %u tensors from %s\n", rb.n_entries, tensors_dir);

    int max_cap = rb.n_entries;
    TensorAnalysis *results = calloc(max_cap, sizeof(TensorAnalysis));
    if (!results) { *out_n = 0; return NULL; }

    int n_captured = 0;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES && n_captured < max_cap; i++) {
        if (!rb.entries[i].occupied) continue;
        if (rb.entries[i].size < 34) continue;

        const char *name = rb.entries[i].name;
        SIDCoord coord;
        if (sid_capture(rb.entries[i].data, rb.entries[i].size,
                        rb.entries[i].dtype, &coord) != 0) continue;

        TensorAnalysis *ta = &results[n_captured];
        strncpy(ta->name, name ? name : "unknown", sizeof(ta->name)-1);
        ta->name[sizeof(ta->name)-1] = '\0';
        ta->layer_idx = extract_layer_idx(name);
        ta->layer_type = classify_layer(name);
        ta->node_id  = coord.node_id;
        ta->resid_x  = coord.resid_x;
        ta->resid_y  = coord.resid_y;

        n_captured++;
        if (n_captured % 100 == 0)
            printf("  Captured %d/%u...\n", n_captured, rb.n_entries);
    }

    printf("  Total captured: %d\n", n_captured);
    rb_free(&rb);
    *out_n = n_captured;
    return results;
}

/* ── Compare two models ── */
static void compare_models(TensorAnalysis *lm2, int n_lm2,
                            TensorAnalysis *vlm, int n_vlm) {
    ModelStats s_lm2, s_vlm;
    compute_model_stats(lm2, n_lm2, &s_lm2);
    compute_model_stats(vlm, n_vlm, &s_vlm);

    printf("\n══════════════════════════════════════════════════════\n");
    printf("  SmolLM2 vs SmolVLM — SID Node Distribution\n");
    printf("══════════════════════════════════════════════════════\n");

    printf("\n  ┌──────────────────────────┬───────────┬───────────┐\n");
    printf("  │ Metric                   │ SmolLM2   │ SmolVLM   │\n");
    printf("  ├──────────────────────────┼───────────┼───────────┤\n");
    printf("  │ Tensors                  │ %9d │ %9d │\n", n_lm2, n_vlm);
    printf("  │ Node μ                   │ %9.0f │ %9.0f │\n",
           s_lm2.node_mean, s_vlm.node_mean);
    printf("  │ Node σ                   │ %9.0f │ %9.0f │\n",
           s_lm2.node_std, s_vlm.node_std);

    /* Unique pentagons */
    int uniq_lm2 = 0, uniq_vlm = 0;
    for (int i = 0; i < 12; i++) {
        if (s_lm2.pent_hist[i] > 0) uniq_lm2++;
        if (s_vlm.pent_hist[i] > 0) uniq_vlm++;
    }
    printf("  │ Pentagons used          │ %9d │ %9d │\n", uniq_lm2, uniq_vlm);
    printf("  │ Pentagon entropy        │ %9.3f │ %9.3f │\n",
           s_lm2.pent_entropy, s_vlm.pent_entropy);
    printf("  └──────────────────────────┴───────────┴───────────┘\n");

    /* Pentagon distribution comparison */
    printf("\n  Pentagon distribution:\n");
    printf("  %5s %10s %10s\n", "Pent", "SmolLM2", "SmolVLM");
    for (int p = 0; p < 12; p++) {
        printf("  %5d %10d %10d\n", p+1, s_lm2.pent_hist[p], s_vlm.pent_hist[p]);
    }

    /* Layer-type comparison */
    printf("\n  Per-layer-type comparison:\n");
    printf("  %-12s %8s %8s %10s %10s\n", "TYPE",
           "LM2_n", "VLM_n", "LM2_N_μ", "VLM_N_μ");
    for (int t = 0; t < 12; t++) {
        int n1 = 0, n2 = 0;
        double m1 = 0, m2 = 0;
        for (int i = 0; i < n_lm2; i++) {
            if (lm2[i].layer_type == t) { n1++; m1 += lm2[i].node_id; }
        }
        for (int i = 0; i < n_vlm; i++) {
            if (vlm[i].layer_type == t) { n2++; m2 += vlm[i].node_id; }
        }
        if (n1 == 0 && n2 == 0) continue;
        if (n1 > 0) m1 /= n1;
        if (n2 > 0) m2 /= n2;
        printf("  %-12s %8d %8d %10.0f %10.0f\n",
               layer_names[t], n1, n2, m1, m2);
    }
}

int main(int argc, char **argv) {
    const char *smollm2_dir = "../../build/smollm2_tensors_raw";
    const char *smolvlm_dir = "../../build/smolvlm_tensors_raw";
    if (argc > 1) smollm2_dir = argv[1];
    if (argc > 2) smolvlm_dir = argv[2];

    /* Capture SmolLM2 */
    printf("\n═══ SmolLM2-360M ═══\n");
    int n_lm2;
    TensorAnalysis *lm2 = capture_all(smollm2_dir, &n_lm2);
    if (!lm2) return 1;
    print_type_stats(lm2, n_lm2);

    /* Capture SmolVLM */
    printf("\n═══ SmolVLM-256M ═══\n");
    int n_vlm;
    TensorAnalysis *vlm = capture_all(smolvlm_dir, &n_vlm);
    if (!vlm) { free(lm2); return 1; }
    print_type_stats(vlm, n_vlm);

    /* Compare */
    compare_models(lm2, n_lm2, vlm, n_vlm);

    printf("\nDone.\n");

    free(lm2);
    free(vlm);
    return 0;
}

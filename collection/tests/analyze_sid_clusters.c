/* analyze_sid_clusters.c — SID coordinate analysis by layer type (node_id)
 * Build:
 *   gcc -DGEO_JUMP_INLINE -DTEST_WITH_TENSORS -I. -I../geo_jump_module/include
 *       -I../src -I../../src -o tests/analyze_sid_clusters.exe
 *       tests/analyze_sid_clusters.c -lm
 * Run:   analyze_sid_clusters.exe <tensors_dir>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#define SID_IMPLEMENTATION
#include "sid.h"

/* ── Layer type classifier ── */
typedef enum {
    LAYER_UNKNOWN = 0,
    LAYER_ATTN_Q,
    LAYER_ATTN_K,
    LAYER_ATTN_V,
    LAYER_ATTN_OUT,
    LAYER_FFN_GATE,
    LAYER_FFN_UP,
    LAYER_FFN_DOWN,
    LAYER_NORM,
    LAYER_EMBED,
    LAYER_ROPE,
    LAYER_OTHER,
} LayerType;

static const char *layer_names[] = {
    "UNKNOWN", "ATTN_Q", "ATTN_K", "ATTN_V", "ATTN_OUT",
    "FFN_GATE", "FFN_UP", "FFN_DOWN", "NORM", "EMBED", "ROPE", "OTHER"
};

static LayerType classify_layer(const char *name) {
    if (!name) return LAYER_UNKNOWN;
    if (strstr(name, "attn_q") || strstr(name, "attn.q"))  return LAYER_ATTN_Q;
    if (strstr(name, "attn_k") || strstr(name, "attn.k"))  return LAYER_ATTN_K;
    if (strstr(name, "attn_v") || strstr(name, "attn.v"))  return LAYER_ATTN_V;
    if (strstr(name, "attn_output") || strstr(name, "attn.o")) return LAYER_ATTN_OUT;
    if (strstr(name, "ffn_gate") || strstr(name, "gate"))   return LAYER_FFN_GATE;
    if (strstr(name, "ffn_up") || strstr(name, "ffn.u"))    return LAYER_FFN_UP;
    if (strstr(name, "ffn_down") || strstr(name, "ffn.d"))  return LAYER_FFN_DOWN;
    if (strstr(name, "norm") || strstr(name, "ln"))         return LAYER_NORM;
    if (strstr(name, "token_embd") || strstr(name, "wte"))  return LAYER_EMBED;
    if (strstr(name, "rope"))                               return LAYER_ROPE;
    return LAYER_OTHER;
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

int main(int argc, char **argv) {
    const char *tensors_dir = "../build/smollm2_tensors_raw";
    if (argc > 1) tensors_dir = argv[1];

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  SID Cluster Analysis — Node positions by layer type\n");
    printf("═══════════════════════════════════════════════════════════\n");

#ifndef TEST_WITH_TENSORS
    printf("SKIP: compile with -DTEST_WITH_TENSORS\n");
    return 0;
#else
    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    if (rb_load(&rb, tensors_dir) != 0) {
        printf("ERROR: cannot load %s\n", tensors_dir);
        return 1;
    }
    printf("Loaded %u tensors\n\n", rb.n_entries);

    /* Per-layer-type analysis */
    typedef struct {
        int         count;
        double      node_mean;
        double      node_std;
        double      resid_x_mean, resid_x_std;
        double      resid_y_mean, resid_y_std;
        double      pent_mean;
        int         pent_dist[12]; /* pentagon 0..11 distribution */
    } LayerStats;

    LayerStats stats[12];
    memset(stats, 0, sizeof(stats));

    /* Per-layer-type scatter data */
    struct Scatter {
        int    layer_idx;
        uint32_t node_id;
        uint8_t  pentagon; /* 1..12 */
        int64_t  resid_mag;
    };
    #define MAX_PER_TYPE 200
    struct Scatter scatter[12][MAX_PER_TYPE];
    int n_scatter[12] = {0};

    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (!rb.entries[i].occupied) continue;

        const char *name = rb.entries[i].name;
        LayerType lt = classify_layer(name);
        int lt_idx = (int)lt;

        SIDCoord coord;
        if (sid_capture(rb.entries[i].data, rb.entries[i].size,
                        rb.entries[i].dtype, &coord) != 0)
            continue;

        uint8_t pent = (uint8_t)geo_pentagon_id(coord.node_id);

        stats[lt_idx].count++;
        stats[lt_idx].node_mean += coord.node_id;
        stats[lt_idx].resid_x_mean += (double)coord.resid_x;
        stats[lt_idx].resid_y_mean += (double)coord.resid_y;
        stats[lt_idx].pent_mean += (double)pent;
        if (pent >= 1 && pent <= 12) stats[lt_idx].pent_dist[pent-1]++;

        if (n_scatter[lt_idx] < MAX_PER_TYPE) {
            scatter[lt_idx][n_scatter[lt_idx]].layer_idx = extract_layer_idx(name);
            scatter[lt_idx][n_scatter[lt_idx]].node_id = coord.node_id;
            scatter[lt_idx][n_scatter[lt_idx]].pentagon = pent;
            scatter[lt_idx][n_scatter[lt_idx]].resid_mag =
                (int64_t)sqrt((double)(coord.resid_x*coord.resid_x + coord.resid_y*coord.resid_y));
            n_scatter[lt_idx]++;
        }
    }

    /* Compute final stats */
    for (int t = 0; t < 12; t++) {
        if (stats[t].count == 0) continue;
        int n = stats[t].count;
        stats[t].node_mean /= n;
        stats[t].resid_x_mean /= n;
        stats[t].resid_y_mean /= n;
        stats[t].pent_mean /= n;

        /* Second pass for std dev */
        for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
            if (!rb.entries[i].occupied) continue;
            if (classify_layer(rb.entries[i].name) != t) continue;

            SIDCoord coord;
            if (sid_capture(rb.entries[i].data, rb.entries[i].size,
                            rb.entries[i].dtype, &coord) != 0) continue;

            double dn = (double)coord.node_id - stats[t].node_mean;
            double dx = coord.resid_x - stats[t].resid_x_mean;
            double dy = coord.resid_y - stats[t].resid_y_mean;
            stats[t].node_std += dn*dn;
            stats[t].resid_x_std += dx*dx;
            stats[t].resid_y_std += dy*dy;
        }
        stats[t].node_std   = sqrt(stats[t].node_std / n);
        stats[t].resid_x_std = sqrt(stats[t].resid_x_std / n);
        stats[t].resid_y_std = sqrt(stats[t].resid_y_std / n);
    }

    /* ── [C1] Print per-layer-type stats ── */
    printf("[C1] Per-layer-type node statistics:\n");
    printf("%-18s %5s  %10s %8s %8s %10s %8s %8s   %s\n",
           "TYPE", "COUNT",
           "N_mean", "N_std", "P_mean",
           "Rx_mean", "Rx_std", "Ry_mean", "PENT_DIST");
    printf("────────────────────────────────────────────────────────────────────────────────────────\n");

    int total_tensors = 0;
    for (int t = 0; t < 12; t++) {
        if (stats[t].count == 0) continue;
        total_tensors += stats[t].count;

        char pd[32] = {0};
        int pd_pos = 0;
        for (int p = 0; p < 12 && pd_pos < 28; p++) {
            pd_pos += snprintf(pd + pd_pos, sizeof(pd)-pd_pos, "%d", stats[t].pent_dist[p]);
        }

        printf("%-18s %5d  %10.0f %8.0f %8.0f  %10.0f %8.0f %8.0f  %s\n",
               layer_names[t], stats[t].count,
               stats[t].node_mean, stats[t].node_std, stats[t].pent_mean,
               stats[t].resid_x_mean, stats[t].resid_x_std,
               stats[t].resid_y_mean, pd);
    }
    printf("\n  Total: %d tensors\n\n", total_tensors);

    /* ── [C2] Pentagon distribution by layer type ── */
    printf("[C2] Pentagon distribution (how many layers per pentagon):\n");
    printf("%-18s", "TYPE");
    for (int p = 0; p < 12; p++) printf(" p%d", p+1);
    printf("\n");
    printf("──────────────────────────────────────────────\n");

    for (int t = 0; t < 12; t++) {
        if (stats[t].count == 0) continue;
        printf("%-18s", layer_names[t]);
        for (int p = 0; p < 12; p++)
            printf(" %2d", stats[t].pent_dist[p]);
        printf("\n");
    }

    /* ── [C3] Predictability ── */
    printf("\n[C3] Node position vs layer index scatter (key layers):\n");
    for (int t = 0; t < 12; t++) {
        if (t == LAYER_ATTN_Q || t == LAYER_FFN_GATE ||
            t == LAYER_NORM || t == LAYER_FFN_DOWN) {
            printf("\n  %s (type %d):\n", layer_names[t], t);
            printf("  %6s %8s %8s %8s\n", "idx", "node", "pent", "|resid|");
            for (int s = 0; s < n_scatter[t]; s++) {
                printf("  %6d %8u %8d %8lld\n",
                       scatter[t][s].layer_idx,
                       scatter[t][s].node_id,
                       scatter[t][s].pentagon,
                       (long long)scatter[t][s].resid_mag);
            }
        }
    }

    /* ── Conclusion ── */
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  Analysis complete — %d tensors, %d types\n", total_tensors, 12);
    printf("═══════════════════════════════════════════════════════════\n");

    rb_free(&rb);
    return 0;
#endif
}

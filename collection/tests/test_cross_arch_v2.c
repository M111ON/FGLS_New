/* test_cross_arch_v2.c — Cross-arch SID comparison using sid.h API
 * Build:
 *   cd collection/tests
 *   gcc -DGEO_JUMP_INLINE -DTEST_WITH_TENSORS -I.. -I../src -I../geo_jump_module/include
 *       -I../../src -o test_cross_arch_v2.exe test_cross_arch_v2.c -lm
 * Run:  test_cross_arch_v2.exe [smollm2_dir] [smolvlm_dir]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Use SID API for proper capture+summon */
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#include "geom_raw_bridge.h"

/* tw_face_bridge gives us 12-face iteration */
#define TW_FACE_BRIDGE_IMPLEMENTATION
#include "tw_face_bridge.h"

/* sid.h gives us capture via sid_capture */
#define SID_IMPLEMENTATION
#include "sid.h"

#define GEO_JUMP_INLINE
#include "geo_jump.h"

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
    if (strstr(name, "ffn_gate")   || strstr(name, "mlp.gate")) return 5;
    if (strstr(name, "ffn_up")     || strstr(name, "mlp.up"))   return 6;
    if (strstr(name, "ffn_down")   || strstr(name, "mlp.down")) return 7;
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

/* Per-tensor analysis */
typedef struct {
    char name[128];
    int  layer_idx;
    uint8_t layer_type;
    SIDCoord coord;     /* SID coordinate from sid_capture (face 0) */
    int64_t vx, vy;     /* 2D signature values */
} TensorInfo;

/* Capture using SID API */
static TensorInfo *capture_using_sid(const char *tensors_dir, int *out_n) {
    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    if (rb_load(&rb, tensors_dir) != 0) {
        printf("ERROR: cannot load %s\n", tensors_dir);
        *out_n = 0;
        return NULL;
    }
    
    printf("Loaded %u tensors from %s\n", rb.n_entries, tensors_dir);
    
    TensorInfo *results = calloc(rb.n_entries, sizeof(TensorInfo));
    if (!results) { *out_n = 0; return NULL; }
    
    int n = 0;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (!rb.entries[i].occupied) continue;
        if (rb.entries[i].size < 34) continue;
        
        const char *name = rb.entries[i].name;
        uint8_t *data = (uint8_t *)rb.entries[i].data;
        size_t sz = rb.entries[i].size;
        int dtype = rb.entries[i].dtype;
        
        /* Capture using SID API */
        SIDCoord coord;
        if (sid_capture(data, sz, dtype, &coord) != 0) continue;
        
        /* Also get 2D signature vx, vy by dequanting first 64 values */
        int n_blocks = sz / 34;
        int n_vals = n_blocks * 32;
        if (n_vals > 64) n_vals = 64;
        if (n_vals < 32) continue;
        
        float f32_vals[64];
        for (int b = 0; b < n_blocks && b*32 < 64; b++) {
            uint16_t scale_bits = *(uint16_t*)(data + b*34);
            float dscale;
            {   int s = (scale_bits >> 15) & 1;
                int e = (scale_bits >> 10) & 0x1F;
                int m = scale_bits & 0x3FF;
                if (e == 0) {
                    dscale = m * 5.960464477539063e-8f;
                    if (s) dscale = -dscale;
                } else {
                    uint32_t fi = (s << 31) | ((e + 112) << 23) | (m << 13);
                    dscale = *(float*)&fi;
                }
            }
            for (int j = 0; j < 32 && b*32+j < 64; j++) {
                int8_t q = *(int8_t*)(data + b*34 + 2 + j);
                f32_vals[b*32 + j] = q * dscale;
            }
        }
        
        double sum_a = 0, sum_b = 0;
        int half = n_vals / 2;
        for (int j = 0; j < half; j++) sum_a += f32_vals[j];
        for (int j = half; j < n_vals; j++) sum_b += f32_vals[j];
        
        int64_t vx = (int64_t)((sum_a / half) * TW_SCALE);
        int64_t vy = (int64_t)((sum_b / half) * TW_SCALE);
        
        TensorInfo *ti = &results[n];
        strncpy(ti->name, name ? name : "unknown", sizeof(ti->name)-1);
        ti->name[sizeof(ti->name)-1] = '\0';
        ti->layer_idx = extract_layer_idx(name);
        ti->layer_type = classify_layer(name);
        ti->coord = coord;
        ti->vx = vx;
        ti->vy = vy;
        n++;
        
        if (n % 100 == 0) printf("  Captured %d/%u...\n", n, rb.n_entries);
    }
    
    printf("  Total captured: %d\n", n);
    rb_free(&rb);
    *out_n = n;
    return results;
}

/* ── Layer-type stats for face-0 capture ── */
static void print_face0_stats(TensorInfo *tensors, int n) {
    typedef struct { int count; int tring_sum; int tring_ss; } T;
    T stats[12]; memset(stats, 0, sizeof(stats));
    
    for (int i = 0; i < n; i++) {
        int t = tensors[i].layer_type;
        if (t < 0 || t >= 12) continue;
        stats[t].count++;
        stats[t].tring_sum += tensors[i].coord.node_id;
        stats[t].tring_ss  += tensors[i].coord.node_id * tensors[i].coord.node_id;
    }
    
    printf("\n  Face-0 TRing by layer type:\n");
    printf("  %-12s %5s %8s %8s %8s %8s %8s\n",
           "TYPE", "COUNT", "TR_μ", "TR_σ", "Z_μ", "ZENTROPY", "Z_SIG");
    for (int t = 0; t < 12; t++) {
        if (stats[t].count == 0) continue;
        double mu = (double)stats[t].tring_sum / stats[t].count;
        double var = (double)stats[t].tring_ss / stats[t].count - mu*mu;
        if (var < 0) var = 0;
        /* Pentagon entropy */
        int zhist[12] = {0};
        double zmu = 0;
        for (int i = 0; i < n; i++) {
            if (tensors[i].layer_type == t) {
                int z = (int)geo_pentagon_id(tensors[i].coord.node_id) - 1;
                if (z >= 0 && z < 12) zhist[z]++;
                zmu += z;
            }
        }
        zmu /= stats[t].count;
        double zent = 0;
        for (int z = 0; z < 12; z++) {
            if (zhist[z] > 0) { double p = (double)zhist[z]/stats[t].count; zent -= p * log2(p); }
        }
        char zsig[32] = {0}; int p = 0;
        for (int z = 0; z < 12 && p < 30; z++) {
            if (zhist[z] > 0) p += snprintf(zsig+p, sizeof(zsig)-p, "p%d:%d ", z+1, zhist[z]);
        }
        printf("  %-12s %5d %8.0f %8.0f %8.0f %8.3f %s\n",
               layer_names[t], stats[t].count, mu, sqrt(var), zmu, zent, zsig);
    }
}

/* ── Layer-type stats for 12-face iteration ── */
static void print_12face_stats(const char *tensors_dir, const char *label) {
    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    if (rb_load(&rb, tensors_dir) != 0) { printf("  SKIP: %s\n", tensors_dir); return; }
    
    printf("\n  %s — 12-Face Full TRing 720 Analysis:\n", label);
    
    /* For each type, collect TRing positions from BEST face (min resid) */
    typedef struct { int count; int pent_hist[12]; } THist;
    THist stats[12]; memset(stats, 0, sizeof(stats));
    
    int n_proc = 0;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (!rb.entries[i].occupied) continue;
        if (rb.entries[i].size < 34) continue;
        
        const char *name = rb.entries[i].name;
        int lt = classify_layer(name);
        if (lt < 0 || lt >= 12) lt = 0;
        
        uint8_t *data = (uint8_t *)rb.entries[i].data;
        size_t sz = rb.entries[i].size;
        int dtype = rb.entries[i].dtype;
        
        SIDCoord coord;
        if (sid_capture(data, sz, dtype, &coord) != 0) continue;
        /* Get vx, vy from dequant */
        int n_blocks = sz / 34;
        int n_vals = n_blocks * 32;
        if (n_vals > 64) n_vals = 64;
        if (n_vals < 32) continue;
        
        float f32_vals[64];
        for (int b = 0; b < n_blocks && b*32 < 64; b++) {
            uint16_t scale_bits = *(uint16_t*)(data + b*34);
            float dscale;
            {   int s = (scale_bits >> 15) & 1;
                int e = (scale_bits >> 10) & 0x1F;
                int m = scale_bits & 0x3FF;
                if (e == 0) { dscale = m * 5.960464477539063e-8f; if (s) dscale = -dscale; }
                else { uint32_t fi = (s << 31) | ((e + 112) << 23) | (m << 13); dscale = *(float*)&fi; }
            }
            for (int j = 0; j < 32 && b*32+j < 64; j++) {
                int8_t q = *(int8_t*)(data + b*34 + 2 + j);
                f32_vals[b*32 + j] = q * dscale;
            }
        }
        double sum_a = 0, sum_b = 0;
        int half = n_vals / 2;
        for (int j = 0; j < half; j++) sum_a += f32_vals[j];
        for (int j = half; j < n_vals; j++) sum_b += f32_vals[j];
        int64_t vx = (int64_t)((sum_a / half) * TW_SCALE);
        int64_t vy = (int64_t)((sum_b / half) * TW_SCALE);
        
        /* Run capo ×12 capture */
        uint32_t capo_nodes[12];
        tw_capture_capo_all(vx, vy, capo_nodes);
        
        /* Pick first node as primary */
        uint32_t primary_node = capo_nodes[0];
        uint8_t pent = (uint8_t)geo_pentagon_id(primary_node);
        if (pent >= 1 && pent <= 12) stats[lt].pent_hist[pent-1]++;
        stats[lt].count++;
        n_proc++;
        if (n_proc % 100 == 0) printf("  Processed %d...\n", n_proc);
    }
    
    printf("  Processed: %d tensors\n", n_proc);
    
    /* Print per-type stats */
    printf("  %-12s %5s %8s\n", "TYPE", "COUNT", "UNIQ");
    for (int t = 0; t < 12; t++) {
        if (stats[t].count == 0) continue;
        int uniq = 0;
        for (int i = 0; i < 12; i++) {
            if (stats[t].pent_hist[i] > 0) uniq++;
        }
        printf("  %-12s %5d %5d/12\n",
               layer_names[t], stats[t].count, uniq);
    }
    
    rb_free(&rb);
}

/* ── Compare ── */
static void compare_face0(TensorInfo *lm2, int n1, TensorInfo *vlm, int n2) {
    printf("\n══════════════════════════════════════════════════════\n");
    printf("  SID Cross-Arch Comparison — Face-0 (no rotation)\n");
    printf("══════════════════════════════════════════════════════\n");
    
    /* Compute per-type mean TRing for both models */
    typedef struct { int c1, c2; double m1, m2; double s1, s2; } CT;
    CT comp[12]; memset(comp, 0, sizeof(comp));
    
    for (int i = 0; i < n1; i++) {
        int t = lm2[i].layer_type; if (t<0||t>=12) continue;
        comp[t].c1++; comp[t].m1 += lm2[i].coord.node_id;
        comp[t].s1 += lm2[i].coord.node_id * lm2[i].coord.node_id;
    }
    for (int i = 0; i < n2; i++) {
        int t = vlm[i].layer_type; if (t<0||t>=12) continue;
        comp[t].c2++; comp[t].m2 += vlm[i].coord.node_id;
        comp[t].s2 += vlm[i].coord.node_id * vlm[i].coord.node_id;
    }
    
    printf("  %-12s %8s %8s %8s %8s %8s\n",
           "TYPE", "LM2_n", "VLM_n", "LM2_TRμ", "VLM_TRμ", "|Δ|");
    double total_abs_diff = 0;
    int n_comp = 0;
    for (int t = 0; t < 12; t++) {
        if (comp[t].c1 == 0 && comp[t].c2 == 0) continue;
        if (comp[t].c1 > 0) comp[t].m1 /= comp[t].c1;
        if (comp[t].c2 > 0) comp[t].m2 /= comp[t].c2;
        double diff = fabs(comp[t].m1 - comp[t].m2);
        total_abs_diff += diff;
        n_comp++;
        printf("  %-12s %8d %8d %8.0f %8.0f %8.0f\n",
               layer_names[t], comp[t].c1, comp[t].c2,
               comp[t].m1, comp[t].m2, diff);
    }
    printf("  ─────────────────────────────────────────────────────\n");
    printf("  Mean |Δ| per type: %.1f  (lower = more architecture-stable)\n",
           total_abs_diff / n_comp);
    
    /* Pentagon distribution comparison */
    int z1[12] = {0}, z2[12] = {0};
    for (int i = 0; i < n1; i++) {
        int p = (int)geo_pentagon_id(lm2[i].coord.node_id) - 1;
        if (p >= 0 && p < 12) z1[p]++;
    }
    for (int i = 0; i < n2; i++) {
        int p = (int)geo_pentagon_id(vlm[i].coord.node_id) - 1;
        if (p >= 0 && p < 12) z2[p]++;
    }
    printf("\n  Pentagon distribution:\n  %5s %10s %10s\n", "Pent", "SmolLM2", "SmolVLM");
    for (int z = 0; z < 12; z++)
        printf("  %5d %10d %10d\n", z+1, z1[z], z2[z]);
    
    /* Key: how many pentagons overlap between the two models? */
    int overlap = 0;
    for (int z = 0; z < 12; z++)
        if (z1[z] > 0 && z2[z] > 0) overlap++;
    printf("  Zone overlap: %d/10\n", overlap);
}

int main(int argc, char **argv) {
    const char *lm2_dir = "../../build/smollm2_tensors_raw";
    const char *vlm_dir = "../../build/smolvlm_tensors_raw";
    if (argc > 1) lm2_dir = argv[1];
    if (argc > 2) vlm_dir = argv[2];
    
    /* Face-0 capture using SID API */
    printf("\n═══ SmolLM2-360M (SID face-0) ═══\n");
    int n_lm2;
    TensorInfo *lm2 = capture_using_sid(lm2_dir, &n_lm2);
    if (!lm2) return 1;
    print_face0_stats(lm2, n_lm2);
    
    printf("\n═══ SmolVLM-256M (SID face-0) ═══\n");
    int n_vlm;
    TensorInfo *vlm = capture_using_sid(vlm_dir, &n_vlm);
    if (!vlm) { free(lm2); return 1; }
    print_face0_stats(vlm, n_vlm);
    
    compare_face0(lm2, n_lm2, vlm, n_vlm);
    
    /* 12-face iteration analysis (slower, only on LM text layers) */
    printf("\n═══ 12-Face Best-Face Analysis ═══\n");
    print_12face_stats(lm2_dir, "SmolLM2-360M");
    print_12face_stats(vlm_dir, "SmolVLM-256M");
    
    free(lm2);
    free(vlm);
    printf("\nDone.\n");
    return 0;
}

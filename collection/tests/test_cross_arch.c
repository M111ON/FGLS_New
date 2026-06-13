/* test_cross_arch.c — Cross-architecture SID comparison: SmolLM2 vs SmolVLM
 * Build:
 *   gcc -DGEO_JUMP_INLINE -DTEST_WITH_TENSORS -I. -I../geo_jump_module/include
 *       -I../src -o tests/test_cross_arch.exe
 *       tests/test_cross_arch.c -lm
 * Run:
 *   test_cross_arch.exe [smollm2_dir] [smolvlm_dir]
 *   (defaults: build/smollm2_tensors_raw build/smolvlm_tensors_raw)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TW_FACE_BRIDGE_IMPLEMENTATION
#include "tw_face_bridge.h"

#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#include "geom_raw_bridge.h"

/* Per-tensor analysis struct */
typedef struct {
    char name[128];
    int   layer_idx;
    uint8_t layer_type; /* 0-11: ATTN_Q, ATTN_K, ... */
    uint16_t tring;     /* 0-719 */
    uint8_t  zone;      /* 0-9 */
    uint8_t  slot;      /* 0-5 */
    uint8_t  face;      /* 0-11 */
    uint8_t  frozen;    /* frozen flag */
    int64_t  resid_x, resid_y;
    uint8_t  captured_face;   /* which face captured this tensor (0-11) */
} TensorAnalysis;

/* Layer type enum (matching classify_layer in tw_tensor_capture.h) */
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
    if (strstr(name, "norm")  || strstr(name, "ln"))       return 8;
    if (strstr(name, "embd")  || strstr(name, "embed"))    return 9;
    if (strstr(name, "head"))                              return 10;
    return 11; /* OTHER */
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

/* Compare two models' TRing distributions */
typedef struct {
    int count;
    int tring_hist[720];
    int zone_hist[10];
    int face_hist[12];
    double tring_mean, tring_std;
    double zone_entropy; /* Shannon entropy of zone distribution */
} ModelStats;

static void compute_model_stats(TensorAnalysis *tensors, int n,
                                 ModelStats *s) {
    memset(s, 0, sizeof(*s));
    s->count = n;
    for (int i = 0; i < n; i++) {
        if (tensors[i].tring < 720) s->tring_hist[tensors[i].tring]++;
        if (tensors[i].zone < 10)   s->zone_hist[tensors[i].zone]++;
        if (tensors[i].face < 12)   s->face_hist[tensors[i].face]++;
        s->tring_mean += tensors[i].tring;
    }
    s->tring_mean /= n;
    double var = 0;
    for (int i = 0; i < n; i++) {
        double d = tensors[i].tring - s->tring_mean;
        var += d*d;
    }
    s->tring_std = sqrt(var / n);
    
    /* Zone entropy */
    s->zone_entropy = 0;
    for (int z = 0; z < 10; z++) {
        if (s->zone_hist[z] > 0) {
            double p = (double)s->zone_hist[z] / n;
            s->zone_entropy -= p * log2(p);
        }
    }
}

/* ── Layer-type stats ── */
typedef struct {
    int count;
    int tring_hist[720];
    int zone_hist[10];
    double tring_mean, tring_std;
} TypeStats;

static void print_type_stats(TensorAnalysis *tensors, int n) {
    TypeStats stats[12];
    memset(stats, 0, sizeof(stats));
    
    for (int i = 0; i < n; i++) {
        int t = tensors[i].layer_type;
        if (t < 0 || t >= 12) t = 0;
        stats[t].count++;
        if (tensors[i].tring < 720) stats[t].tring_hist[tensors[i].tring]++;
        if (tensors[i].zone < 10)   stats[t].zone_hist[tensors[i].zone]++;
        stats[t].tring_mean += tensors[i].tring;
    }
    for (int t = 0; t < 12; t++) {
        if (stats[t].count == 0) continue;
        stats[t].tring_mean /= stats[t].count;
        double var = 0;
        for (int i = 0; i < n; i++) {
            if (tensors[i].layer_type == t) {
                double d = tensors[i].tring - stats[t].tring_mean;
                var += d*d;
            }
        }
        stats[t].tring_std = sqrt(var / stats[t].count);
    }
    
    printf("\n  Per-layer-type TRing stats:\n");
    printf("  %-12s %5s %8s %8s %8s   ZONE_DIST\n", "TYPE", "COUNT",
           "TR_μ", "TR_σ", "Z_μ");
    for (int t = 0; t < 12; t++) {
        if (stats[t].count == 0) continue;
        double z_mean = 0;
        for (int z = 0; z < 10; z++)
            z_mean += z * stats[t].zone_hist[z];
        z_mean /= stats[t].count;
        
        char zd[64] = {0};
        int pos = 0;
        for (int z = 0; z < 10 && pos < 60; z++) {
            if (stats[t].zone_hist[z] > 0)
                pos += snprintf(zd+pos, sizeof(zd)-pos, "z%d:%d ",
                                z, stats[t].zone_hist[z]);
        }
        printf("  %-12s %5d %8.0f %8.0f %8.0f   %s\n",
               layer_names[t], stats[t].count,
               stats[t].tring_mean, stats[t].tring_std, z_mean, zd);
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
        
        const char *name = rb.entries[i].name;
        if (rb.entries[i].size < 34) continue; /* Need at least 1 Q8_0 block */
        
        /* Use first 32 values of tensor data as 2D signature via TW dequant */
        /* We'll use tw_tensor_capture.h's approach: read Q8_0 block */
        uint8_t *data = (uint8_t *)rb.entries[i].data;
        size_t nbytes = rb.entries[i].size;
        
        /* Dequant first 2 Q8_0 blocks into f32 buffer */
        float f32_vals[64];
        int n_blocks = nbytes / 34;
        int n_f32 = n_blocks * 32;
        if (n_f32 > 64) n_f32 = 64;
        
        for (int b = 0; b < n_blocks && b*32 < 64; b++) {
            uint16_t scale_bits = *(uint16_t*)(data + b*34);
            /* f16 to float */
            float dscale;
            {   int sign = (scale_bits >> 15) & 1;
                int exp  = (scale_bits >> 10) & 0x1F;
                int mant = scale_bits & 0x3FF;
                if (exp == 0) {
                    dscale = mant * 5.960464477539063e-8f;
                    if (sign) dscale = -dscale;
                } else {
                    uint32_t fi = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
                    dscale = *(float*)&fi;
                }
            }
            for (int j = 0; j < 32 && b*32+j < 64; j++) {
                int8_t q = *(int8_t*)(data + b*34 + 2 + j);
                f32_vals[b*32 + j] = q * dscale;
            }
        }
        
        /* Compute 2D signature */
        double sum_a = 0, sum_b = 0;
        int na = 0, nb = 0;
        int half = n_f32 / 2;
        for (int j = 0; j < half; j++) { sum_a += f32_vals[j]; na++; }
        for (int j = half; j < n_f32; j++) { sum_b += f32_vals[j]; nb++; }
        
        if (na == 0 || nb == 0) continue;
        
        int64_t vx = (int64_t)((sum_a / na) * TW_SCALE);
        int64_t vy = (int64_t)((sum_b / nb) * TW_SCALE);
        
        /* Run 12-face bridge */
        TWFaceIterResult iter;
        tw_iterate_faces(vx, vy, &iter);
        
        /* Pick face 0 for primary capture */
        int primary_face = 0;
        TWFaceCapture *cap = &iter.faces[primary_face];
        
        TensorAnalysis *ta = &results[n_captured];
        strncpy(ta->name, name ? name : "unknown", sizeof(ta->name)-1);
        ta->name[sizeof(ta->name)-1] = '\0';
        ta->layer_idx = extract_layer_idx(name);
        ta->layer_type = classify_layer(name);
        ta->tring = cap->face * 60 + cap->zone * 6 + cap->slot;
        ta->zone = cap->zone;
        ta->slot = cap->slot;
        ta->face = cap->face;
        ta->frozen = tw_face_is_frozen(cap, 12);
        ta->resid_x = cap->resid_x;
        ta->resid_y = cap->resid_y;
        ta->captured_face = primary_face;
        
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
    printf("  SmolLM2 vs SmolVLM — SID TRing Distribution\n");
    printf("══════════════════════════════════════════════════════\n");
    
    printf("\n  ┌─────────────────────┬───────────┬───────────┐\n");
    printf("  │ Metric              │ SmolLM2   │ SmolVLM   │\n");
    printf("  ├─────────────────────┼───────────┼───────────┤\n");
    printf("  │ Tensors             │ %9d │ %9d │\n", n_lm2, n_vlm);
    printf("  │ TRing μ             │ %9.0f │ %9.0f │\n",
           s_lm2.tring_mean, s_vlm.tring_mean);
    printf("  │ TRing σ             │ %9.0f │ %9.0f │\n",
           s_lm2.tring_std, s_vlm.tring_std);
    
    /* Unique TRing positions */
    int uniq_lm2 = 0, uniq_vlm = 0;
    for (int i = 0; i < 720; i++) {
        if (s_lm2.tring_hist[i] > 0) uniq_lm2++;
        if (s_vlm.tring_hist[i] > 0) uniq_vlm++;
    }
    printf("  │ TRing slots used    │ %9d │ %9d │\n", uniq_lm2, uniq_vlm);
    printf("  │ Coverage (of 720)   │ %9.1f%% │ %9.1f%% │\n",
           uniq_lm2*100.0/720, uniq_vlm*100.0/720);
    printf("  │ Zone entropy        │ %9.3f │ %9.3f │\n",
           s_lm2.zone_entropy, s_vlm.zone_entropy);
    printf("  └─────────────────────┴───────────┴───────────┘\n");
    
    /* Zone distribution comparison */
    printf("\n  Zone distribution:\n");
    printf("  %5s %10s %10s\n", "Zone", "SmolLM2", "SmolVLM");
    for (int z = 0; z < 10; z++) {
        printf("  %5d %10d %10d\n", z, s_lm2.zone_hist[z], s_vlm.zone_hist[z]);
    }
    
    /* Face distribution */
    printf("\n  Face distribution:\n");
    printf("  %5s %10s %10s\n", "Face", "SmolLM2", "SmolVLM");
    for (int f = 0; f < 12; f++) {
        printf("  %5d %10d %10d\n", f, s_lm2.face_hist[f], s_vlm.face_hist[f]);
    }
    
    /* Layer-type comparison */
    printf("\n  Per-layer-type comparison:\n");
    printf("  %-12s %8s %8s %8s %8s\n", "TYPE",
           "LM2_n", "VLM_n", "LM2_TRμ", "VLM_TRμ");
    for (int t = 0; t < 12; t++) {
        int n1 = 0, n2 = 0;
        double m1 = 0, m2 = 0;
        for (int i = 0; i < n_lm2; i++) {
            if (lm2[i].layer_type == t) { n1++; m1 += lm2[i].tring; }
        }
        for (int i = 0; i < n_vlm; i++) {
            if (vlm[i].layer_type == t) { n2++; m2 += vlm[i].tring; }
        }
        if (n1 == 0 && n2 == 0) continue;
        if (n1 > 0) m1 /= n1;
        if (n2 > 0) m2 /= n2;
        printf("  %-12s %8d %8d %8.0f %8.0f\n",
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
    
    /* Check: do same layer types map to same zones? */
    printf("\n═══ Zone Predictability Check ═══\n");
    printf("  Are NORM weights always zone 7 across both models?\n");
    for (int i = 0; i < n_lm2; i++) {
        if (lm2[i].layer_type == 8 && lm2[i].zone != 7)
            printf("    SmolLM2 NORM in zone %d (not 7!): %s\n",
                   lm2[i].zone, lm2[i].name);
    }
    for (int i = 0; i < n_vlm; i++) {
        if (vlm[i].layer_type == 8 && vlm[i].zone != 7)
            printf("    SmolVLM NORM in zone %d (not 7!): %s\n",
                   vlm[i].zone, vlm[i].name);
    }
    
    printf("  Did all NORM land in zone 7? YES (unless printed above)\n");
    
    /* Vision encoder analysis */
    printf("\n═══ SmolVLM Vision Encoder ═══\n");
    int n_vision = 0;
    for (int i = 0; i < n_vlm; i++) {
        if (strstr(vlm[i].name, "vision"))
            n_vision++;
    }
    printf("  Vision tensors: %d\n", n_vision);
    
    printf("\nDone.\n");
    
    free(lm2);
    free(vlm);
    return 0;
}

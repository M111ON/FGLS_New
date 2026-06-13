/* test_tring_predictor.c — Build SID TRing predictor from cross-arch data
 * 
 * Given SmolLM2 data, predict SmolVLM TRing from layer_type alone.
 * Tests accuracy of the prediction.
 *
 * Build:
 *   gcc -DGEO_JUMP_INLINE -DTEST_WITH_TENSORS -I.. -I../src -I../geo_jump_module/include
 *       -I../../src -o tests/test_tring_predictor.exe tests/test_tring_predictor.c -lm
 * Run:  test_tring_predictor.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#include "geom_raw_bridge.h"
#define SID_IMPLEMENTATION
#include "sid.h"

/* ── Layer types ── */
#define LT_ATTN_Q   1
#define LT_ATTN_K   2
#define LT_ATTN_V   3
#define LT_ATTN_OUT 4
#define LT_FFN_GATE 5
#define LT_FFN_UP   6
#define LT_FFN_DOWN 7
#define LT_NORM     8

static int classify(const char *name) {
    if (!name) return 0;
    if (strstr(name, "attn_q")) return 1;
    if (strstr(name, "attn_k")) return 2;
    if (strstr(name, "attn_v")) return 3;
    if (strstr(name, "attn_output") || strstr(name, "attn_o")) return 4;
    if (strstr(name, "ffn_gate")   || strstr(name, "mlp.gate")) return 5;
    if (strstr(name, "ffn_up")     || strstr(name, "mlp.up"))   return 6;
    if (strstr(name, "ffn_down")   || strstr(name, "mlp.down")) return 7;
    if (strstr(name, "norm"))      return 8;
    return 0;
}

static const char *type_name(int t) {
    static const char *names[] = {"?", "ATTN_Q","ATTN_K","ATTN_V","ATTN_OUT",
                                   "FFN_GATE","FFN_UP","FFN_DOWN","NORM"};
    return (t>=1&&t<=8) ? names[t] : "?";
}

/* Capture data from a directory */
typedef struct {
    int     layer_type;
    int     layer_idx;
    uint16_t tring;
    uint8_t  zone;
    char    name[128];
} TRec;

static TRec *capture_dir(const char *dir, int *out_n, int max_n) {
    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    if (rb_load(&rb, dir) != 0) { *out_n=0; return NULL; }
    
    int cap = (max_n > 0 && max_n < (int)rb.n_entries) ? max_n : (int)rb.n_entries;
    if (cap == 0) { *out_n = 0; rb_free(&rb); return NULL; }
    TRec *recs = calloc(cap, sizeof(TRec));
    if (!recs) { *out_n=0; return NULL; }
    
    int n=0;
    for (uint32_t i=0; i<RB_MAX_ENTRIES && n<rb.n_entries; i++) {
        if (!rb.entries[i].occupied) continue;
        if (rb.entries[i].size < 34) continue;
        
        const char *nm = rb.entries[i].name;
        int lt = classify(nm);
        if (lt == 0) continue; /* Only known types */
        
        SIDCoord c;
        if (sid_capture(rb.entries[i].data, rb.entries[i].size,
                        rb.entries[i].dtype, 0, &c) != 0) continue;
        
        recs[n].layer_type = lt;
        recs[n].tring = c.tring_pos;
        recs[n].zone  = c.zone;
        strncpy(recs[n].name, nm ? nm : "?", sizeof(recs[n].name)-1);
        
        /* Extract layer index from name */
        const char *p = nm;
        int li = -1;
        while (*p) {
            if (*p >= '0' && *p <= '9') {
                li = 0;
                while (*p >= '0' && *p <= '9') { li = li*10 + (*p-'0'); p++; }
                break;
            }
            p++;
        }
        recs[n].layer_idx = li;
        n++;
        
        if (max_n > 0 && n >= max_n) break;
    }
    
    rb_free(&rb);
    *out_n = n;
    return recs;
}

/* Per-type stats */
typedef struct {
    int count;
    double tring_sum;
    double tring_ss;
    int tring_hist[60]; /* face-0 range 0-59 */
    int zone_hist[10];
} TypeStats;

static void compute_stats(TRec *recs, int n, TypeStats *stats) {
    memset(stats, 0, sizeof(*stats) * 12);
    for (int i=0; i<n; i++) {
        int t = recs[i].layer_type;
        if (t < 1 || t > 8) continue;
        stats[t].count++;
        stats[t].tring_sum += recs[i].tring;
        stats[t].tring_ss  += recs[i].tring * recs[i].tring;
        if (recs[i].tring < 60) stats[t].tring_hist[recs[i].tring]++;
        if (recs[i].zone < 10)   stats[t].zone_hist[recs[i].zone]++;
    }
}

int main(int argc, char **argv) {
    const char *lm2_dir = "../../build/smollm2_tensors_raw";
    const char *vlm_dir = "../../build/smolvlm_tensors_raw";
    if (argc > 1) lm2_dir = argv[1];
    if (argc > 2) vlm_dir = argv[2];
    
    /* Capture both */
    printf("Capturing SmolLM2...\n");
    int n1;
    TRec *lm2 = capture_dir(lm2_dir, &n1, 0);
    if (!lm2) return 1;
    printf("  %d LM tensors captured\n", n1);
    
    printf("Capturing SmolVLM (LM only)...\n");
    int n2;
    TRec *vlm = capture_dir(vlm_dir, &n2, 0);
    if (!vlm) { free(lm2); return 1; }
    printf("  %d VLM tensors captured\n", n2);
    
    /* Compute per-type stats */
    TypeStats s1[12], s2[12];
    compute_stats(lm2, n1, s1);
    compute_stats(vlm, n2, s2);
    
    /* ── BUILd PREDICTOR from SmolLM2 data ── */
    printf("\n══════════════════════════════════════════════════════\n");
    printf("  SID TRing PREDICTOR — Training (SmolLM2) → Prediction (SmolVLM)\n");
    printf("══════════════════════════════════════════════════════\n");
    
    /* Predictor: for each type, use SmolLM2 mean TRing as prediction */
    double pred[12] = {0}; /* prediction for SmolVLM */
    double pred_err[12] = {0}; /* absolute error */
    double slot_pred[60] = {0}; /* slot distribution prediction */
    int slot_obs[60] = {0}; /* observed count */
    
    printf("\n  %-12s %8s %8s %8s %8s %8s %8s\n",
           "TYPE", "LM2_TRμ", "VLM_TRμ", "PRED", "|ERR|", "LM2_σ", "PASS?");
    int n_pass = 0, n_fail = 0;
    double total_err = 0;
    
    for (int t = 1; t <= 8; t++) {
        if (s1[t].count == 0 && s2[t].count == 0) continue;
        double mu1 = s1[t].tring_sum / s1[t].count;
        double mu2 = s2[t].tring_sum / s2[t].count;
        double var = s1[t].tring_ss / s1[t].count - mu1*mu1;
        if (var < 0) var = 0;
        double sigma = sqrt(var);
        
        pred[t] = mu1; /* SmolLM2 mean = predictor */
        double err = fabs(mu2 - mu1);
        pred_err[t] = err;
        total_err += err;
        
        /* PASS if error < 1 sigma */
        int pass = (err < sigma) || (err < 5);
        if (pass) n_pass++; else n_fail++;
        
        printf("  %-12s %8.0f %8.0f %8.0f %8.0f %8.0f %s\n",
               type_name(t), mu1, mu2, pred[t], err, sigma,
               pass ? "PASS" : "FAIL");
        
        /* Slot distribution */
        for (int s = 0; s < 60; s++) {
            if (s1[t].tring_hist[s] > 0) {
                double p = (double)s1[t].tring_hist[s] / s1[t].count;
                slot_pred[s] += p / 8; /* average over 8 types */
            }
        }
    }
    
    printf("  ─────────────────────────────────────────────────────\n");
    printf("  Pass: %d/8  Fail: %d/8  Mean |Δ|: %.1f\n",
           n_pass, n_fail, total_err / 8);
    
    /* ── Layer-index analysis: is there a trend? ── */
    printf("\n  Layer-index trend (per type):\n");
    for (int t = 1; t <= 8; t++) {
        /* Check if early layers differ from late layers */
        int early_layers = 0, late_layers = 0;
        double early_sum = 0, late_sum = 0;
        int n_layers = 0;
        int max_idx = 0;
        
        for (int i = 0; i < n1; i++) {
            if (lm2[i].layer_type == t) {
                n_layers++;
                if (lm2[i].layer_idx > max_idx) max_idx = lm2[i].layer_idx;
            }
        }
        
        if (n_layers < 2) continue;
        
        for (int i = 0; i < n1; i++) {
            if (lm2[i].layer_type != t) continue;
            if (lm2[i].layer_idx < max_idx / 3) {
                early_layers++;
                early_sum += lm2[i].tring;
            } else if (lm2[i].layer_idx > max_idx * 2 / 3) {
                late_layers++;
                late_sum += lm2[i].tring;
            }
        }
        
        if (early_layers > 0 && late_layers > 0) {
            double early_mu = early_sum / early_layers;
            double late_mu = late_sum / late_layers;
            double drift = late_mu - early_mu;
            if (fabs(drift) > 3) {
                printf("  %-12s early TRμ=%.0f (n=%d) late TRμ=%.0f (n=%d) drift=%.0f\n",
                       type_name(t), early_mu, early_layers, late_mu, late_layers, drift);
            }
        }
    }
    
    /* ── PREDICTION: Apply predictor to SmolVLM ── */
    printf("\n  Prediction accuracy on SmolVLM:\n");
    printf("  %-12s %8s %8s %8s %8s %8s\n",
           "TYPE", "ACTUAL", "PREDICT", "|ERR|", "TOL", "VERDICT");
    double total_abs = 0;
    int total_pred = 0;
    for (int t = 1; t <= 8; t++) {
        if (s2[t].count == 0) continue;
        double actual = s2[t].tring_sum / s2[t].count;
        double err = fabs(actual - pred[t]);
        double sigma = sqrt(s1[t].tring_ss / s1[t].count 
                          - (s1[t].tring_sum/s1[t].count)*(s1[t].tring_sum/s1[t].count));
        double tol = sigma < 5 ? 5 : sigma;
        total_abs += err;
        total_pred++;
        printf("  %-12s %8.0f %8.0f %8.0f %8.0f %s\n",
               type_name(t), actual, pred[t], err, tol,
               err <= tol ? "PASS ✓" : "FAIL ✗");
    }
    printf("  ─────────────────────────────────────────────────────\n");
    printf("  Mean |Δ|: %.1f  (%d types)\n", total_abs / total_pred, total_pred);
    
    /* ── Zone-level predictor ── */
    printf("\n  Zone-level prediction (does each type map to same zone?):\n");
    printf("  %-12s %15s %15s %8s\n", "TYPE", "LM2_zone_dist", "VLM_zone_dist", "MATCH");
    for (int t = 1; t <= 8; t++) {
        if (s1[t].count == 0 || s2[t].count == 0) continue;
        char z1[64]={0}, z2[64]={0};
        int p1=0, p2=0;
        int overlap = 0;
        for (int z = 0; z < 10; z++) {
            if (s1[t].zone_hist[z] > 0)
                p1 += snprintf(z1+p1, sizeof(z1)-p1, "z%d:%d ", z, s1[t].zone_hist[z]);
            if (s2[t].zone_hist[z] > 0)
                p2 += snprintf(z2+p2, sizeof(z2)-p2, "z%d:%d ", z, s2[t].zone_hist[z]);
            if (s1[t].zone_hist[z] > 0 && s2[t].zone_hist[z] > 0) overlap++;
        }
        printf("  %-12s %15s %15s %s\n", type_name(t), z1, z2,
               overlap >= 4 ? "GOOD" : overlap >= 2 ? "FAIR" : "POOR");
    }
    
    free(lm2);
    free(vlm);
    printf("\nDone.\n");
    return 0;
}

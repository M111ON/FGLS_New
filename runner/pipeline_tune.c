/* ============================================================
 * pipeline_tune.c — Tuned FreeCentroid variants
 *
 * Problem: linear grid [0..31] doesn't match weight distribution
 *   Weights are centered around 0, scattered — not a ramp
 *
 * Tuning approaches:
 *   A. Sort-first: sort weights, then grid fits sorted order
 *   B. Centered grid: grid centered at mean, spacing = std
 *   C. Quantile grid: grid at actual weight percentiles
 *   D. Hybrid: FreeCentroid structure + Vert24 centroids
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define Q8_BLOCK_SZ 32

static float q8_dequant(int8_t q, uint16_t sc16) {
    int sign = (sc16 >> 15) & 1;
    int exp = (sc16 >> 10) & 0x1f;
    int mantissa = sc16 & 0x3ff;
    float scale;
    if (exp == 0) scale = (sign ? -1 : 1) * ldexp(mantissa, -24);
    else if (exp == 31) scale = (sign ? -1 : 1) * INFINITY;
    else scale = (sign ? -1 : 1) * ldexp(1.0 + mantissa / 1024.0, exp - 15);
    return q * scale;
}

static void sort_floats(float *arr, int n) {
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (arr[i] > arr[j]) { float t = arr[i]; arr[i] = arr[j]; arr[j] = t; }
}

typedef struct {
    int total_bytes;
    float avg_error_pct;
    int delta_bits;
} TuneResult;

/* ============================================================
 * Variant A: Sort-First Grid
 * Sort weights, then use linear grid on sorted order
 * ============================================================ */
static TuneResult tune_sort_first(float *weights, int n, int delta_bits) {
    TuneResult r = {0};
    r.delta_bits = delta_bits;

    float sorted[Q8_BLOCK_SZ];
    for (int i = 0; i < n; i++) sorted[i] = weights[i];
    sort_floats(sorted, n);

    float range = sorted[n-1] - sorted[0];
    if (range < 1e-10f) range = 1.0f;

    /* Map each weight to its sorted position (0..n-1) */
    float max_delta = 0;
    int max_val = (1 << delta_bits) - 1;

    for (int i = 0; i < n; i++) {
        /* Find position of weights[i] in sorted array */
        int pos = 0;
        for (int j = 0; j < n; j++) {
            if (weights[i] <= sorted[j]) { pos = j; break; }
            if (j == n-1) pos = j;
        }
        float ideal = sorted[pos]; /* exact sorted value */
        float d = fabsf(weights[i] - ideal);
        if (d > max_delta) max_delta = d;
    }
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    float sum_error = 0;
    for (int i = 0; i < n; i++) {
        int pos = 0;
        for (int j = 0; j < n; j++) {
            if (weights[i] <= sorted[j]) { pos = j; break; }
            if (j == n-1) pos = j;
        }
        float ideal = sorted[pos];
        float delta = weights[i] - ideal;
        int q = (int)roundf(delta / delta_scale * max_val);
        if (q < -max_val) q = -max_val;
        if (q > max_val) q = max_val;
        float dq = (float)q * delta_scale / max_val;
        float recon = ideal + dq;
        float err = fabsf(weights[i] - recon);
        sum_error += err;
    }

    r.avg_error_pct = 100.0f * (sum_error / n) / range;

    /* Storage: R (fp16) + θ (fp16) = 4B + max_delta (fp16) = 2B + deltas */
    r.total_bytes = 4 + 2 + (n * delta_bits + 7) / 8;
    return r;
}

/* ============================================================
 * Variant B: Centered Grid
 * Grid centered at mean, spacing = std
 * positions: mean + k × std, k in [-(n/2), +(n/2)]
 * ============================================================ */
static TuneResult tune_centered_grid(float *weights, int n, int delta_bits) {
    TuneResult r = {0};
    r.delta_bits = delta_bits;

    float range = weights[0], max_w = weights[0];
    for (int i = 1; i < n; i++) {
        if (weights[i] < range) range = weights[i];
        if (weights[i] > max_w) max_w = weights[i];
    }
    float w_range = max_w - range;
    if (w_range < 1e-10f) w_range = 1.0f;

    /* Compute mean and std */
    float sum = 0;
    for (int i = 0; i < n; i++) sum += weights[i];
    float mean = sum / n;

    float sum_sq = 0;
    for (int i = 0; i < n; i++) {
        float d = weights[i] - mean;
        sum_sq += d * d;
    }
    float std = sqrtf(sum_sq / n);
    if (std < 1e-10f) std = w_range / n;

    /* Grid positions: mean + k × std */
    int max_val = (1 << delta_bits) - 1;
    float max_delta = 0;

    for (int i = 0; i < n; i++) {
        /* Find closest grid point */
        float k = roundf((weights[i] - mean) / std);
        float ideal = mean + k * std;
        float d = fabsf(weights[i] - ideal);
        if (d > max_delta) max_delta = d;
    }
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    float sum_error = 0;
    for (int i = 0; i < n; i++) {
        float k = roundf((weights[i] - mean) / std);
        float ideal = mean + k * std;
        float delta = weights[i] - ideal;
        int q = (int)roundf(delta / delta_scale * max_val);
        if (q < -max_val) q = -max_val;
        if (q > max_val) q = max_val;
        float dq = (float)q * delta_scale / max_val;
        float recon = ideal + dq;
        float err = fabsf(weights[i] - recon);
        sum_error += err;
    }

    r.avg_error_pct = 100.0f * (sum_error / n) / w_range;
    r.total_bytes = 4 + 2 + (n * delta_bits + 7) / 8; /* R+θ + max_delta + deltas */
    return r;
}

/* ============================================================
 * Variant C: Quantile Grid
 * Grid at actual weight percentiles (sorted values)
 * Each weight → nearest percentile position
 * ============================================================ */
static TuneResult tune_quantile_grid(float *weights, int n, int delta_bits) {
    TuneResult r = {0};
    r.delta_bits = delta_bits;

    float sorted[Q8_BLOCK_SZ];
    for (int i = 0; i < n; i++) sorted[i] = weights[i];
    sort_floats(sorted, n);

    float range = sorted[n-1] - sorted[0];
    if (range < 1e-10f) range = 1.0f;

    /* Grid = sorted values themselves (perfect quantile grid)
     * This is essentially the same as Vert24 with N=32 centroids
     * But we store the grid as R + θ (fp16) instead of individual centroids
     */
    int max_val = (1 << delta_bits) - 1;
    float max_delta = 0;

    for (int i = 0; i < n; i++) {
        /* Binary search for closest sorted value */
        int lo = 0, hi = n - 1;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (sorted[mid] < weights[i]) lo = mid + 1;
            else hi = mid;
        }
        float ideal = sorted[lo];
        float d = fabsf(weights[i] - ideal);
        if (d > max_delta) max_delta = d;
    }
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    float sum_error = 0;
    for (int i = 0; i < n; i++) {
        int lo = 0, hi = n - 1;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (sorted[mid] < weights[i]) lo = mid + 1;
            else hi = mid;
        }
        float ideal = sorted[lo];
        float delta = weights[i] - ideal;
        int q = (int)roundf(delta / delta_scale * max_val);
        if (q < -max_val) q = -max_val;
        if (q > max_val) q = max_val;
        float dq = (float)q * delta_scale / max_val;
        float recon = ideal + dq;
        float err = fabsf(weights[i] - recon);
        sum_error += err;
    }

    r.avg_error_pct = 100.0f * (sum_error / n) / range;
    /* Storage: R + θ (4B) + max_delta (2B) + sorted grid (n × fp16) + deltas */
    r.total_bytes = 4 + 2 + n * 2 + (n * delta_bits + 7) / 8;
    return r;
}

/* ============================================================
 * Variant D: Hybrid — FreeCentroid + Centroids
 * Use Vert24-style centroids but store as R + θ structure
 * + per-centroid delta refinement
 * ============================================================ */
static TuneResult tune_hybrid(float *weights, int n, int n_centroids, int delta_bits) {
    TuneResult r = {0};
    r.delta_bits = delta_bits;

    float sorted[Q8_BLOCK_SZ];
    for (int i = 0; i < n; i++) sorted[i] = weights[i];
    sort_floats(sorted, n);

    float range = sorted[n-1] - sorted[0];
    if (range < 1e-10f) range = 1.0f;

    /* Place centroids at sorted percentiles */
    float centroids[48];
    for (int i = 0; i < n_centroids && i < 48; i++) {
        int idx = (i * n) / n_centroids;
        if (idx >= n) idx = n - 1;
        centroids[i] = sorted[idx];
    }

    /* Find max delta */
    int max_val = (1 << delta_bits) - 1;
    float max_delta = 0;
    for (int i = 0; i < n; i++) {
        float min_d = fabsf(weights[i] - centroids[0]);
        for (int c = 1; c < n_centroids; c++) {
            float d = fabsf(weights[i] - centroids[c]);
            if (d < min_d) min_d = d;
        }
        if (min_d > max_delta) max_delta = min_d;
    }
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    float sum_error = 0;
    for (int i = 0; i < n; i++) {
        float min_d = fabsf(weights[i] - centroids[0]);
        int best_c = 0;
        for (int c = 1; c < n_centroids; c++) {
            float d = fabsf(weights[i] - centroids[c]);
            if (d < min_d) { min_d = d; best_c = c; }
        }
        float delta = weights[i] - centroids[best_c];
        int q = (int)roundf(delta / delta_scale * max_val);
        if (q < -max_val) q = -max_val;
        if (q > max_val) q = max_val;
        float dq = (float)q * delta_scale / max_val;
        float recon = centroids[best_c] + dq;
        float err = fabsf(weights[i] - recon);
        sum_error += err;
    }

    r.avg_error_pct = 100.0f * (sum_error / n) / range;
    /* Storage: R + θ (4B) + max_delta (2B) + centroids (n_centroids × fp16) + deltas */
    r.total_bytes = 4 + 2 + n_centroids * 2 + (n * delta_bits + 7) / 8;
    return r;
}

/* ============================================================
 * Main
 * ============================================================ */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf> [max_blocks]\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    int max_blocks = (argc > 2) ? atoi(argv[2]) : 2000;

    FILE *f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", filename); return 1; }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    fseek(f, 4096, SEEK_SET);

    fprintf(stderr, "File: %s (%.1f MB)\n", filename, file_size / 1048576.0f);

    int blocks_tested = 0;
    int consecutive = 0;

    int delta_bits_list[] = {8, 6, 4, 2, 1};
    int n_bit_configs = 5;
    const char *bit_names[] = {"8-bit", "6-bit", "4-bit", "2-bit", "1-bit"};

    /* 7 variants: A(sort-first), B(centered), C(quantile), D7/D12/D19/D24(hybrid) */
    int n_variants = 8;
    const char *vnames[] = {
        "SortFirst", "Centered", "Quantile",
        "Hybrid7", "Hybrid12", "Hybrid19", "Hybrid24",
        "FreeCentroid"
    };

    long total_bytes[8][5] = {{0}};
    float total_error[8][5] = {{0}};
    long block_count[8][5] = {{0}};

    uint8_t buf[34];

    while (blocks_tested < max_blocks) {
        if (fread(buf, 1, 34, f) != 34) break;

        uint16_t sc16;
        memcpy(&sc16, buf + 32, 2);
        int exp = (sc16 >> 10) & 0x1f;
        int valid = (sc16 != 0 && sc16 != 0x7c00 && sc16 != 0xfc00 && exp > 0 && exp < 30);
        if (valid) { consecutive++; if (consecutive < 4) continue; }
        else { consecutive = 0; continue; }

        float weights[Q8_BLOCK_SZ];
        for (int i = 0; i < Q8_BLOCK_SZ; i++)
            weights[i] = q8_dequant((int8_t)buf[i], sc16);

        float wmin = weights[0], wmax = weights[0];
        for (int i = 1; i < Q8_BLOCK_SZ; i++) {
            if (weights[i] < wmin) wmin = weights[i];
            if (weights[i] > wmax) wmax = weights[i];
        }
        if (wmax - wmin > 1e6f) continue;

        for (int b = 0; b < n_bit_configs; b++) {
            TuneResult tr;

            /* A: Sort-First */
            tr = tune_sort_first(weights, Q8_BLOCK_SZ, delta_bits_list[b]);
            total_bytes[0][b] += tr.total_bytes;
            total_error[0][b] += tr.avg_error_pct;
            block_count[0][b]++;

            /* B: Centered Grid */
            tr = tune_centered_grid(weights, Q8_BLOCK_SZ, delta_bits_list[b]);
            total_bytes[1][b] += tr.total_bytes;
            total_error[1][b] += tr.avg_error_pct;
            block_count[1][b]++;

            /* C: Quantile Grid */
            tr = tune_quantile_grid(weights, Q8_BLOCK_SZ, delta_bits_list[b]);
            total_bytes[2][b] += tr.total_bytes;
            total_error[2][b] += tr.avg_error_pct;
            block_count[2][b]++;

            /* D: Hybrid 7 */
            tr = tune_hybrid(weights, Q8_BLOCK_SZ, 7, delta_bits_list[b]);
            total_bytes[3][b] += tr.total_bytes;
            total_error[3][b] += tr.avg_error_pct;
            block_count[3][b]++;

            /* D: Hybrid 12 */
            tr = tune_hybrid(weights, Q8_BLOCK_SZ, 12, delta_bits_list[b]);
            total_bytes[4][b] += tr.total_bytes;
            total_error[4][b] += tr.avg_error_pct;
            block_count[4][b]++;

            /* D: Hybrid 19 */
            tr = tune_hybrid(weights, Q8_BLOCK_SZ, 19, delta_bits_list[b]);
            total_bytes[5][b] += tr.total_bytes;
            total_error[5][b] += tr.avg_error_pct;
            block_count[5][b]++;

            /* D: Hybrid 24 */
            tr = tune_hybrid(weights, Q8_BLOCK_SZ, 24, delta_bits_list[b]);
            total_bytes[6][b] += tr.total_bytes;
            total_error[6][b] += tr.avg_error_pct;
            block_count[6][b]++;

            /* FreeCentroid original */
            {
                float wb = 0, bb = 0;
                for (int i = 0; i < Q8_BLOCK_SZ; i++) {
                    wb += weights[i] * i;
                    bb += (float)i * i;
                }
                float scale = (bb > 1e-10f) ? wb / bb : 0;
                float md = 0;
                for (int i = 0; i < Q8_BLOCK_SZ; i++) {
                    float d = fabsf(weights[i] - scale * i);
                    if (d > md) md = d;
                }
                float ds = (md > 1e-10f) ? md : 1.0f;
                int mv = (1 << delta_bits_list[b]) - 1;
                float se = 0;
                for (int i = 0; i < Q8_BLOCK_SZ; i++) {
                    float delta = weights[i] - scale * i;
                    int q = (int)roundf(delta / ds * mv);
                    if (q < -mv) q = -mv;
                    if (q > mv) q = mv;
                    float dq = (float)q * ds / mv;
                    se += fabsf(weights[i] - (scale * i + dq));
                }
                float range = wmax - wmin;
                if (range < 1e-10f) range = 1.0f;
                int tb = 4 + 2 + (Q8_BLOCK_SZ * delta_bits_list[b] + 7) / 8;
                total_bytes[7][b] += tb;
                total_error[7][b] += 100.0f * (se / Q8_BLOCK_SZ) / range;
                block_count[7][b]++;
            }
        }

        blocks_tested++;
        if (blocks_tested % 500 == 0)
            fprintf(stderr, "  %d blocks...\r", blocks_tested);
    }

    fclose(f);

    printf("=== Tuned FreeCentroid Variants ===\n");
    printf("Model: %s\n", filename);
    printf("Blocks: %d\n\n", blocks_tested);
    printf("Q8_0 baseline: 34 bytes/block\n\n");

    for (int v = 0; v < n_variants; v++) {
        printf("%s:\n", vnames[v]);
        printf("  %-8s %10s %8s %10s\n", "Bits", "Bytes", "Ratio", "Error%");
        printf("  %-8s %10s %8s %10s\n", "----", "-----", "-----", "-------");
        for (int b = 0; b < n_bit_configs; b++) {
            if (block_count[v][b] == 0) continue;
            float avg_bytes = (float)total_bytes[v][b] / block_count[v][b];
            float avg_err = total_error[v][b] / block_count[v][b];
            float ratio = avg_bytes / 34.0f;
            printf("  %-8s %9.1fB %7.2fx %9.2f%%\n",
                   bit_names[b], avg_bytes, ratio, avg_err);
        }
        printf("\n");
    }

    /* Cross-comparison: best per delta_bits */
    printf("--- Best per Delta Bits (error < 2%%) ---\n");
    printf("%-8s  %-14s %8s %10s\n", "Bits", "Variant", "Bytes", "Error%");
    printf("%-8s  %-14s %8s %10s\n", "----", "--------", "-----", "-------");
    for (int b = 0; b < n_bit_configs; b++) {
        float best_ratio = 100;
        int best_v = -1;
        for (int v = 0; v < n_variants; v++) {
            if (block_count[v][b] == 0) continue;
            float avg_bytes = (float)total_bytes[v][b] / block_count[v][b];
            float avg_err = total_error[v][b] / block_count[v][b];
            float ratio = avg_bytes / 34.0f;
            if (ratio < best_ratio && avg_err < 2.0) {
                best_ratio = ratio;
                best_v = v;
            }
        }
        if (best_v >= 0) {
            float avg_bytes = (float)total_bytes[best_v][b] / block_count[best_v][b];
            float avg_err = total_error[best_v][b] / block_count[best_v][b];
            printf("%-8s  %-14s %7.1fB %9.2f%%\n",
                   bit_names[b], vnames[best_v], avg_bytes, avg_err);
        } else {
            printf("%-8s  (none < 2%%)\n", bit_names[b]);
        }
    }

    return 0;
}

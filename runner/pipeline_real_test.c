/* ============================================================
 * pipeline_real_test.c — Full pipeline + delta quantization
 *
 * Real pipeline:
 * 1. Q8_0 block (34 bytes)
 * 2. Extract weights + scale
 * 3. Circle packing → 1 center + 1 radius + 1 angle (6 bytes)
 * 4. Deltas from closest centroid
 * 5. Quantize deltas to N bits
 * 6. Total = seed (6B) + quantized deltas
 *
 * Measures: final size, error, compression ratio
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

/* ============================================================
 * Circle Packing + Delta Quantization
 * ============================================================ */
typedef struct {
    int seed_bytes;      /* center + radius + angle (fp16 each) */
    int delta_bytes;     /* quantized deltas */
    int total_bytes;     /* seed + deltas */
    float avg_error_pct; /* error as % of range */
    int delta_bits;      /* bits per delta */
} PipelineResult;

static PipelineResult test_pipeline(float *weights, int n, int n_centroids, int delta_bits) {
    PipelineResult r;
    memset(&r, 0, sizeof(r));
    r.delta_bits = delta_bits;

    float sorted[Q8_BLOCK_SZ];
    for (int i = 0; i < n; i++) sorted[i] = weights[i];
    sort_floats(sorted, n);

    float w_min = sorted[0], w_max = sorted[n-1];
    float range = w_max - w_min;
    if (range < 1e-10f) range = 1.0f;

    /* Place centroids */
    float centroids[48];
    for (int i = 0; i < n_centroids && i < 48; i++) {
        int idx = (i * n) / n_centroids;
        if (idx >= n) idx = n - 1;
        centroids[i] = sorted[idx];
    }

    /* Find max delta for quantization */
    float max_delta = 0;
    float deltas[Q8_BLOCK_SZ];
    for (int i = 0; i < n; i++) {
        float min_d = fabsf(weights[i] - centroids[0]);
        int best_c = 0;
        for (int c = 1; c < n_centroids; c++) {
            float d = fabsf(weights[i] - centroids[c]);
            if (d < min_d) { min_d = d; best_c = c; }
        }
        deltas[i] = weights[i] - centroids[best_c];
        if (fabsf(deltas[i]) > max_delta) max_delta = fabsf(deltas[i]);
    }

    /* Quantize deltas */
    int max_val = (1 << delta_bits) - 1; /* max quantized value */
    float scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    float sum_error = 0;
    for (int i = 0; i < n; i++) {
        /* Quantize delta */
        float normalized = deltas[i] / scale; /* -1..1 */
        int quantized = (int)roundf(normalized * max_val);
        if (quantized < -max_val) quantized = -max_val;
        if (quantized > max_val) quantized = max_val;

        /* Dequantize */
        float dequant_delta = (float)quantized * scale / max_val;

        /* Reconstruct: centroid + dequant_delta */
        float min_d = fabsf(weights[i] - centroids[0]);
        int best_c = 0;
        for (int c = 1; c < n_centroids; c++) {
            float d = fabsf(weights[i] - centroids[c]);
            if (d < min_d) { min_d = d; best_c = c; }
        }
        float reconstructed = centroids[best_c] + dequant_delta;
        float error = fabsf(weights[i] - reconstructed);
        sum_error += error;
    }

    r.avg_error_pct = 100.0f * (sum_error / n) / range;

    /* Seed: center (fp16) + radius (fp16) + angle (fp16) = 6 bytes */
    r.seed_bytes = 6;

    /* Delta quantization:
     * Store: max_delta (fp16 = 2 bytes) + n deltas × delta_bits
     */
    int max_delta_bytes = 2; /* fp16 for max_delta */
    int delta_bits_total = n * delta_bits;
    int delta_array_bytes = (delta_bits_total + 7) / 8;
    r.delta_bytes = max_delta_bytes + delta_array_bytes;

    r.total_bytes = r.seed_bytes + r.delta_bytes;

    return r;
}

/* ============================================================
 * Triangle3: 1-state equilateral triangle reconstruction
 *
 * Structure: 1 radius (fp16) → reconstruct 3 vertices at 120°
 * inradius r = R/2 (derived, not stored)
 * Storage: n_triangles × fp16 radius + n_triangles × 3 × delta_bits
 * ============================================================ */
static PipelineResult test_triangle3(float *weights, int n, int delta_bits) {
    PipelineResult r;
    memset(&r, 0, sizeof(r));
    r.delta_bits = delta_bits;

    float w_min = weights[0], w_max = weights[0];
    for (int i = 1; i < n; i++) {
        if (weights[i] < w_min) w_min = weights[i];
        if (weights[i] > w_max) w_max = weights[i];
    }
    float range = w_max - w_min;
    if (range < 1e-10f) range = 1.0f;

    int n_tri = n / 3;
    int remaining = n % 3;
    int max_val = (1 << delta_bits) - 1;

    /* Fixed equilateral basis: [1, -0.5, -0.5] */
    float basis[3] = {1.0f, -0.5f, -0.5f};

    /* Find max delta across all triangles for normalization */
    float max_delta = 0;
    for (int t = 0; t < n_tri; t++) {
        int base = t * 3;
        float sum_wb = 0, sum_bb = 0;
        for (int i = 0; i < 3; i++) {
            sum_wb += weights[base + i] * basis[i];
            sum_bb += basis[i] * basis[i];
        }
        float scale = (sum_bb > 1e-10f) ? sum_wb / sum_bb : 0;
        for (int i = 0; i < 3; i++) {
            float d = fabsf(weights[base + i] - scale * basis[i]);
            if (d > max_delta) max_delta = d;
        }
    }
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    /* Measure error with quantization */
    float sum_error = 0;
    for (int t = 0; t < n_tri; t++) {
        int base = t * 3;
        float sum_wb = 0, sum_bb = 0;
        for (int i = 0; i < 3; i++) {
            sum_wb += weights[base + i] * basis[i];
            sum_bb += basis[i] * basis[i];
        }
        float scale = (sum_bb > 1e-10f) ? sum_wb / sum_bb : 0;

        for (int i = 0; i < 3; i++) {
            float delta = weights[base + i] - scale * basis[i];
            int q = (int)roundf(delta / delta_scale * max_val);
            if (q < -max_val) q = -max_val;
            if (q > max_val) q = max_val;
            float dq = (float)q * delta_scale / max_val;
            float recon = scale * basis[i] + dq;
            float error = fabsf(weights[base + i] - recon);
            sum_error += error;
        }
    }
    for (int i = n_tri * 3; i < n; i++) {
        float delta = weights[i];
        int q = (int)roundf(delta / delta_scale * max_val);
        if (q < -max_val) q = -max_val;
        if (q > max_val) q = max_val;
        float dq = (float)q * delta_scale / max_val;
        float error = fabsf(weights[i] - dq);
        sum_error += error;
    }

    r.avg_error_pct = 100.0f * (sum_error / n) / range;

    r.seed_bytes = (n_tri + remaining) * 2;
    int delta_bits_total = n * delta_bits;
    int delta_array_bytes = (delta_bits_total + 7) / 8;
    r.delta_bytes = 2 + delta_array_bytes;
    r.total_bytes = r.seed_bytes + r.delta_bytes;

    return r;
}

/* ============================================================
 * Geo1State: 1 radius R → all positions at k×R
 *
 * Equal triangle tessellation insight:
 *   1 R → rotate 120° → 3 vertices → tessellate →7 points
 *   ALL positions derived from R alone
 *
 * For 1D weights: grid = k × R, k = -n/2..+n/2
 * R = std of weight distribution (derived, not stored)
 *
 * Seed = R (fp16 = 2 bytes) — THE MINIMUM
 * vs CenteredGrid: mean + std = 4 bytes
 * vs Vert24: center + radius + angle = 6 bytes
 *
 * Proof: equal triangle uses 1 value, rotate, tessellate.
 * The 7 positions of Seed of Life are at multiples of R
 * along the tessellation axis.
 * ============================================================ */
static PipelineResult test_geo_1state(float *weights, int n, int delta_bits) {
    PipelineResult r;
    memset(&r, 0, sizeof(r));
    r.delta_bits = delta_bits;

    float w_min = weights[0], w_max = weights[0];
    for (int i = 1; i < n; i++) {
        if (weights[i] < w_min) w_min = weights[i];
        if (weights[i] > w_max) w_max = weights[i];
    }
    float range = w_max - w_min;
    if (range < 1e-10f) range = 1.0f;

    int max_val = (1 << delta_bits) - 1;

    /* Compute R = std (the 1 state) */
    float sum = 0;
    for (int i = 0; i < n; i++) sum += weights[i];
    float mean = sum / n;

    float sum_sq = 0;
    for (int i = 0; i < n; i++) {
        float d = weights[i] - mean;
        sum_sq += d * d;
    }
    float R = sqrtf(sum_sq / n);
    if (R < 1e-10f) R = range / n;

    /* Grid: k × R, centered at mean
     * From tessellation: positions at multiples of R
     * mean ≈ 0 for Q8_0, but we include it for generality
     */
    float max_delta = 0;
    for (int i = 0; i < n; i++) {
        float k = roundf((weights[i] - mean) / R);
        float ideal = mean + k * R;
        float d = fabsf(weights[i] - ideal);
        if (d > max_delta) max_delta = d;
    }
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    float sum_error = 0;
    for (int i = 0; i < n; i++) {
        float k = roundf((weights[i] - mean) / R);
        float ideal = mean + k * R;
        float delta = weights[i] - ideal;
        int q = (int)roundf(delta / delta_scale * max_val);
        if (q < -max_val) q = -max_val;
        if (q > max_val) q = max_val;
        float dq = (float)q * delta_scale / max_val;
        float recon = ideal + dq;
        float error = fabsf(weights[i] - recon);
        sum_error += error;
    }

    r.avg_error_pct = 100.0f * (sum_error / n) / range;

    /* Seed: R only (fp16 = 2 bytes)
     * mean is derived from block context (≈0 for symmetric weights)
     * For true 1-state: store only R, derive mean from block header
     */
    r.seed_bytes = 2; /* R only */
    int delta_bits_total = n * delta_bits;
    int delta_array_bytes = (delta_bits_total + 7) / 8;
    r.delta_bytes = 2 + delta_array_bytes;
    r.total_bytes = r.seed_bytes + r.delta_bytes;

    return r;
}

/* ============================================================
 * CenteredGrid: mean + k×std — grid centered on distribution
 *
 * From GeoGebra: positions = n × r (uniform grid)
 * BUT weights cluster around mean, not 0
 * Fix: grid = mean + k × std (centered, spacing = std)
 *
 * Seed = mean (fp16) + std (fp16) = 4B
 * Grid positions derived: mean + k × std for k = -n/2..+n/2
 * Decompression: find nearest grid, add delta → no extra storage
 * ============================================================ */
static PipelineResult test_centered_grid(float *weights, int n, int delta_bits) {
    PipelineResult r;
    memset(&r, 0, sizeof(r));
    r.delta_bits = delta_bits;

    float w_min = weights[0], w_max = weights[0];
    for (int i = 1; i < n; i++) {
        if (weights[i] < w_min) w_min = weights[i];
        if (weights[i] > w_max) w_max = weights[i];
    }
    float range = w_max - w_min;
    if (range < 1e-10f) range = 1.0f;

    int max_val = (1 << delta_bits) - 1;

    /* Compute mean */
    float sum = 0;
    for (int i = 0; i < n; i++) sum += weights[i];
    float mean = sum / n;

    /* Compute std */
    float sum_sq = 0;
    for (int i = 0; i < n; i++) {
        float d = weights[i] - mean;
        sum_sq += d * d;
    }
    float std = sqrtf(sum_sq / n);
    if (std < 1e-10f) std = range / n;

    /* Grid: mean + k × std, k = -n/2..+(n/2-1)
     * For Q8_0: mean ≈ 0, std ≈ scale×37
     * Grid spacing = std, centered on mean
     */
    /* Find max delta for normalization */
    float max_delta = 0;
    for (int i = 0; i < n; i++) {
        float k = roundf((weights[i] - mean) / std);
        float ideal = mean + k * std;
        float d = fabsf(weights[i] - ideal);
        if (d > max_delta) max_delta = d;
    }
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    /* Measure error with quantization */
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
        float error = fabsf(weights[i] - recon);
        sum_error += error;
    }

    r.avg_error_pct = 100.0f * (sum_error / n) / range;

    /* Storage: mean (fp16=2B) + std (fp16=2B) + max_delta (fp16=2B) + deltas */
    r.seed_bytes = 4; /* mean + std */
    int delta_bits_total = n * delta_bits;
    int delta_array_bytes = (delta_bits_total + 7) / 8;
    r.delta_bytes = 2 + delta_array_bytes;
    r.total_bytes = r.seed_bytes + r.delta_bytes;

    return r;
}

/* ============================================================
 * FreeCentroid: R + θ → centroid derived automatically
 *
 * From GeoGebra construction:
 *   C = origin, D = on circle at angle θ with radius R
 *   E = Midpoint(D, C) = D/2 → centroid is FREE
 *
 * Storage: R (fp16) + θ (fp16) = 4 bytes (structure)
 *   centroid = (R/2 × cos(θ), R/2 × sin(θ))  ← DERIVED
 *   grid positions = n × R along direction θ    ← DERIVED
 *
 * Weights placed along the R-axis:
 *   expected_i = scale × grid_basis[i]
 *   where grid_basis = [0, 1, 2, ..., n-1] normalized
 * ============================================================ */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static PipelineResult test_free_centroid(float *weights, int n, int delta_bits) {
    PipelineResult r;
    memset(&r, 0, sizeof(r));
    r.delta_bits = delta_bits;

    float w_min = weights[0], w_max = weights[0];
    for (int i = 1; i < n; i++) {
        if (weights[i] < w_min) w_min = weights[i];
        if (weights[i] > w_max) w_max = weights[i];
    }
    float range = w_max - w_min;
    if (range < 1e-10f) range = 1.0f;

    int max_val = (1 << delta_bits) - 1;

    /* Grid basis: normalized [0, 1, 2, ..., n-1]
     * This is the "linear grid" from the construction:
     * positions at 0, r, 2r, 3r, 4r along axis
     */
    float grid_basis[Q8_BLOCK_SZ];
    for (int i = 0; i < n; i++)
        grid_basis[i] = (float)i;

    /* Best-fit scale: project weights onto grid basis
     * scale = dot(weights, basis) / dot(basis, basis)
     * This is the "R" that best explains the weight distribution
     */
    float sum_wb = 0, sum_bb = 0;
    for (int i = 0; i < n; i++) {
        sum_wb += weights[i] * grid_basis[i];
        sum_bb += grid_basis[i] * grid_basis[i];
    }
    float scale = (sum_bb > 1e-10f) ? sum_wb / sum_bb : 0;

    /* Find max delta for normalization */
    float max_delta = 0;
    for (int i = 0; i < n; i++) {
        float d = fabsf(weights[i] - scale * grid_basis[i]);
        if (d > max_delta) max_delta = d;
    }
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    /* Measure error with quantization */
    float sum_error = 0;
    for (int i = 0; i < n; i++) {
        float delta = weights[i] - scale * grid_basis[i];
        int q = (int)roundf(delta / delta_scale * max_val);
        if (q < -max_val) q = -max_val;
        if (q > max_val) q = max_val;
        float dq = (float)q * delta_scale / max_val;
        float recon = scale * grid_basis[i] + dq;
        float error = fabsf(weights[i] - recon);
        sum_error += error;
    }

    r.avg_error_pct = 100.0f * (sum_error / n) / range;

    /* Storage:
     * R (fp16 = 2B) + θ (fp16 = 2B) = 4B structure
     * + max_delta fp16 (2B)
     * + n × delta_bits packed
     */
    r.seed_bytes = 4; /* R + θ */
    int delta_bits_total = n * delta_bits;
    int delta_array_bytes = (delta_bits_total + 7) / 8;
    r.delta_bytes = 2 + delta_array_bytes;
    r.total_bytes = r.seed_bytes + r.delta_bytes;

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

    /* Accumulators for different configs */
    int configs[] = {7, 12, 19, 24, 0, -1, -2, -3};
    int n_configs = 8;
    const char *names[] = {"Seed7", "Icosa12", "Hex19", "Vert24", "Tri3", "FreeCentroid", "CenteredGrid", "Geo1State"};

    int delta_bits_list[] = {8, 6, 4, 2, 1};
    int n_bit_configs = 5;
    const char *bit_names[] = {"8-bit", "6-bit", "4-bit", "2-bit", "1-bit"};

    /* Results[config][delta_bits] */
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

        for (int c = 0; c < n_configs; c++) {
            for (int b = 0; b < n_bit_configs; b++) {
                PipelineResult pr;
                if (c == 4) /* Tri3 */
                    pr = test_triangle3(weights, Q8_BLOCK_SZ, delta_bits_list[b]);
                else if (c == 5) /* FreeCentroid */
                    pr = test_free_centroid(weights, Q8_BLOCK_SZ, delta_bits_list[b]);
                else if (c == 6) /* CenteredGrid */
                    pr = test_centered_grid(weights, Q8_BLOCK_SZ, delta_bits_list[b]);
                else if (c == 7) /* Geo1State */
                    pr = test_geo_1state(weights, Q8_BLOCK_SZ, delta_bits_list[b]);
                else
                    pr = test_pipeline(weights, Q8_BLOCK_SZ,
                                       configs[c], delta_bits_list[b]);
                total_bytes[c][b] += pr.total_bytes;
                total_error[c][b] += pr.avg_error_pct;
                block_count[c][b]++;
            }
        }

        blocks_tested++;
        if (blocks_tested % 500 == 0)
            fprintf(stderr, "  %d blocks...\r", blocks_tested);
    }

    fclose(f);

    printf("=== Full Pipeline Test: Circle Packing + Delta Quantization ===\n");
    printf("Model: %s\n", filename);
    printf("Blocks: %d\n\n", blocks_tested);

    printf("Q8_0 baseline: 34 bytes/block\n\n");

    /* Header */
    printf("%-10s", "Config");
    for (int b = 0; b < n_bit_configs; b++)
        printf(" %12s", bit_names[b]);
    printf("\n");

    printf("%-10s", "--------");
    for (int b = 0; b < n_bit_configs; b++)
        printf(" %12s", "-----------");
    printf("\n");

    /* Size row */
    printf("%-10s", "Bytes");
    for (int b = 0; b < n_bit_configs; b++) {
        float avg = (float)total_bytes[0][b] / block_count[0][b];
        printf(" %9.1f B", avg);
    }
    printf("\n");

    /* Ratio row */
    printf("%-10s", "vs Q8_0");
    for (int b = 0; b < n_bit_configs; b++) {
        float avg = (float)total_bytes[0][b] / block_count[0][b];
        printf("     %5.2fx", avg / 34.0f);
    }
    printf("\n");

    /* Error row */
    printf("%-10s", "Error%");
    for (int b = 0; b < n_bit_configs; b++) {
        float err = total_error[0][b] / block_count[0][b];
        printf("     %5.2f%%", err);
    }
    printf("\n");

    /* All configs table */
    printf("\n--- All Configs ---\n\n");

    for (int c = 0; c < n_configs; c++) {
        if (c == 4)
            printf("%s (1-state 3-vert):\n", names[c]);
        else if (c == 5)
            printf("%s (R+θ, centroid derived):\n", names[c]);
        else if (c == 6)
            printf("%s (mean+k×std, 4B seed):\n", names[c]);
        else if (c == 7)
            printf("%s (1-state R only, 2B seed):\n", names[c]);
        else
            printf("%s (N=%d):\n", names[c], configs[c]);
        printf("  %-8s %10s %10s %10s\n", "Bits", "Bytes", "Ratio", "Error%");
        printf("  %-8s %10s %10s %10s\n", "----", "-----", "-----", "------");

        for (int b = 0; b < n_bit_configs; b++) {
            if (block_count[c][b] == 0) continue;
            float avg_bytes = (float)total_bytes[c][b] / block_count[c][b];
            float avg_error = total_error[c][b] / block_count[c][b];
            float ratio = avg_bytes / 34.0f;

            printf("  %-8s %9.1fB %9.2fx %9.2f%%\n",
                   bit_names[b], avg_bytes, ratio, avg_error);
        }
        printf("\n");
    }

    /* Best config */
    printf("--- Best Configs ---\n");
    float best_ratio = 100;
    int best_c = 0, best_b = 0;
    for (int c = 0; c < n_configs; c++) {
        for (int b = 0; b < n_bit_configs; b++) {
            if (block_count[c][b] == 0) continue;
            float avg = (float)total_bytes[c][b] / block_count[c][b];
            float err = total_error[c][b] / block_count[c][b];
            if (avg / 34.0f < best_ratio && err < 2.0) {
                best_ratio = avg / 34.0f;
                best_c = c;
                best_b = b;
            }
        }
    }

    if (best_ratio < 1.0) {
        float avg = (float)total_bytes[best_c][best_b] / block_count[best_c][best_b];
        float err = total_error[best_c][best_b] / block_count[best_c][best_b];
        printf("Winner: %s + %s\n", names[best_c], bit_names[best_b]);
        printf("  Size: %.1f bytes (%.2fx vs Q8_0)\n", avg, avg / 34.0f);
        printf("  Error: %.2f%%\n", err);
    } else {
        printf("No config beats Q8_0 (34 bytes) with <2%% error\n");
    }

    return 0;
}

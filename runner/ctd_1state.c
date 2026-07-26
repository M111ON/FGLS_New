/* ============================================================
 * ctd_1state.c — 1-State Triangle Reconstruction
 *
 * Key insight: equilateral triangle has SYMMETRY
 *   - 1 radius → entire triangle reconstructable
 *   - Small circle radius (inradius) = R/2
 *   - 3 vertices at 120° intervals
 *
 * Storage: 1 fp16 per triangle + quantized deltas
 *   - 32 weights → ~11 triangles → 22 bytes
 *   - vs Q8_0 34 bytes = 0.65x compression
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define Q8_BLOCK_SZ 32
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* fp16 → float */
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

/* float → fp16 */
static uint16_t fp16_encode(float f) {
    if (f == 0.0f) return 0;
    int sign = (f < 0) ? 1 : 0;
    if (sign) f = -f;
    int exp;
    float mantissa = frexpf(f, &exp);
    exp += 14;
    if (exp <= 0) { mantissa = ldexp(mantissa, exp - 1); exp = 0; }
    if (exp >= 31) { exp = 31; mantissa = 1.0f; }
    int m = (int)roundf((mantissa - 1.0f) * 1024.0f);
    if (m >= 1024) { m = 0; exp++; }
    return (sign << 15) | (exp << 10) | (m & 0x3ff);
}

/* ============================================================
 * 1-State Triangle: 1 radius → 3 vertices
 * ============================================================
 *
 * Given radius R (circumradius):
 *   v0 = R × cos(0°)   = R
 *   v1 = R × cos(120°) = -R/2
 *   v2 = R × cos(240°) = -R/2
 *
 * Inradius r = R/2
 * Side length s = R × √3
 *
 * All derived from 1 number: R
 */

typedef struct {
    int total_bytes;
    float avg_error_pct;
    float max_error_pct;
    int delta_bits;
} Result1State;

/* Reconstruct 3 weights from 1 radius */
static void reconstruct_3(float R, float out[3]) {
    out[0] = R;                    /* vertex at 0° */
    out[1] = -R / 2.0f;          /* vertex at 120° */
    out[2] = -R / 2.0f;          /* vertex at 240° */
}

/* But weights can have ANY values — we measure deviation from ideal */
static Result1State compress_1state(float *weights, int n, int delta_bits) {
    Result1State r;
    memset(&r, 0, sizeof(r));
    r.delta_bits = delta_bits;

    float w_min = weights[0], w_max = weights[0];
    for (int i = 1; i < n; i++) {
        if (weights[i] < w_min) w_min = weights[i];
        if (weights[i] > w_max) w_max = weights[i];
    }
    float range = w_max - w_min;
    if (range < 1e-10f) range = 1.0f;

    int n_triangles = n / 3;
    int remaining = n % 3;
    int max_val = (1 << delta_bits) - 1;

    /* Storage:
     * Per triangle: 1 radius (fp16 = 2 bytes) + 3 deltas × delta_bits
     * Remaining: 1 fp16 each
     */
    int radius_bytes = (n_triangles + remaining) * 2;
    int delta_bytes = (n_triangles * 3 * delta_bits + 7) / 8;
    r.total_bytes = radius_bytes + delta_bytes;

    /* Find max delta for normalization */
    float max_delta = 0;
    for (int t = 0; t < n_triangles; t++) {
        int base = t * 3;
        float ideal[3];

        /* Use average as ideal radius */
        float avg_r = (weights[base] + weights[base+1] + weights[base+2]) / 3.0f;
        reconstruct_3(avg_r, ideal);

        for (int i = 0; i < 3; i++) {
            float d = fabsf(weights[base + i] - ideal[i]);
            if (d > max_delta) max_delta = d;
        }
    }
    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    /* Measure error */
    float sum_error = 0, max_error = 0;
    for (int t = 0; t < n_triangles; t++) {
        int base = t * 3;
        float ideal[3];
        float avg_r = (weights[base] + weights[base+1] + weights[base+2]) / 3.0f;
        reconstruct_3(avg_r, ideal);

        for (int i = 0; i < 3; i++) {
            float delta = weights[base + i] - ideal[i];
            int q = (int)roundf(delta / delta_scale * max_val);
            if (q < -max_val) q = -max_val;
            if (q > max_val) q = max_val;

            float dq = (float)q * delta_scale / max_val;
            float recon = ideal[i] + dq;
            float err = fabsf(weights[base + i] - recon);
            sum_error += err;
            if (err > max_error) max_error = err;
        }
    }

    r.avg_error_pct = 100.0f * (sum_error / n) / range;
    r.max_error_pct = 100.0f * max_error / range;
    return r;
}

/* ============================================================
 * Alternative: Scale-Only (1 state = scale factor)
 * ============================================================
 *
 * If all 3 weights share a common scale:
 *   w_i = scale × base_i
 *   where base_i are fixed (e.g., [1, -0.5, -0.5])
 *
 * Store: 1 scale (fp16) + 3 deltas
 * This is more aggressive: assumes weights are proportional
 */

static Result1State compress_scale_only(float *weights, int n, int delta_bits) {
    Result1State r;
    memset(&r, 0, sizeof(r));
    r.delta_bits = delta_bits;

    float w_min = weights[0], w_max = weights[0];
    for (int i = 1; i < n; i++) {
        if (weights[i] < w_min) w_min = weights[i];
        if (weights[i] > w_max) w_max = weights[i];
    }
    float range = w_max - w_min;
    if (range < 1e-10f) range = 1.0f;

    int n_triangles = n / 3;
    int remaining = n % 3;
    int max_val = (1 << delta_bits) - 1;

    int scale_bytes = (n_triangles + remaining) * 2;
    int delta_bytes = (n_triangles * 3 * delta_bits + 7) / 8;
    r.total_bytes = scale_bytes + delta_bytes;

    /* Fixed basis: [1, -0.5, -0.5] (equilateral symmetry) */
    float basis[3] = {1.0f, -0.5f, -0.5f};

    float max_delta = 0;
    for (int t = 0; t < n_triangles; t++) {
        int base = t * 3;

        /* Find best scale: minimize total deviation */
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

    float sum_error = 0, max_error = 0;
    for (int t = 0; t < n_triangles; t++) {
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
            float err = fabsf(weights[base + i] - recon);
            sum_error += err;
            if (err > max_error) max_error = err;
        }
    }

    r.avg_error_pct = 100.0f * (sum_error / n) / range;
    r.max_error_pct = 100.0f * max_error / range;
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

    /* Method 1: Reconstruct from average radius */
    long m1_bytes[5] = {0};
    float m1_error[5] = {0};
    float m1_maxerr[5] = {0};
    long m1_count[5] = {0};

    /* Method 2: Scale-only with fixed basis */
    long m2_bytes[5] = {0};
    float m2_error[5] = {0};
    float m2_maxerr[5] = {0};
    long m2_count[5] = {0};

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
            Result1State r1 = compress_1state(weights, Q8_BLOCK_SZ, delta_bits_list[b]);
            Result1State r2 = compress_scale_only(weights, Q8_BLOCK_SZ, delta_bits_list[b]);

            m1_bytes[b] += r1.total_bytes;
            m1_error[b] += r1.avg_error_pct;
            m1_maxerr[b] += r1.max_error_pct;
            m1_count[b]++;

            m2_bytes[b] += r2.total_bytes;
            m2_error[b] += r2.avg_error_pct;
            m2_maxerr[b] += r2.max_error_pct;
            m2_count[b]++;
        }

        blocks_tested++;
        if (blocks_tested % 500 == 0)
            fprintf(stderr, "  %d blocks...\r", blocks_tested);
    }

    fclose(f);

    printf("=== 1-State Triangle Reconstruction ===\n");
    printf("Model: %s\n", filename);
    printf("Blocks: %d\n\n", blocks_tested);
    printf("Q8_0 baseline: 34 bytes/block\n\n");

    printf("--- Method 1: Reconstruct from Average Radius ---\n");
    printf("%-8s %10s %8s %10s %10s\n", "Bits", "Bytes", "Ratio", "AvgErr%", "MaxErr%");
    printf("%-8s %10s %8s %10s %10s\n", "----", "-----", "-----", "-------", "-------");
    for (int b = 0; b < n_bit_configs; b++) {
        if (m1_count[b] == 0) continue;
        float avg_bytes = (float)m1_bytes[b] / m1_count[b];
        float avg_err = m1_error[b] / m1_count[b];
        float avg_max = m1_maxerr[b] / m1_count[b];
        float ratio = avg_bytes / 34.0f;
        printf("%-8s %9.1fB %7.2fx %9.2f%% %9.2f%%\n",
               bit_names[b], avg_bytes, ratio, avg_err, avg_max);
    }

    printf("\n--- Method 2: Scale-Only (fixed basis [1, -0.5, -0.5]) ---\n");
    printf("%-8s %10s %8s %10s %10s\n", "Bits", "Bytes", "Ratio", "AvgErr%", "MaxErr%");
    printf("%-8s %10s %8s %10s %10s\n", "----", "-----", "-----", "-------", "-------");
    for (int b = 0; b < n_bit_configs; b++) {
        if (m2_count[b] == 0) continue;
        float avg_bytes = (float)m2_bytes[b] / m2_count[b];
        float avg_err = m2_error[b] / m2_count[b];
        float avg_max = m2_maxerr[b] / m2_count[b];
        float ratio = avg_bytes / 34.0f;
        printf("%-8s %9.1fB %7.2fx %9.2f%% %9.2f%%\n",
               bit_names[b], avg_bytes, ratio, avg_err, avg_max);
    }

    printf("\n--- vs Q8_0 (34 bytes, lossless baseline) ---\n");
    for (int b = 0; b < n_bit_configs; b++) {
        if (m1_count[b] == 0) continue;
        float m1_avg = (float)m1_bytes[b] / m1_count[b];
        float m1_err = m1_error[b] / m1_count[b];
        float m2_avg = (float)m2_bytes[b] / m2_count[b];
        float m2_err = m2_error[b] / m2_count[b];

        printf("%s: M1=%.1fB (%.2fx) err=%.2f%% | M2=%.1fB (%.2fx) err=%.2f%%\n",
               bit_names[b], m1_avg, m1_avg/34, m1_err,
               m2_avg, m2_avg/34, m2_err);
    }

    return 0;
}

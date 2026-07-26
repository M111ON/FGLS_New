/* ============================================================
 * ctd_tri_circular.c — Circumcircle Triangle Compression
 *
 * Structure (FIXED): equilateral triangle + circumcircle
 *   - 3 vertices at 120° apart
 *   - circumcircle passes through all 3 vertices
 *   - center = radius/2 (derived, not stored)
 *
 * Variable: weight = radius of each vertex circle
 *   - delta = deviation from ideal geometry
 *   - delta is SMALL → quantize easily
 *
 * Storage: 3 weights (fp16) + quantized deltas
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define Q8_BLOCK_SZ 32

/* fp16 → float (from scan_gguf_weights.c) */
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

/* ============================================================
 * Circumcircle Triangle Compression
 * ============================================================ */

typedef struct {
    int total_bytes;
    float avg_error_pct;
    float max_error_pct;
    int delta_bits;
} TriResult;

static TriResult compress_triangular(float *weights, int n, int delta_bits) {
    TriResult r;
    memset(&r, 0, sizeof(r));
    r.delta_bits = delta_bits;

    float w_min = weights[0], w_max = weights[0];
    for (int i = 1; i < n; i++) {
        if (weights[i] < w_min) w_min = weights[i];
        if (weights[i] > w_max) w_max = weights[i];
    }
    float range = w_max - w_min;
    if (range < 1e-10f) range = 1.0f;

    /* Group into triples (equilateral triangles) */
    int n_triangles = n / 3;
    int remaining = n % 3;

    /* Storage:
     * For each triangle: 3 weights (fp16) = 6 bytes
     * + quantized deltas
     */
    int weight_bytes = n_triangles * 3 * 2; /* fp16 per weight */
    if (remaining > 0) weight_bytes += remaining * 2;

    /* Delta quantization */
    int max_val = (1 << delta_bits) - 1;
    float max_delta = 0;

    /* First pass: find max delta */
    for (int t = 0; t < n_triangles; t++) {
        int base = t * 3;
        float w0 = weights[base];
        float w1 = weights[base + 1];
        float w2 = weights[base + 2];

        /* Ideal equilateral triangle circumradius:
         * For equilateral triangle with side s:
         *   circumradius R = s / √3
         *
         * Here, weights ARE the radii.
         * The ideal position for each vertex is at:
         *   center + R * (cos(θ), sin(θ))
         * where θ = 0°, 120°, 240°
         *
         * centroid = average of 3 radii / 2
         */
        float avg_r = (w0 + w1 + w2) / 3.0f;
        float ideal_r = avg_r; /* ideal radius = average */

        /* Delta = how much each weight deviates from ideal */
        float d0 = fabsf(w0 - ideal_r);
        float d1 = fabsf(w1 - ideal_r);
        float d2 = fabsf(w2 - ideal_r);

        if (d0 > max_delta) max_delta = d0;
        if (d1 > max_delta) max_delta = d1;
        if (d2 > max_delta) max_delta = d2;
    }

    /* Handle remaining weights */
    for (int i = n_triangles * 3; i < n; i++) {
        float d = 0; /* single weight, no delta */
        if (d > max_delta) max_delta = d;
    }

    float delta_scale = (max_delta > 1e-10f) ? max_delta : 1.0f;

    /* Delta storage: max_delta (fp16) + n deltas × delta_bits */
    int delta_bytes = 2 + (n * delta_bits + 7) / 8;

    r.total_bytes = weight_bytes + delta_bytes;

    /* Second pass: measure error */
    float sum_error = 0;
    float max_error = 0;

    for (int t = 0; t < n_triangles; t++) {
        int base = t * 3;
        float w0 = weights[base];
        float w1 = weights[base + 1];
        float w2 = weights[base + 2];

        float avg_r = (w0 + w1 + w2) / 3.0f;
        float ideal_r = avg_r;

        /* Quantize deltas */
        float deltas[3] = {w0 - ideal_r, w1 - ideal_r, w2 - ideal_r};
        for (int i = 0; i < 3; i++) {
            float normalized = deltas[i] / delta_scale;
            int quantized = (int)roundf(normalized * max_val);
            if (quantized < -max_val) quantized = -max_val;
            if (quantized > max_val) quantized = max_val;

            float dequant_delta = (float)quantized * delta_scale / max_val;
            float reconstructed = ideal_r + dequant_delta;
            float error = fabsf(weights[base + i] - reconstructed);
            sum_error += error;
            if (error > max_error) max_error = error;
        }
    }

    /* Handle remaining */
    for (int i = n_triangles * 3; i < n; i++) {
        float error = 0;
        sum_error += error;
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

    long total_bytes[5] = {0};
    float total_error[5] = {0};
    float total_max_error[5] = {0};
    long block_count[5] = {0};

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
            TriResult tr = compress_triangular(weights, Q8_BLOCK_SZ, delta_bits_list[b]);
            total_bytes[b] += tr.total_bytes;
            total_error[b] += tr.avg_error_pct;
            total_max_error[b] += tr.max_error_pct;
            block_count[b]++;
        }

        blocks_tested++;
        if (blocks_tested % 500 == 0)
            fprintf(stderr, "  %d blocks...\r", blocks_tested);
    }

    fclose(f);

    printf("=== Circumcircle Triangle Compression ===\n");
    printf("Model: %s\n", filename);
    printf("Blocks: %d\n\n", blocks_tested);

    printf("Q8_0 baseline: 34 bytes/block\n\n");

    printf("%-8s %10s %8s %10s %10s\n",
           "Bits", "Bytes", "Ratio", "AvgErr%", "MaxErr%");
    printf("%-8s %10s %8s %10s %10s\n",
           "----", "-----", "-----", "-------", "-------");

    for (int b = 0; b < n_bit_configs; b++) {
        if (block_count[b] == 0) continue;
        float avg_bytes = (float)total_bytes[b] / block_count[b];
        float avg_err = total_error[b] / block_count[b];
        float avg_max = total_max_error[b] / block_count[b];
        float ratio = avg_bytes / 34.0f;

        printf("%-8s %9.1fB %7.2fx %9.2f%% %9.2f%%\n",
               bit_names[b], avg_bytes, ratio, avg_err, avg_max);
    }

    /* Compare with Q8_0 */
    printf("\n--- vs Q8_0 ---\n");
    for (int b = 0; b < n_bit_configs; b++) {
        if (block_count[b] == 0) continue;
        float avg_bytes = (float)total_bytes[b] / block_count[b];
        float avg_err = total_error[b] / block_count[b];
        float ratio = avg_bytes / 34.0f;

        if (ratio < 1.0 && avg_err < 2.0)
            printf("%s: SMALLER + low error → worth it\n", bit_names[b]);
    }

    return 0;
}

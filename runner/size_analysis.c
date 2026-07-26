/* ============================================================
 * size_analysis.c — Storage size after circle packing
 *
 * Compare: Q8_0 original vs circle packing representation
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
    int n_centroids;
    int delta_bits;      /* bits per delta */
    int storage_bytes;   /* total storage */
    float avg_delta_pct; /* fit quality */
} SizeResult;

static SizeResult test_config(float *weights, int n, int n_centroids) {
    SizeResult r;
    r.n_centroids = n_centroids;

    float sorted[Q8_BLOCK_SZ];
    for (int i = 0; i < n; i++) sorted[i] = weights[i];
    sort_floats(sorted, n);

    float w_min = sorted[0], w_max = sorted[n-1];
    float range = w_max - w_min;
    if (range < 1e-10f) range = 1.0f;

    /* Place centroids at sorted positions */
    float centroids[48];
    for (int i = 0; i < n_centroids && i < 48; i++) {
        int idx = (i * n) / n_centroids;
        if (idx >= n) idx = n - 1;
        centroids[i] = sorted[idx];
    }

    /* Find max delta for quantization */
    float max_delta = 0;
    float sum_delta = 0;
    for (int i = 0; i < n; i++) {
        float min_d = fabsf(weights[i] - centroids[0]);
        for (int c = 1; c < n_centroids; c++) {
            float d = fabsf(weights[i] - centroids[c]);
            if (d < min_d) min_d = d;
        }
        sum_delta += min_d;
        if (min_d > max_delta) max_delta = min_d;
    }
    float avg_delta = sum_delta / n;
    r.avg_delta_pct = 100.0f * avg_delta / range;

    /* Determine bits needed for delta quantization */
    /* Options: 8 bits (uint8), 6 bits, 4 bits, 2 bits */
    int bits = 8;
    if (max_delta / range < 0.25f) bits = 2;
    else if (max_delta / range < 0.0625f) bits = 4;
    else if (max_delta / range < 0.015625f) bits = 6;

    r.delta_bits = bits;

    /* Storage calculation:
     * Centroids: n_centroids × fp16 (2 bytes each) = 2N bytes
     * Scale: fp16 (2 bytes)
     * Range: fp16 (2 bytes) — needed to dequantize deltas
     * Deltas: 32 × bits / 8 bytes
     */
    int centroid_bytes = n_centroids * 2;  /* fp16 */
    int meta_bytes = 4;                     /* scale + range (fp16 each) */
    int delta_bytes = (32 * bits + 7) / 8; /* round up to bytes */
    r.storage_bytes = centroid_bytes + meta_bytes + delta_bytes;

    return r;
}

int main(int argc, char **argv) {
    const char *fn = argv[1];
    FILE *f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", fn); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    fseek(f, 4096, SEEK_SET);

    int configs[] = {7, 12, 19, 24, 36, 48};
    const char *names[] = {"Seed7", "Icosa12", "Hex19", "Vert24", "Full36", "Metatron48"};
    int n_configs = 6;

    /* Accumulate results */
    long *total_bytes = (long *)calloc(n_configs, sizeof(long));
    long *total_blocks = (long *)calloc(n_configs, sizeof(long));
    float *total_delta = (float *)calloc(n_configs, sizeof(float));

    int blocks_tested = 0;
    int consecutive = 0;
    uint8_t buf[34];

    while (blocks_tested < 2000) {
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
            SizeResult sr = test_config(weights, Q8_BLOCK_SZ, configs[c]);
            total_bytes[c] += sr.storage_bytes;
            total_delta[c] += sr.avg_delta_pct;
            total_blocks[c]++;
        }
        blocks_tested++;
    }
    fclose(f);

    printf("=== Size Analysis: Circle Packing vs Q8_0 ===\n");
    printf("Model: %s\n", fn);
    printf("Blocks tested: %d\n\n", blocks_tested);

    printf("Q8_0 original: 34 bytes/block (32 × int8 + 2 × fp16 scale)\n\n");

    printf("%-12s %3s %6s %8s %10s %8s\n",
           "Config", "N", "Bytes", "Δ%", "Ratio", "Δeff");
    printf("%-12s %3s %6s %8s %10s %8s\n",
           "------------", "---", "------", "--------", "----------", "--------");

    for (int c = 0; c < n_configs; c++) {
        if (total_blocks[c] == 0) continue;
        float avg_bytes = (float)total_bytes[c] / total_blocks[c];
        float avg_delta = total_delta[c] / total_blocks[c];
        float ratio = avg_bytes / 34.0f;
        float efficiency = avg_delta / avg_bytes;

        printf("%-12s %3d %6.1f %7.2f%% %9.2fx %8.4f\n",
               names[c], configs[c], avg_bytes, avg_delta, ratio, efficiency);
    }

    /* Optimal: what's the minimum storage for <1% delta? */
    printf("\n--- Sweet Spot ---\n");
    for (int c = 0; c < n_configs; c++) {
        if (total_blocks[c] == 0) continue;
        float avg_bytes = (float)total_bytes[c] / total_blocks[c];
        float avg_delta = total_delta[c] / total_blocks[c];
        if (avg_delta < 1.0f) {
            printf("%s: %.1f bytes/block, %.2f%% delta, %.1fx vs Q8_0\n",
                   names[c], avg_bytes, avg_delta, avg_bytes / 34.0f);
        }
    }

    free(total_bytes); free(total_blocks); free(total_delta);
    return 0;
}

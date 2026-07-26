/* ============================================================
 * pipeline_test.c — Test if tools work together
 *
 * Tests:
 * 1. Q8_0 alone (baseline)
 * 2. Q8_0 → circle packing (Seed7/Vert24)
 * 3. Q8_0 → beam_value encoding → circle packing
 * 4. Q8_0 → geo_frame_seek mapping → circle packing
 *
 * Measures: size, error, speed
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define Q8_BLOCK_SZ 32
#define Q8_BLOCK_BYTES 34

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
 * Beam Value Encoding (from beam_value.c)
 * Q8 weight → 8-bit BeamCode
 * IMPORTANT: must work on RAW int8, not dequantized float
 * ============================================================ */
static uint8_t weight_to_beamcode(int8_t raw_q8) {
    /* Q8 is already 8-bit — just return as uint8 */
    return (uint8_t)(raw_q8 + 128);
}

static int8_t beamcode_to_weight(uint8_t code) {
    return (int8_t)(code - 128);
}

/* ============================================================
 * Geo Frame Seek Mapping (from geo_frame_seek.h)
 * Simple: data position → deterministic enc (0-1439)
 * ============================================================ */
static uint16_t data_to_frame_seek(int data_index) {
    /* enc(t) = (t × 37) % 1440 — stride-37 walk */
    return (uint16_t)((data_index * 37) % 1440);
}

/* ============================================================
 * Circle Packing Fit
 * ============================================================ */
typedef struct {
    float avg_delta_pct;
    float max_delta_pct;
    int exact_match;
    int total;
} CircleFit;

static CircleFit test_circle_packing(float *weights, int n, int n_centroids) {
    CircleFit fit = {0, 0, 0, n};

    float sorted[Q8_BLOCK_SZ];
    for (int i = 0; i < n; i++) sorted[i] = weights[i];
    sort_floats(sorted, n);

    float w_min = sorted[0], w_max = sorted[n-1];
    float range = w_max - w_min;
    if (range < 1e-10f) return fit;

    /* Place centroids */
    float centroids[48];
    for (int i = 0; i < n_centroids && i < 48; i++) {
        int idx = (i * n) / n_centroids;
        if (idx >= n) idx = n - 1;
        centroids[i] = sorted[idx];
    }

    /* Measure fit */
    float sum_delta = 0, max_delta = 0;
    for (int i = 0; i < n; i++) {
        float min_d = fabsf(weights[i] - centroids[0]);
        for (int c = 1; c < n_centroids; c++) {
            float d = fabsf(weights[i] - centroids[c]);
            if (d < min_d) min_d = d;
        }
        sum_delta += min_d;
        if (min_d > max_delta) max_delta = min_d;
        if (min_d < 1e-7f) fit.exact_match++;
    }

    fit.avg_delta_pct = 100.0f * (sum_delta / n) / range;
    fit.max_delta_pct = 100.0f * max_delta / range;
    return fit;
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
    int max_blocks = (argc > 2) ? atoi(argv[2]) : 500;

    FILE *f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", filename); return 1; }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    fseek(f, 4096, SEEK_SET);

    fprintf(stderr, "File: %s (%.1f MB)\n", filename, file_size / 1048576.0f);

    /* Results accumulators */
    int blocks_tested = 0;
    int consecutive = 0;

    /* Size counters */
    long size_q8 = 0;        /* Q8_0 baseline */
    long size_beam = 0;      /* beam_value encoding */
    long size_seed7 = 0;     /* circle packing Seed7 */
    long size_vert24 = 0;    /* circle packing Vert24 */
    long size_beam_seed7 = 0;/* beam + Seed7 */
    long size_beam_vert24 = 0;/* beam + Vert24 */

    /* Error counters */
    float err_seed7 = 0, err_vert24 = 0;
    float err_beam_seed7 = 0, err_beam_vert24 = 0;
    float err_beam = 0;

    /* Timing */
    clock_t start = clock();

    uint8_t buf[Q8_BLOCK_BYTES];

    while (blocks_tested < max_blocks) {
        if (fread(buf, 1, Q8_BLOCK_BYTES, f) != Q8_BLOCK_BYTES) break;

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

        /* === 1. Q8_0 baseline === */
        size_q8 += Q8_BLOCK_BYTES;

        /* === 2. Beam Value encoding (on raw int8, not dequantized) === */
        uint8_t beam_codes[Q8_BLOCK_SZ];
        float beam_weights[Q8_BLOCK_SZ];
        float beam_sum_delta = 0;
        for (int i = 0; i < Q8_BLOCK_SZ; i++) {
            beam_codes[i] = weight_to_beamcode((int8_t)buf[i]);
            /* Reconstruct: beamcode → int8 → dequant */
            int8_t raw = beamcode_to_weight(beam_codes[i]);
            beam_weights[i] = q8_dequant(raw, sc16);
            beam_sum_delta += fabsf(weights[i] - beam_weights[i]);
        }
        size_beam += Q8_BLOCK_SZ; /* 1 byte per weight */
        err_beam += beam_sum_delta / Q8_BLOCK_SZ;

        /* === 3. Circle packing on original weights === */
        CircleFit fit7 = test_circle_packing(weights, Q8_BLOCK_SZ, 7);
        CircleFit fit24 = test_circle_packing(weights, Q8_BLOCK_SZ, 24);

        /* Size: centroids (fp16) + deltas (uint8) */
        size_seed7 += 7 * 2 + 4 + Q8_BLOCK_SZ; /* 7 centroids fp16 + scale + deltas */
        size_vert24 += 24 * 2 + 4 + Q8_BLOCK_SZ;
        err_seed7 += fit7.avg_delta_pct;
        err_vert24 += fit24.avg_delta_pct;

        /* === 4. Circle packing on beam-encoded weights === */
        CircleFit fit7b = test_circle_packing(beam_weights, Q8_BLOCK_SZ, 7);
        CircleFit fit24b = test_circle_packing(beam_weights, Q8_BLOCK_SZ, 24);

        size_beam_seed7 += 7 * 2 + 4 + Q8_BLOCK_SZ;
        size_beam_vert24 += 24 * 2 + 4 + Q8_BLOCK_SZ;
        err_beam_seed7 += fit7b.avg_delta_pct;
        err_beam_vert24 += fit24b.avg_delta_pct;

        blocks_tested++;
        if (blocks_tested % 100 == 0)
            fprintf(stderr, "  %d blocks...\r", blocks_tested);
    }

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    fclose(f);

    printf("=== Pipeline Integration Test ===\n");
    printf("Model: %s\n", filename);
    printf("Blocks: %d\n", blocks_tested);
    printf("Time: %.3f sec (%.0f blocks/sec)\n\n", elapsed, blocks_tested / elapsed);

    printf("Method                     Size/block  Ratio   AvgΔ%%   vs Q8\n");
    printf("-------------------------  ----------  ------  ------  -----\n");

    double avg_q8 = (double)size_q8 / blocks_tested;
    double avg_beam = (double)size_beam / blocks_tested;
    double avg_s7 = (double)size_seed7 / blocks_tested;
    double avg_v24 = (double)size_vert24 / blocks_tested;
    double avg_bs7 = (double)size_beam_seed7 / blocks_tested;
    double avg_bv24 = (double)size_beam_vert24 / blocks_tested;

    printf("Q8_0 baseline              %6.1f B    1.00x   —      —\n", avg_q8);
    printf("BeamCode only              %6.1f B    %.2fx  %.2f%%  %s\n",
           avg_beam, avg_beam/avg_q8, err_beam/blocks_tested*100,
           avg_beam < avg_q8 ? "✓" : "✗");
    printf("Seed7 (1+6)                %6.1f B    %.2fx  %.2f%%  %s\n",
           avg_s7, avg_s7/avg_q8, err_seed7/blocks_tested,
           avg_s7 < avg_q8 ? "✓" : "✗");
    printf("Vert24                     %6.1f B    %.2fx  %.2f%%  %s\n",
           avg_v24, avg_v24/avg_q8, err_vert24/blocks_tested,
           avg_v24 < avg_q8 ? "✓" : "✗");
    printf("BeamCode + Seed7           %6.1f B    %.2fx  %.2f%%  %s\n",
           avg_bs7, avg_bs7/avg_q8, err_beam_seed7/blocks_tested,
           avg_bs7 < avg_q8 ? "✓" : "✗");
    printf("BeamCode + Vert24          %6.1f B    %.2fx  %.2f%%  %s\n",
           avg_bv24, avg_bv24/avg_q8, err_beam_vert24/blocks_tested,
           avg_bv24 < avg_q8 ? "✓" : "✗");

    /* Verdict */
    printf("\n--- Verdict ---\n");
    if (avg_s7 < avg_q8 && err_seed7/blocks_tested < 2.0)
        printf("Seed7: SMALLER + acceptable error → worth integrating\n");
    else
        printf("Seed7: NOT worth integrating (bigger or too much error)\n");

    if (avg_v24 < avg_q8 && err_vert24/blocks_tested < 1.0)
        printf("Vert24: SMALLER + low error → worth integrating\n");
    else
        printf("Vert24: NOT worth integrating\n");

    if (avg_bs7 < avg_s7)
        printf("BeamCode+Seed7: BETTER than Seed7 alone → beam helps\n");
    else
        printf("BeamCode+Seed7: WORSE than Seed7 alone → beam doesn't help\n");

    return 0;
}

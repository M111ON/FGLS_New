/* ============================================================
 * shape_performance_test.c — Benchmark different circle configs
 *
 * Tests: how well do different geometric arrangements fit
 * real GGUF Q8_0 weight data?
 *
 * Configs:
 *   7   = Seed of Life (1 center + 6 at 60°)
 *   12  = Icosahedral vertices
 *   19  = Hex-19 cluster (1+6+12)
 *   24  = Vertices + edge midpoints
 *   36  = Full icosahedral symmetry
 *   48  = GEO_BLOCK (Metatron: 4×4×3)
 *
 * Compile: gcc -O2 -o shape_perf shape_performance_test.c -lm
 * Usage:   ./shape_perf <model.gguf> [max_blocks]
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define MAX_CIRCLES 64
#define Q8_BLOCK_SZ 32
#define Q8_BLOCK_BYTES 34  /* 32 int8 + 2 fp16 scale */

/* ============================================================
 * fp16 → float (same as scan_gguf_weights.c)
 * ============================================================ */
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
 * Circle Arrangement Types
 * ============================================================ */
typedef enum {
    ARR_SEED7,        /* 7: 1 center + 6 at 60° */
    ARR_ICOSA12,      /* 12: icosahedral vertices */
    ARR_HEX19,        /* 19: 1+6+12 (hex cluster) */
    ARR_VERT24,       /* 24: vertices + edge midpoints */
    ARR_FULL36,       /* 36: full icosahedral symmetry */
    ARR_METATRON48,   /* 48: GEO_BLOCK */
    ARR_RANDOM7,      /* 7: random (baseline) */
    ARR_RANDOM12,     /* 12: random (baseline) */
    ARR_LINEAR7,      /* 7: evenly spaced (baseline) */
    ARR_LINEAR12,     /* 12: evenly spaced (baseline) */
    ARR_COUNT
} ArrangementType;

static const char *arr_names[] = {
    "Seed7(1+6@60°)", "Icosa12", "Hex19(1+6+12)",
    "Vert24", "Full36", "Metatron48",
    "Random7", "Random12", "Linear7", "Linear12"
};

/* ============================================================
 * Sort
 * ============================================================ */
static void sort_floats(float *arr, int n) {
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (arr[i] > arr[j]) { float t = arr[i]; arr[i] = arr[j]; arr[j] = t; }
}

/* ============================================================
 * Generate circle centroids in sorted weight space
 * ============================================================ */
static void generate_centroids_sorted(float *centroids, int n,
                                       float *sorted, int n_sorted,
                                       ArrangementType arr) {
    /* All configs use sorted weights to place centroids at
     * geometrically meaningful positions */

    float w_min = sorted[0];
    float w_max = sorted[n_sorted - 1];
    float median = sorted[n_sorted / 2];

    memset(centroids, 0, n * sizeof(float));

    switch (arr) {
    case ARR_SEED7: {
        /* 1 center (median) + 6 outer at 60° intervals
         * mapped to sorted weight positions */
        centroids[0] = median;
        for (int i = 1; i < 7 && i < n; i++) {
            int idx = (i * n_sorted) / 7;
            if (idx >= n_sorted) idx = n_sorted - 1;
            centroids[i] = sorted[idx];
        }
        break;
    }
    case ARR_ICOSA12: {
        /* 12: 5-fold symmetry (upper ring 5, lower ring 5, north, south) */
        centroids[0] = w_max; /* north pole */
        centroids[11] = w_min; /* south pole */
        for (int i = 1; i <= 5 && i < n; i++) {
            int idx = (i * n_sorted) / 6;
            if (idx >= n_sorted) idx = n_sorted - 1;
            centroids[i] = sorted[idx];
        }
        for (int i = 6; i <= 10 && i < n; i++) {
            int idx = ((i - 5) * n_sorted) / 6;
            if (idx >= n_sorted) idx = n_sorted - 1;
            centroids[i] = sorted[idx];
        }
        break;
    }
    case ARR_HEX19: {
        /* 1+6+12 = 19 (hex cluster rings) */
        centroids[0] = median;
        for (int i = 1; i <= 6 && i < n; i++) {
            int idx = (i * n_sorted) / 7;
            if (idx >= n_sorted) idx = n_sorted - 1;
            centroids[i] = sorted[idx];
        }
        for (int i = 7; i <= 18 && i < n; i++) {
            int idx = ((i - 6) * n_sorted) / 13;
            if (idx >= n_sorted) idx = n_sorted - 1;
            centroids[i] = sorted[idx];
        }
        break;
    }
    case ARR_VERT24: {
        /* 24: evenly distributed in sorted space */
        for (int i = 0; i < n && i < 24; i++) {
            int idx = (i * n_sorted) / 24;
            if (idx >= n_sorted) idx = n_sorted - 1;
            centroids[i] = sorted[idx];
        }
        break;
    }
    case ARR_FULL36: {
        /* 36: 3 rings of 12 */
        for (int ring = 0; ring < 3; ring++) {
            for (int i = 0; i < 12; i++) {
                int idx = (ring * n_sorted + i * n_sorted / 3) / 12;
                if (idx >= n_sorted) idx = n_sorted - 1;
                int ci = ring * 12 + i;
                if (ci < n) centroids[ci] = sorted[idx];
            }
        }
        break;
    }
    case ARR_METATRON48: {
        /* 48: 4 layers of 12 */
        for (int layer = 0; layer < 4; layer++) {
            for (int i = 0; i < 12; i++) {
                int idx = (layer * n_sorted / 4 + i * n_sorted / 48);
                if (idx >= n_sorted) idx = n_sorted - 1;
                int ci = layer * 12 + i;
                if (ci < n) centroids[ci] = sorted[idx];
            }
        }
        break;
    }
    case ARR_RANDOM7:
    case ARR_RANDOM12: {
        unsigned int seed = 42;
        for (int i = 0; i < n; i++) {
            seed = seed * 1103515245 + 12345;
            int idx = (int)((float)(seed & 0x7FFFFFFF) / (float)0x7FFFFFFF * (n_sorted - 1));
            if (idx >= n_sorted) idx = n_sorted - 1;
            centroids[i] = sorted[idx];
        }
        break;
    }
    case ARR_LINEAR7:
    case ARR_LINEAR12: {
        for (int i = 0; i < n; i++) {
            int idx = (i * n_sorted) / n;
            if (idx >= n_sorted) idx = n_sorted - 1;
            centroids[i] = sorted[idx];
        }
        break;
    }
    default:
        centroids[0] = median;
        break;
    }
}

/* ============================================================
 * K-means optimization on 1D
 * ============================================================ */
static void optimize_centroids_1d(float *centroids, int nc,
                                   float *weights, int nw, int iters) {
    for (int it = 0; it < iters; it++) {
        float *sum = (float *)calloc(nc, sizeof(float));
        int *cnt = (int *)calloc(nc, sizeof(int));

        for (int i = 0; i < nw; i++) {
            float best_d = fabsf(weights[i] - centroids[0]);
            int best_c = 0;
            for (int c = 1; c < nc; c++) {
                float d = fabsf(weights[i] - centroids[c]);
                if (d < best_d) { best_d = d; best_c = c; }
            }
            sum[best_c] += weights[i];
            cnt[best_c]++;
        }

        for (int c = 0; c < nc; c++) {
            if (cnt[c] > 0) centroids[c] = sum[c] / cnt[c];
        }

        free(sum);
        free(cnt);
    }
}

/* ============================================================
 * Fit measurement
 * ============================================================ */
typedef struct {
    float avg_delta;
    float max_delta;
    float psnr;
    int   exact_match;
    int   n_weights;
} FitResult;

static FitResult measure_fit(float *weights, int n, float *centroids, int nc) {
    FitResult r;
    memset(&r, 0, sizeof(r));
    r.n_weights = n;

    float w_min = weights[0], w_max = weights[0];
    for (int i = 1; i < n; i++) {
        if (weights[i] < w_min) w_min = weights[i];
        if (weights[i] > w_max) w_max = weights[i];
    }
    float signal = w_max - w_min;
    if (signal < 1e-10f) signal = 1.0f;

    float sum_sq = 0;
    for (int i = 0; i < n; i++) {
        float min_d = fabsf(weights[i] - centroids[0]);
        for (int c = 1; c < nc; c++) {
            float d = fabsf(weights[i] - centroids[c]);
            if (d < min_d) min_d = d;
        }
        /* Store RELATIVE delta (fraction of range) */
        r.avg_delta += min_d / signal;
        if (min_d / signal > r.max_delta) r.max_delta = min_d / signal;
        if (min_d < 1e-7f) r.exact_match++;
        sum_sq += (min_d / signal) * (min_d / signal);
    }
    r.avg_delta /= n;

    float mse = sum_sq / n;
    if (mse > 1e-10f) r.psnr = -10.0f * log10f(mse);
    else r.psnr = 999.0f;

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
    int max_blocks = (argc > 2) ? atoi(argv[2]) : 300;

    FILE *f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", filename); return 1; }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    fprintf(stderr, "File: %s (%.1f MB)\n", filename, file_size / 1048576.0f);

    /* Skip GGUF header (first 4KB minimum, then scan for tensor regions) */
    long scan_start = 4096;
    fseek(f, scan_start, SEEK_SET);

    /* Configs to test */
    int configs[] = {7, 12, 19, 24, 36, 48, 7, 12, 7, 12};
    ArrangementType arrs[] = {
        ARR_SEED7, ARR_ICOSA12, ARR_HEX19,
        ARR_VERT24, ARR_FULL36, ARR_METATRON48,
        ARR_RANDOM7, ARR_RANDOM12, ARR_LINEAR7, ARR_LINEAR12
    };
    int total_configs = 10;

    float *sum_avg = (float *)calloc(total_configs, sizeof(float));
    float *sum_max = (float *)calloc(total_configs, sizeof(float));
    float *sum_psnr = (float *)calloc(total_configs, sizeof(float));
    int *sum_exact = (int *)calloc(total_configs, sizeof(int));
    int *sum_nw = (int *)calloc(total_configs, sizeof(int));
    long *block_cnt = (long *)calloc(total_configs, sizeof(long));

    int blocks_tested = 0;
    int consecutive_valid = 0;

    fprintf(stderr, "Scanning for Q8_0 tensor blocks (max %d)...\n", max_blocks);

    uint8_t block_buf[Q8_BLOCK_BYTES];

    while (blocks_tested < max_blocks) {
        if (fread(block_buf, 1, Q8_BLOCK_BYTES, f) != Q8_BLOCK_BYTES) break;

        /* Validate fp16 scale */
        uint16_t sc16;
        memcpy(&sc16, block_buf + 32, 2);
        int exp = (sc16 >> 10) & 0x1f;
        int valid = (sc16 != 0 && sc16 != 0x7c00 && sc16 != 0xfc00 &&
                     exp > 0 && exp < 30);

        if (valid) {
            consecutive_valid++;
            /* Only process if in a tensor region (4+ consecutive valid blocks) */
            if (consecutive_valid < 4) continue;
        } else {
            if (consecutive_valid >= 4) {
                /* End of tensor region — skip */
            }
            consecutive_valid = 0;
            continue;
        }

        /* Dequantize Q8 block */
        float weights[Q8_BLOCK_SZ];
        for (int i = 0; i < Q8_BLOCK_SZ; i++) {
            weights[i] = q8_dequant((int8_t)block_buf[i], sc16);
        }

        /* Skip constant blocks */
        int all_same = 1;
        for (int i = 1; i < Q8_BLOCK_SZ; i++) {
            if (weights[i] != weights[0]) { all_same = 0; break; }
        }
        if (all_same) continue;

        /* Skip blocks with extreme range (>1M) — these are outliers */
        {
            float wmin = weights[0], wmax = weights[0];
            for (int i = 1; i < Q8_BLOCK_SZ; i++) {
                if (weights[i] < wmin) wmin = weights[i];
                if (weights[i] > wmax) wmax = weights[i];
            }
            if (wmax - wmin > 1e6f) continue;
        }

        /* Sort weights for centroid placement */
        float sorted[Q8_BLOCK_SZ];
        for (int i = 0; i < Q8_BLOCK_SZ; i++) sorted[i] = weights[i];
        sort_floats(sorted, Q8_BLOCK_SZ);

        /* Test each configuration */
        for (int c = 0; c < total_configs; c++) {
            int nc = configs[c];
            float centroids[MAX_CIRCLES];

            /* Generate initial centroids from sorted weights */
            generate_centroids_sorted(centroids, nc, sorted, Q8_BLOCK_SZ, arrs[c]);

            /* Optimize with K-means (5 iterations) */
            optimize_centroids_1d(centroids, nc, weights, Q8_BLOCK_SZ, 5);

            /* Measure fit */
            FitResult fr = measure_fit(weights, Q8_BLOCK_SZ, centroids, nc);

            sum_avg[c] += fr.avg_delta;
            sum_max[c] += fr.max_delta;
            sum_psnr[c] += fr.psnr;
            sum_exact[c] += fr.exact_match;
            sum_nw[c] += fr.n_weights;
            block_cnt[c]++;
        }

        blocks_tested++;
        if (blocks_tested % 50 == 0)
            fprintf(stderr, "  %d blocks...\r", blocks_tested);
    }

    fclose(f);

    fprintf(stderr, "\nTested %d Q8_0 blocks\n\n", blocks_tested);

    /* Print results */
    printf("=== Shape Performance Test ===\n");
    printf("Model: %s\n", filename);
    printf("Blocks: %d\n\n", blocks_tested);

    printf("%-18s %3s %8s %8s %7s %6s\n",
           "Config", "N", "AvgΔ%", "MaxΔ%", "PSNR", "Exact%");
    printf("%-18s %3s %8s %8s %7s %6s\n",
           "------------------", "---", "--------", "--------", "-------", "------");

    for (int c = 0; c < total_configs; c++) {
        if (block_cnt[c] == 0) continue;
        float avg_d = sum_avg[c] / block_cnt[c] * 100.0f; /* to percent */
        float max_d = sum_max[c] / block_cnt[c] * 100.0f;
        float psnr = sum_psnr[c] / block_cnt[c];
        float exact = 100.0f * sum_exact[c] / sum_nw[c];

        printf("%-18s %3d %7.2f%% %7.2f%% %7.1f %5.1f%%\n",
               arr_names[arrs[c]], configs[c], avg_d, max_d, psnr, exact);
    }

    /* Winner */
    int best = 0;
    float best_d = 1e30f;
    for (int c = 0; c < 6; c++) {
        if (block_cnt[c] == 0) continue;
        float d = sum_avg[c] / block_cnt[c];
        if (d < best_d) { best_d = d; best = c; }
    }

    printf("\n--- Winner ---\n");
    printf("Best: %s (N=%d, avg_delta=%.4f)\n",
           arr_names[arrs[best]], configs[best], best_d);

    /* Efficiency: delta per circle */
    printf("\n--- Efficiency (Δ / N) ---\n");
    printf("%-18s %3s %10s\n", "Config", "N", "Δ/N");
    printf("%-18s %3s %10s\n", "------------------", "---", "----------");
    for (int c = 0; c < 6; c++) {
        if (block_cnt[c] == 0) continue;
        float d = sum_avg[c] / block_cnt[c];
        printf("%-18s %3d %10.6f\n", arr_names[arrs[c]], configs[c], d / configs[c]);
    }

    free(sum_avg); free(sum_max); free(sum_psnr);
    free(sum_exact); free(sum_nw); free(block_cnt);
    return 0;
}

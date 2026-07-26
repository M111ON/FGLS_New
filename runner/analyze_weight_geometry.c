#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "dramtile_store.h"

/* Circle packing analysis for weight compression */
typedef struct {
    float center;
    float radius;
    float centroids[7];
    float deltas[16];
    float max_delta;
    float avg_delta;
    int n_circles;
} CirclePacking;

/* Sort float array ascending */
static void sort_floats(float *arr, int n) {
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (arr[i] > arr[j]) { float t = arr[i]; arr[i] = arr[j]; arr[j] = t; }
}

/* Analyze weights as circle packing */
CirclePacking analyze_circle_packing(float *weights, int n) {
    CirclePacking cp;
    memset(&cp, 0, sizeof(cp));
    
    if (n < 7) return cp;
    
    /* Sort weights */
    float sorted[64];
    for (int i = 0; i < n; i++) sorted[i] = weights[i];
    sort_floats(sorted, n);
    
    /* 7 circles: 1 center + 6 around */
    /* Center = median (sorted[n/2]) */
    cp.center = sorted[n/2];
    
    /* 6 outer circles = sorted positions at 60° intervals */
    cp.n_circles = 7;
    cp.centroids[0] = cp.center;
    for (int i = 1; i < 7; i++) {
        int idx = (i * n) / 7;
        if (idx >= n) idx = n-1;
        cp.centroids[i] = sorted[idx];
    }
    
    /* Radius = average distance from center */
    float sum_dist = 0;
    for (int i = 1; i < 7; i++) {
        sum_dist += fabs(cp.centroids[i] - cp.center);
    }
    cp.radius = sum_dist / 6.0f;
    
    /* Calculate deltas from circle model */
    cp.max_delta = 0;
    cp.avg_delta = 0;
    for (int i = 0; i < n; i++) {
        /* Find closest circle */
        float min_dist = fabs(weights[i] - cp.centroids[0]);
        for (int c = 1; c < 7; c++) {
            float d = fabs(weights[i] - cp.centroids[c]);
            if (d < min_dist) min_dist = d;
        }
        cp.deltas[i] = min_dist;
        cp.avg_delta += min_dist;
        if (min_dist > cp.max_delta) cp.max_delta = min_dist;
    }
    cp.avg_delta /= n;
    
    return cp;
}

/* Analyze Gosper curve traversal pattern */
void analyze_gosper_pattern(float *weights, int n) {
    fprintf(stderr, "\n=== Gosper Curve Pattern Analysis ===\n");
    
    /* Check if adjacent weights (by value) have correlation */
    float sorted[64];
    for (int i = 0; i < n; i++) sorted[i] = weights[i];
    sort_floats(sorted, n);
    
    /* Calculate autocorrelation at lag 1 (adjacent in sorted order) */
    float sum_sq = 0, sum_cross = 0;
    for (int i = 0; i < n-1; i++) {
        float diff = sorted[i+1] - sorted[i];
        sum_sq += diff * diff;
        sum_cross += sorted[i] * sorted[i+1];
    }
    
    float correlation = sum_cross / (sum_sq + 1e-10f);
    fprintf(stderr, "Sorted autocorrelation (lag 1): %.4f\n", correlation);
    fprintf(stderr, "Variance of sorted differences: %.6f\n", sum_sq / (n-1));
}

/* Analyze Fibonacci decomposition */
void analyze_fibonacci(float *weights, int n) {
    fprintf(stderr, "\n=== Fibonacci Decomposition Analysis ===\n");
    
    /* Fibonacci levels: 1, 2, 3, 5, 8, 13, 21... */
    int fibs[] = {1, 2, 3, 5, 8, 13, 21, 34};
    int nfibs = 8;
    
    float total_range = 0;
    for (int i = 0; i < n; i++) {
        float min_val = weights[0], max_val = weights[0];
        for (int j = 1; j < n; j++) {
            if (weights[j] < min_val) min_val = weights[j];
            if (weights[j] > max_val) max_val = weights[j];
        }
        total_range = max_val - min_val;
    }
    
    fprintf(stderr, "Total range: %.6f\n", total_range);
    
    /* Check if weights cluster at Fibonacci ratios */
    for (int f = 0; f < nfibs; f++) {
        int level = fibs[f];
        if (level > n) break;
        
        float level_range = total_range / level;
        int count_in_level = 0;
        
        for (int i = 0; i < n; i++) {
            int bucket = (int)((weights[i] - weights[0]) / level_range);
            if (bucket >= 0 && bucket < level) count_in_level++;
        }
        
        fprintf(stderr, "Level %2d: %d/%d weights in %d buckets (%.1f%%)\n",
                level, count_in_level, n, level, 100.0f * count_in_level / n);
    }
}

int main(int argc, char **argv) {
    const char *filename = "real_weights.dramtile";
    if (argc > 1) filename = argv[1];
    
    DRamTileStore store;
    memset(&store, 0, sizeof(store));
    
    if (dt_store_init_twin(&store, filename, 1024*1024*1024) != 0) {
        fprintf(stderr, "Failed to open %s\n", filename);
        return 1;
    }
    
    fprintf(stderr, "Loaded: %s (%u tensors, %zu bytes)\n", 
            filename, store.n_stored, store.used);
    
    /* Analyze each tensor */
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store.hash[i].dram_addr == 0) continue;
        if (store.hash[i].dram_addr & 0x80000000u) continue;
        
        uint8_t *data = store.base + store.hash[i].offset;
        int n_floats = store.hash[i].size / sizeof(float);
        if (n_floats < 4 || n_floats > 64) continue;
        
        float *weights = (float *)data;
        
        fprintf(stderr, "\n--- %s (%d floats, %zu bytes) ---\n",
                store.hash[i].name, n_floats, store.hash[i].size);
        
        /* Print raw weights */
        fprintf(stderr, "Weights: ");
        for (int j = 0; j < n_floats && j < 16; j++) {
            fprintf(stderr, "%.4f ", weights[j]);
        }
        fprintf(stderr, "\n");
        
        /* Circle packing analysis */
        CirclePacking cp = analyze_circle_packing(weights, n_floats);
        fprintf(stderr, "Circle Packing:\n");
        fprintf(stderr, "  Center: %.4f\n", cp.center);
        fprintf(stderr, "  Radius: %.4f\n", cp.radius);
        fprintf(stderr, "  Max delta: %.4f (%.1f%% of range)\n", 
                cp.max_delta, 100.0f * cp.max_delta / (cp.radius * 2 + 1e-10f));
        fprintf(stderr, "  Avg delta: %.4f (%.1f%% of range)\n",
                cp.avg_delta, 100.0f * cp.avg_delta / (cp.radius * 2 + 1e-10f));
        
        /* Compression potential */
        float range = cp.max_delta * 2;
        float blueprint_size = 7 * sizeof(float);  /* 7 centroids */
        float data_size = n_floats * sizeof(float);  /* original */
        float delta_size = n_floats * sizeof(uint8_t);  /* quantized deltas */
        float compression = (blueprint_size + delta_size) / data_size;
        
        fprintf(stderr, "  Blueprint: %.0f bytes (7 centroids)\n", blueprint_size);
        fprintf(stderr, "  Data: %.0f bytes (quantized deltas)\n", delta_size);
        fprintf(stderr, "  Total: %.0f bytes (%.1f%% of original)\n",
                blueprint_size + delta_size, 100.0f * compression);
        
        /* Gosper pattern */
        analyze_gosper_pattern(weights, n_floats);
        
        /* Fibonacci pattern */
        analyze_fibonacci(weights, n_floats);
    }
    
    dt_store_destroy_twin(&store);
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <float.h>

/* ============================================================
 * Blueprint Compression: Circle Packing
 * Compress → Decompress → Verify Integrity
 * ============================================================ */

/* Sort float array */
static void sort_floats(float *arr, int n) {
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (arr[i] > arr[j]) { float t = arr[i]; arr[i] = arr[j]; arr[j] = t; }
}

/* Find closest centroid for a weight */
static int find_closest(float val, float *centroids, int n_centroids) {
    int best = 0;
    float best_d = fabs(val - centroids[0]);
    for (int i = 1; i < n_centroids; i++) {
        float d = fabs(val - centroids[i]);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* ============================================================
 * Blueprint Compression Structure
 * ============================================================ */
typedef struct {
    float center;         /* median weight */
    float radius;         /* average distance from center */
    float centroids[7];   /* 7 circle centroids */
    uint8_t deltas[32];   /* quantized deltas (0-255) */
    float delta_scale;    /* scale for deltas: max_delta / 255 */
    float max_delta;      /* maximum delta before quantization */
    float avg_delta;      /* average delta */
    int n_weights;        /* number of weights in block */
    float original[32];   /* original weights (for verification) */
    float reconstructed[32]; /* reconstructed weights */
} BlueprintBlock;

/* Compress 32 weights into blueprint */
BlueprintBlock blueprint_compress(float *weights, int n) {
    BlueprintBlock bp;
    memset(&bp, 0, sizeof(bp));
    bp.n_weights = n;
    
    /* Store original for verification */
    for (int i = 0; i < n; i++) bp.original[i] = weights[i];
    
    /* Sort to find structure */
    float sorted[32];
    for (int i = 0; i < n; i++) sorted[i] = weights[i];
    sort_floats(sorted, n);
    
    /* 7 centroids: 1 center + 6 outer */
    bp.center = sorted[n / 2];
    bp.centroids[0] = bp.center;
    for (int i = 1; i < 7; i++) {
        int idx = (i * n) / 7;
        if (idx >= n) idx = n - 1;
        bp.centroids[i] = sorted[idx];
    }
    
    /* Radius = average distance from center */
    float sum_dist = 0;
    for (int i = 1; i < 7; i++) {
        sum_dist += fabs(bp.centroids[i] - bp.center);
    }
    bp.radius = sum_dist / 6.0f;
    
    /* Find max delta for quantization */
    bp.max_delta = 0;
    bp.avg_delta = 0;
    for (int i = 0; i < n; i++) {
        int c = find_closest(weights[i], bp.centroids, 7);
        float delta = fabs(weights[i] - bp.centroids[c]);
        bp.avg_delta += delta;
        if (delta > bp.max_delta) bp.max_delta = delta;
    }
    bp.avg_delta /= n;
    
    /* Quantize deltas to uint8 */
    bp.delta_scale = bp.max_delta / 255.0f;
    if (bp.delta_scale < 1e-10f) bp.delta_scale = 1e-10f;
    
    for (int i = 0; i < n; i++) {
        int c = find_closest(weights[i], bp.centroids, 7);
        float delta = weights[i] - bp.centroids[c];
        /* Store sign + magnitude */
        float q = fabs(delta) / bp.delta_scale;
        if (q > 255) q = 255;
        bp.deltas[i] = (uint8_t)q;
        /* Store sign in reconstructed */
        bp.reconstructed[i] = bp.centroids[c] + (delta >= 0 ? 1 : -1) * bp.deltas[i] * bp.delta_scale;
    }
    
    return bp;
}

/* Decompress blueprint back to weights */
void blueprint_decompress(BlueprintBlock *bp) {
    for (int i = 0; i < bp->n_weights; i++) {
        /* Find which centroid this weight was closest to */
        int c = find_closest(bp->original[i], bp->centroids, 7);
        float original_delta = bp->original[i] - bp->centroids[c];
        /* Reconstruct using quantized delta with original sign */
        bp->reconstructed[i] = bp->centroids[c] + 
            (original_delta >= 0 ? 1 : -1) * bp->deltas[i] * bp->delta_scale;
    }
}

/* Verify integrity between original and reconstructed */
typedef struct {
    float mse;          /* mean squared error */
    float max_error;    /* maximum absolute error */
    float avg_error;    /* average absolute error */
    float psnr;         /* peak signal-to-noise ratio */
    float relative_err; /* error relative to range */
    int exact_matches;  /* number of exact matches */
} IntegrityResult;

IntegrityResult verify_integrity(BlueprintBlock *bp) {
    IntegrityResult r;
    memset(&r, 0, sizeof(r));
    
    float sum_sq_err = 0;
    float sum_abs_err = 0;
    float max_abs_err = 0;
    float range = 0;
    
    /* Find range of original */
    float min_v = bp->original[0], max_v = bp->original[0];
    for (int i = 1; i < bp->n_weights; i++) {
        if (bp->original[i] < min_v) min_v = bp->original[i];
        if (bp->original[i] > max_v) max_v = bp->original[i];
    }
    range = max_v - min_v;
    if (range < 1e-10f) range = 1e-10f;
    
    for (int i = 0; i < bp->n_weights; i++) {
        float err = fabs(bp->original[i] - bp->reconstructed[i]);
        sum_sq_err += err * err;
        sum_abs_err += err;
        if (err > max_abs_err) max_abs_err = err;
        if (err < 1e-10f) r.exact_matches++;
    }
    
    r.mse = sum_sq_err / bp->n_weights;
    r.max_error = max_abs_err;
    r.avg_error = sum_abs_err / bp->n_weights;
    r.relative_err = 100.0f * r.avg_error / range;
    
    /* PSNR = 10 * log10(max_val^2 / MSE) */
    float max_val_fabs = fabs(max_v) > fabs(min_v) ? fabs(max_v) : fabs(min_v);
    if (r.mse > 1e-10f) {
        r.psnr = 10.0f * log10f(max_val_fabs * max_val_fabs / r.mse);
    } else {
        r.psnr = 999.0f; /* infinite PSNR */
    }
    
    return r;
}

/* ============================================================
 * Analysis across multiple quantization formats
 * ============================================================ */

/* Simulate different Q formats by requantizing */
void simulate_q_format(float *weights, int n, int bits, const char *format_name) {
    /* Simulate requantization: round to N-bit precision */
    float max_val = 0;
    for (int i = 0; i < n; i++) {
        float a = fabs(weights[i]);
        if (a > max_val) max_val = a;
    }
    
    float scale = max_val / ((1 << (bits-1)) - 1);
    if (scale < 1e-10f) scale = 1e-10f;
    
    float requantized[32];
    for (int i = 0; i < n; i++) {
        int q = (int)round(weights[i] / scale);
        int max_q = (1 << (bits-1)) - 1;
        if (q > max_q) q = max_q;
        if (q < -max_q) q = -max_q;
        requantized[i] = q * scale;
    }
    
    /* Compress with blueprint */
    BlueprintBlock bp = blueprint_compress(requantized, n);
    blueprint_decompress(&bp);
    IntegrityResult r = verify_integrity(&bp);
    
    float range = 0;
    float min_v = requantized[0], max_v = requantized[0];
    for (int i = 1; i < n; i++) {
        if (requantized[i] < min_v) min_v = requantized[i];
        if (requantized[i] > max_v) max_v = requantized[i];
    }
    range = max_v - min_v;
    
    /* Compression ratio */
    float original_bytes = n * 4.0f;  /* float32 */
    float blueprint_bytes = 7 * 4.0f + n * 1.0f + 4.0f; /* centroids + deltas + scale */
    float ratio = 100.0f * blueprint_bytes / original_bytes;
    
    fprintf(stderr, "  %s: avgΔ=%.2f%% maxΔ=%.2f%% PSNR=%.1f dB exact=%d/%d compress=%.1f%%\n",
            format_name, r.relative_err, 100.0f * r.max_error / range,
            r.psnr, r.exact_matches, n, ratio);
}

/* ============================================================
 * Main: Test with actual GGUF data
 * ============================================================ */

/* Q8_0 dequant */
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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf> [scan_mb]\n", argv[0]);
        return 1;
    }
    
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }
    
    _fseeki64(f, 0, SEEK_END);
    long long file_size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    
    int scan_mb = argc > 2 ? atoi(argv[2]) : 20;
    long long scan_limit = (long long)scan_mb * 1024 * 1024;
    if (scan_limit > file_size) scan_limit = file_size;
    
    fprintf(stderr, "=== Blueprint Compression Integrity Test ===\n");
    fprintf(stderr, "File: %s (%.1f MB, scan %.1f MB)\n", argv[1],
            file_size / (1024.0*1024.0), scan_limit / (1024.0*1024.0));
    
    /* Scan for Q8_0 blocks */
    fseek(f, 4096, SEEK_SET);
    long long scan_pos = 4096;
    
    int block_count = 0;
    int analyzed = 0;
    int max_blocks = 50;
    
    /* Accumulators for summary */
    float total_mse = 0, total_max_err = 0, total_psnr = 0;
    int total_exact = 0;
    int total_weights = 0;
    
    fprintf(stderr, "\n--- Q8_0 Format (original) ---\n");
    
    uint8_t block[34];
    while (scan_pos < scan_limit && analyzed < max_blocks) {
        if (fread(block, 1, 34, f) != 34) break;
        scan_pos += 34;
        
        uint16_t sc16;
        memcpy(&sc16, block + 32, 2);
        
        int valid = 0;
        if (sc16 != 0 && sc16 != 0x7c00 && sc16 != 0xfc00) {
            int exp = (sc16 >> 10) & 0x1f;
            if (exp > 0 && exp < 30) valid = 1;
        }
        
        if (!valid) continue;
        
        /* Dequantize Q8_0 block */
        float weights[32];
        for (int i = 0; i < 32; i++) {
            weights[i] = q8_dequant((int8_t)block[i], sc16);
        }
        
        /* Check if block has meaningful data */
        float min_v = weights[0], max_v = weights[0];
        for (int i = 1; i < 32; i++) {
            if (weights[i] < min_v) min_v = weights[i];
            if (weights[i] > max_v) max_v = weights[i];
        }
        if (max_v - min_v < 1e-6f) continue;
        
        /* Compress → Decompress → Verify */
        BlueprintBlock bp = blueprint_compress(weights, 32);
        blueprint_decompress(&bp);
        IntegrityResult r = verify_integrity(&bp);
        
        float range = max_v - min_v;
        fprintf(stderr, "  block_%03d: avgΔ=%.2f%% maxΔ=%.2f%% PSNR=%.1f dB exact=%d/32\n",
                analyzed, 100.0f * r.avg_error / range,
                100.0f * r.max_error / range,
                r.psnr, r.exact_matches);
        
        total_mse += r.mse;
        total_max_err += r.max_error;
        total_psnr += r.psnr;
        total_exact += r.exact_matches;
        total_weights += 32;
        analyzed++;
        
        if (block_count++ % 100 == 0) {
            fprintf(stderr, "\r  Scanning %lld / %lld (%.1f%%)...",
                    scan_pos, scan_limit, 100.0 * scan_pos / scan_limit);
        }
    }
    
    /* Summary */
    fprintf(stderr, "\n\n=== Summary ===\n");
    fprintf(stderr, "Blocks analyzed: %d\n", analyzed);
    fprintf(stderr, "Total weights: %d\n", total_weights);
    fprintf(stderr, "Avg MSE: %.6f\n", total_mse / analyzed);
    fprintf(stderr, "Avg max error: %.4f\n", total_max_err / analyzed);
    fprintf(stderr, "Avg PSNR: %.1f dB\n", total_psnr / analyzed);
    fprintf(stderr, "Exact matches: %d / %d (%.1f%%)\n",
            total_exact, total_weights, 100.0f * total_exact / total_weights);
    
    /* Compression analysis */
    fprintf(stderr, "\n=== Compression Analysis ===\n");
    fprintf(stderr, "Original: 32 weights × 4 bytes = 128 bytes\n");
    fprintf(stderr, "Blueprint: 7 centroids × 4 bytes + 32 deltas × 1 byte + scale × 4 bytes = 64 bytes\n");
    fprintf(stderr, "Compression ratio: %.1f%%\n", 100.0f * 64 / 128);
    
    /* Test with different quantization formats */
    fprintf(stderr, "\n--- Simulated Quantization Formats ---\n");
    /* Re-read and test with simulated formats */
    fseek(f, 4096, SEEK_SET);
    scan_pos = 4096;
    analyzed = 0;
    block_count = 0;
    
    while (scan_pos < scan_limit && analyzed < 10) {
        if (fread(block, 1, 34, f) != 34) break;
        scan_pos += 34;
        
        uint16_t sc16;
        memcpy(&sc16, block + 32, 2);
        
        int valid = 0;
        if (sc16 != 0 && sc16 != 0x7c00 && sc16 != 0xfc00) {
            int exp = (sc16 >> 10) & 0x1f;
            if (exp > 0 && exp < 30) valid = 1;
        }
        
        if (!valid) continue;
        
        float weights[32];
        for (int i = 0; i < 32; i++) {
            weights[i] = q8_dequant((int8_t)block[i], sc16);
        }
        
        float min_v = weights[0], max_v = weights[0];
        for (int i = 1; i < 32; i++) {
            if (weights[i] < min_v) min_v = weights[i];
            if (weights[i] > max_v) max_v = weights[i];
        }
        if (max_v - min_v < 1e-6f) continue;
        
        fprintf(stderr, "\n[Block %d] Original range: [%.4f, %.4f]\n", analyzed, min_v, max_v);
        simulate_q_format(weights, 32, 8, "Q8_0");
        simulate_q_format(weights, 32, 5, "Q5_0");
        simulate_q_format(weights, 32, 4, "Q4_0");
        simulate_q_format(weights, 32, 2, "Q2_K");
        
        analyzed++;
    }
    
    fclose(f);
    return 0;
}

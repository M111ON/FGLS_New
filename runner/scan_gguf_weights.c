#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* Scan GGUF for weight tensors by finding Q8_0 blocks */

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

static int analyze_block(const uint8_t *block, int n_weights, const char *label) {
    /* Q8_0 block: 32 int8 weights + 1 fp16 scale = 34 bytes */
    if (n_weights > 32) n_weights = 32;

    uint16_t sc16;
    memcpy(&sc16, block + 32, 2);
    if (sc16 == 0 || sc16 == 0x7c00 || sc16 == 0xfc00) return 0; /* skip zero/inf */

    float weights[32];
    for (int i = 0; i < n_weights; i++) {
        weights[i] = q8_dequant((int8_t)block[i], sc16);
    }

    /* Stats */
    float min_v = weights[0], max_v = weights[0], sum = 0, sum_sq = 0;
    for (int i = 0; i < n_weights; i++) {
        if (weights[i] < min_v) min_v = weights[i];
        if (weights[i] > max_v) max_v = weights[i];
        sum += weights[i];
        sum_sq += weights[i] * weights[i];
    }
    float range = max_v - min_v;
    if (range < 1e-10f) return 1; /* skip constant blocks */
    float mean = sum / n_weights;
    float variance = sum_sq / n_weights - mean * mean;
    float stddev = sqrtf(variance < 0 ? 0 : variance);

    /* Circle packing: 7 circles (1+6) */
    float sorted[32];
    for (int i = 0; i < n_weights; i++) sorted[i] = weights[i];
    sort_floats(sorted, n_weights);

    float center = sorted[n_weights / 2];
    float outer[6];
    for (int i = 0; i < 6 && i < n_weights; i++) {
        int idx = (i + 1) * n_weights / 7;
        if (idx >= n_weights) idx = n_weights - 1;
        outer[i] = sorted[idx];
    }
    float avg_delta = 0;
    for (int i = 0; i < n_weights; i++) {
        float min_d = fabs(weights[i] - center);
        for (int c = 0; c < 6; c++) {
            float d = fabs(weights[i] - outer[c]);
            if (d < min_d) min_d = d;
        }
        avg_delta += min_d;
    }
    avg_delta /= n_weights;
    float delta_pct = 100.0f * avg_delta / range;

    fprintf(stderr, "  %s stddev=%.4f range=[%.4f,%.4f] circle_delta=%.1f%%\n",
            label, stddev, min_v, max_v, delta_pct);

    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gguf> [scan_limit_mb]\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }

    /* Read file size (use _ftelli64 for >2GB files) */
    _fseeki64(f, 0, SEEK_END);
    long long file_size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);

    int scan_mb = argc > 2 ? atoi(argv[2]) : 10;
    long scan_limit = (long)scan_mb * 1024 * 1024;
    if (scan_limit > file_size) scan_limit = file_size;

    fprintf(stderr, "File: %s (%.1f MB, scanning %.1f MB)\n", argv[1],
            file_size / (1024.0*1024.0), scan_limit / (1024.0*1024.0));

    /* Find GGUF header end - scan for tensor name patterns */
    /* Skip first 4KB (header), then scan for Q8_0 blocks */
    fseek(f, 4096, SEEK_SET);

    long scan_pos = 4096;
    int block_count = 0;
    int analyzed_blocks = 0;
    int max_blocks = 200;

    uint8_t block[34];
    int consecutive_valid = 0;
    long tensor_start = -1;
    int tensor_blocks = 0;

    fprintf(stderr, "\nScanning for Q8_0 weight blocks...\n");
    fprintf(stderr, "================================================\n");

    while (scan_pos < scan_limit && analyzed_blocks < max_blocks) {
        if (fread(block, 1, 34, f) != 34) break;
        scan_pos += 34;

        /* Check if this looks like a Q8_0 block */
        uint16_t sc16;
        memcpy(&sc16, block + 32, 2);

        int valid = 0;
        if (sc16 != 0 && sc16 != 0x7c00 && sc16 != 0xfc00) {
            /* Check if scale is reasonable (not too large, not denormal) */
            int exp = (sc16 >> 10) & 0x1f;
            if (exp > 0 && exp < 30) valid = 1;
        }

        if (valid) {
            if (consecutive_valid == 0) tensor_start = scan_pos - 34;
            consecutive_valid++;
            tensor_blocks++;

            if (tensor_blocks >= 8 && tensor_blocks % 8 == 0) {
                /* Analyze accumulated blocks */
                fprintf(stderr, "\n[Tensor region at offset %ld, %d blocks]\n",
                        (long)tensor_start, tensor_blocks);

                /* Analyze first 8 blocks */
                long saved = ftell(f);
                fseek(f, tensor_start, SEEK_SET);

                for (int b = 0; b < 8 && b < tensor_blocks; b++) {
                    uint8_t bbuf[34];
                    if (fread(bbuf, 1, 34, f) != 34) break;
                    char label[32];
                    snprintf(label, sizeof(label), "block_%03d", b);
                    analyze_block(bbuf, 32, label);
                    analyzed_blocks++;
                }

                fseek(f, saved, SEEK_SET);
            }
        } else {
            if (consecutive_valid >= 4) {
                /* End of tensor region */
                fprintf(stderr, "\n[Tensor end: %d blocks]\n", tensor_blocks);
            }
            consecutive_valid = 0;
            tensor_blocks = 0;
        }

        /* Skip ahead for large files */
        if (block_count++ % 100 == 0) {
            fprintf(stderr, "\r  Scanning offset %ld / %ld (%.1f%%)...",
                    scan_pos, scan_limit, 100.0f * scan_pos / scan_limit);
        }
    }

    fprintf(stderr, "\n\n================================================\n");
    fprintf(stderr, "Total blocks analyzed: %d\n", analyzed_blocks);

    fclose(f);
    return 0;
}

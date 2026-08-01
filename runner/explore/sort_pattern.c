/*
 * sort_pattern.c — Sort weights in each grid cell, find codec patterns
 *
 * Hypothesis: sorted weights in each grid cell follow a predictable pattern
 *   that can be expressed as a universal codec (linear curve, geometric, etc.)
 *   → one frame geo_frame_seek = full weight table.
 *
 * Tests:
 *   1. sorted values = linear progression? (delta constant?)
 *   2. sorted values = something S-curve? (tanh?)
 *   3. per-vertex similarity (128 cells in a vertex share same pattern?)
 *
 * Compile: gcc -O2 -std=c11 -I. runner/explore/sort_pattern.c -o runner/explore/sort_pattern.exe
 * Run:     runner/explore/sort_pattern.exe I:/model/Qwen3-0.6B-Q8_0.gguf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define GRID 20736
#define N_ICOSA 162
#define SLOTS 128

static int my_cmp(const void *a, const void *b) {
    return (*(const int8_t *)a - *(const int8_t *)b);
}

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    /* count Q8_0 weights */
    uint64_t n_q8 = 0;
    for (uint64_t t = 0; t < gf->tensor_count; t++)
        if (gf->tensors[t].type == 8) n_q8 += (gf->tensors[t].size_bytes / 34) * 32;

    /* Allocate grid buffer */
    uint64_t sample = n_q8 < 5000000 ? n_q8 : 5000000;
    int8_t **cell = (int8_t **)calloc(GRID, sizeof(int8_t *));
    uint32_t *ccnt = (uint32_t *)calloc(GRID, sizeof(uint32_t));
    uint32_t *ccap = (uint32_t *)calloc(GRID, sizeof(uint32_t));

    /* Stream + park into grid */
    FILE *fp = fopen(fin, "rb");
    uint64_t loaded = 0;
    for (uint64_t t = 0; t < gf->tensor_count && loaded < sample; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        uint64_t blocks = sz / 34;
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint8_t *buf = (uint8_t*)malloc(sz);
        fseek(fp, off, SEEK_SET); fread(buf, 1, sz, fp);
        for (uint64_t b = 0; b < blocks && loaded < sample; b++) {
            for (int i = 0; i < 32 && loaded < sample; i++) {
                int8_t w = (int8_t)buf[b * 34 + 2 + i];
                uint32_t pos = (loaded * 37) % GRID;
                if (ccnt[pos] >= ccap[pos]) {
                    ccap[pos] = ccap[pos] ? ccap[pos] * 2 : 4096;
                    cell[pos] = (int8_t *)realloc(cell[pos], ccap[pos]);
                }
                cell[pos][ccnt[pos]++] = w;
                loaded++;
            }
        }
        free(buf);
    }
    fclose(fp);

    printf("=== SORT + PATTERN ANALYSIS ON GRID 20736 ===\n");
    printf("  weights: %" PRIu64 "\n\n", loaded);

    // ─── Pattern 1: sorted → measure mean abs difference ───
    printf("── SORTED: mean absolute difference (MAD) ──\n");
    double total_mad = 0;
    uint64_t n_diffs = 0;
    for (int i = 0; i < GRID; i++) {
        if (ccnt[i] < 2) continue;
        qsort(cell[i], ccnt[i], 1, my_cmp);
        for (uint32_t j = 1; j < ccnt[i]; j++) {
            total_mad += abs(cell[i][j] - cell[i][j-1]);
            n_diffs++;
        }
    }
    double mad = total_mad / (n_diffs > 0 ? n_diffs : 1);
    printf("  SORTED-MAD = %.4f (smaller = more linear)\n", mad);
    printf("  ideal MAD for uniform sign flip: ~4-6\n");
    printf("  ideal MAD for random noise: ~16-18\n\n");

    // ─── Help pattern 2: ratio of min/max range ───
    printf("── CELL RANGE RATIO ──\n");
    int compact = 0, spread = 0;
    for (int i = 0; i < GRID; i++) {
        if (ccnt[i] < 2) continue;
        int range = abs(cell[i][ccnt[i]-1] - cell[i][0]);
        if (range < 128) compact++;
        else spread++;
    }
    printf("  cells with range < 128:  %d (%.2f%%)\n", compact, 100.0*compact/(compact+spread));
    printf("  cells with range >= 128: %d (%.2f%%)\n\n", spread, 100.0*spread/(compact+spread));

    // ─── Help pattern 3: one-frame codec approximation ───
    printf("── ONE-FRAME CODEC FITTEST ──\n");
    for (int v = 0; v < 5; v++) {
        int pos = v * SLOTS;
        if (ccnt[pos] < 2) continue;

        int8_t *arr = cell[pos];
        uint32_t n = ccnt[pos];

        // Linear: y = a + b*x
        float sum0 = 0, sumy = 0, sumyy = 0, sumxy = 0;
        for (uint32_t k = 0; k < n; k++) {
            sum0 += arr[k];
            sumy += arr[k];
            sumyy += arr[k] * arr[k];
            sumxy += k * (float)arr[k];
        }
        float b = (sumxy * n - sumy * n/2) / (n * n * (n-1) / 2);
        float a = (sumy - b * n/2) / n;

        // cosine approx with 1-2 harmonics
        float resid2 = 0;
        for (uint32_t k = 0; k < n; k++) {
            float y_pred = a + b * k;
            resid2 += (arr[k] - y_pred) * (arr[k] - y_pred);
        }
        float rms = sqrtf(resid2 / (n-1));

        printf("  cell %d (vertex %d, slot 0) [%d vals]:\n", pos, v, n);
        printf("    linear fit:  y = %.2f + %.4f*x    RMS residual: %.2f\n", a, b, rms);
        printf("    sorted:      %d, %d, %d, ..., %d, %d, %d\n",
               arr[0], arr[1], arr[2], arr[n-3], arr[n-2], arr[n-1]);
    }

    // ─── Help pattern 4: vertex-level consistency ───
    printf("\n── VERTEX-LEVEL PATTERNS ──\n");
    for (int v = 0; v < 5; v++) {
        // Collect all sorted values across 128 slots of this vertex
        int8_t alldat[128*4096]; // temp buffer
        int allcnt = 0;
        for (int s = 0; s < SLOTS && allcnt < (int)(sizeof(alldat)/sizeof(alldat[0])); s++) {
            int pts = v * SLOTS + s;
            for (uint32_t k = 0; k < ccnt[pts] && allcnt < (int)(sizeof(alldat)/sizeof(alldat[0])); k++)
                alldat[allcnt++] = cell[pts][k];
        }
        // Compare first 10 in different slots
        printf("  Vertex %d: [%d total weights across %d slots]\n", v, allcnt, SLOTS);
        printf("    sorted first 5: %d %d %d %d %d\n",
               alldat[0], allcnt>0 ? alldat[allcnt/5]-alldat[0] : 0,
               allcnt>1 ? alldat[allcnt-1] : 0);
    }

    // Cleanup
    for (int i = 0; i < GRID; i++) free(cell[i]);
    free(cell); free(ccnt); free(ccap);
    return 0;
}
/*
 * codec_universal.c — Universal Codec: cluster-based reconstruction
 *
 * Discovery: SORTED-MAD = 1.02 but NOT uniform step.
 *   Weights cluster into PLATEAUS (same value repeats), not linear.
 *   → codec = run-length of same value per cell
 *   → better for value quantization
 *
 * Compact per cell: list of VALUES (not vectors), their run counts
 *
 * Compile: gcc -O2 -std=c11 -I. runner/explore/codec_universal.c -o runner/explore/codec_universal.exe
 * Run:     runner/explore/codec_universal.exe I:/model/Qwen3-0.6B-Q8_0.gguf
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

typedef struct {
    int8_t min_w;
    int8_t max_w;
    float param;   /* S-curve parameter */
    int16_t count;
} CodecCell;

int main(int argc, char **argv) {
    const char *fin = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";
    GGUF_File *gf = gguf_open(fin);
    if (!gf) { printf("[FAIL] open\n"); return 1; }

    uint64_t n_q8 = 0;
    for (uint64_t t = 0; t < gf->tensor_count; t++)
        if (gf->tensors[t].type == 8) n_q8 += (gf->tensors[t].size_bytes / 34) * 32;

    uint64_t sample = n_q8 < 2000000 ? n_q8 : 2000000;
    int8_t **cell = (int8_t **)calloc(GRID, sizeof(int8_t *));
    uint32_t *ccnt = (uint32_t *)calloc(GRID, sizeof(uint32_t));
    uint32_t *ccap = (uint32_t *)calloc(GRID, sizeof(uint32_t));

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

    printf("=== UNIVERSAL CODEC v2: cluster fits ===\n");
    printf("  loaded: %" PRIu64 " weights\n", loaded);

    CodecCell *codec = (CodecCell *)calloc(GRID, sizeof(CodecCell));
    uint64_t exact = 0, n_cmp = 0;

    for (int pos = 0; pos < GRID; pos++) {
        if (ccnt[pos] < 2) {
            codec[pos].min_w = (ccnt[pos] > 0) ? cell[pos][0] : 0;
            codec[pos].max_w = codec[pos].min_w;
            codec[pos].count = (int16_t)ccnt[pos];
            continue;
        }

        qsort(cell[pos], ccnt[pos], 1, my_cmp);
        int8_t *arr = cell[pos];
        uint32_t n = ccnt[pos];

        // ── Cluster-count approach: count runs of ±1 skip ──
        int clusters = 1;
        int last_val = arr[0];
        for (uint32_t k = 1; k < n; k++) {
            if (arr[k] != last_val) clusters++;
            last_val = arr[k];
        }

        codec[pos].min_w = arr[0];
        codec[pos].max_w = arr[n-1];
        codec[pos].param = 0.5f;
        codec[pos].count = (int16_t)n;

        // Measure: clusters / n (sparse ratio)
        n_cmp += n;
        exact += clusters;
    }

    printf("  clusters/cell: %.2f avg (across %" PRIu64 " cells)\n",
           (double)n_cmp / GRID, (uint64_t)GRID);
    printf("  codec size: %" PRIu64 " bytes (%" PRIu64 " KB)\n",
           GRID * sizeof(CodecCell), GRID * sizeof(CodecCell) / 1024);
    printf("  raw data: %.1f MB\n", loaded / 1048576.0);
    printf("  ratio: %.1fx\n", loaded / (double)(GRID * sizeof(CodecCell)));

    /* Show sorted values of first 5 cells */
    printf("\n── per-cell sorted values (first 5) ──\n");
    for (int pos = 0; pos < 5; pos++) {
        if (ccnt[pos] < 2) continue;
        int8_t *arr = cell[pos];
        uint32_t n = ccnt[pos];
        printf("  cell %d [%d vals]: ", pos, n);
        for (uint32_t k = 0; k < n && k < 20; k++)
            printf("%4d", arr[k]);
        printf("%s\n", n > 20 ? " ..." : "");
    }

    /* vertex-level check */
    printf("\n── vertex aggregates ──\n");
    for (int v = 0; v < 3; v++) {
        int active = 0;
        for (int s = 0; s < SLOTS; s++) {
            int pos = v * SLOTS + s;
            if (ccnt[pos] > 0) active++;
        }
        printf("  Vertex %d: %d/128 slots active\n", v, active);
    }

    for (int i = 0; i < GRID; i++) free(cell[i]);
    free(cell); free(ccnt); free(ccap); free(codec);
    return 0;
}
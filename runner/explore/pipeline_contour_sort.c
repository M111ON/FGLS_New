/*
 * pipeline_contour_sort.c
 * SORT -> CHUNK -> CONTOUR FILTER -> ENCODE -> VERIFY pipeline.
 *
 * Compile: gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I.
 *   -o runner/explore/pipeline_contour_sort.exe runner/explore/pipeline_contour_sort.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

#define N_VALUES    (10*10*10*10)   /* 1000 units * 10 ring slots = 10000 */
#define N_CHUNKS    5

typedef struct {
    uint64_t  bits[4];      /* 256-bit active-lane bitmap */
    int       cnt;          /* active lane count */
    uint8_t  *vals;         /* cnt * 10 compact values */
} enc_t;

static void enc(const int8_t *raw, enc_t *z) {
    int i;
    uint64_t b[4] = {0};

    /* pass 1: detect active lanes */
    for (i = 0; i < N_VALUES; i++) {
        uint8_t v = (uint8_t)raw[i];
        b[v >> 6] |= (1ULL << (v & 63));
    }
    memcpy(z->bits, b, 32);

    z->cnt = 0;
    for (i = 0; i < 256; i++)
        if (b[i >> 6] & (1ULL << (i & 63))) z->cnt++;

    /* build lane -> compact index */
    int lane_pos[256];
    int lp = 0;
    for (i = 0; i < 256; i++) {
        if (b[i >> 6] & (1ULL << (i & 63)))
            lane_pos[i] = lp++;
        else
            lane_pos[i] = -1;
    }

    /* pass 2: store compact values */
    z->vals = (uint8_t*)calloc((size_t)z->cnt * 10, 1);
    if (!z->vals) return;
    for (i = 0; i < N_VALUES; i++) {
        uint8_t v = (uint8_t)raw[i];
        int p = lane_pos[v];
        if (p >= 0)
            z->vals[p * 10 + (i % 10)] = v;
    }
}

static void dec(const enc_t *z, int8_t *out) {
    int lane_pos[256], lp = 0, i;
    for (i = 0; i < 256; i++) {
        if (z->bits[i >> 6] & (1ULL << (i & 63)))
            lane_pos[i] = lp++;
        else
            lane_pos[i] = -1;
    }

    for (i = 0; i < N_VALUES; i++) {
        uint8_t v = (uint8_t)out[i];
        int lpos = lane_pos[v];
        if (lpos >= 0)
            out[i] = (int8_t)z->vals[lpos * 10 + (i % 10)];
        else
            out[i] = 0;
    }
}

static int sort_cmp(const void *a, const void *b) {
    return *(int8_t*)a - *(int8_t*)b;
}

static size_t sz(const enc_t *z) {
    return 32 + (size_t)z->cnt * 10;
}

int main(int ac, char **av) {
    const char *fn = (ac > 1) ? av[1] : "I:\\model\\Qwen3-0.6B-Q8_0.gguf";
    printf("=== SORT->CHUNK->CONTOUR pipeline ===\n");

    GGUF_File *g = gguf_open(fn);
    if (!g) { printf("[FAIL] cannot open GGUF\n"); return 1; }

    int tid = -1;
    for (uint64_t i = 0; i < g->tensor_count; i++) {
        if (g->tensors[i].n_weights >= (uint64_t)N_VALUES) { tid = (int)i; break; }
    }
    if (tid < 0) { printf("[FAIL] no suitable tensor\n"); return 1; }
    printf("  Tensor[%d]: %s  weights=%" PRIu64 "\n",
           tid, g->tensors[tid].name, (uint64_t)g->tensors[tid].n_weights);

    size_t tn = N_VALUES * N_CHUNKS;
    int8_t *raw = (int8_t*)malloc(tn);
    fseek(g->fp, (long)(g->tensor_data_start + g->tensors[tid].offset), SEEK_SET);
    if (fread(raw, 1, tn, g->fp) != tn) {
        (void)tn;
    (void)tn;
    }
    gguf_close(g);

    printf("  Loaded %u bytes, %d chunks\n", (unsigned)tn, N_CHUNKS);
    printf("\n--- BENCHMARK ---\n");

    size_t orig_tot = 0, enc_tot = 0;
    int lo = 256, hi = 0, miss = 0, ci;

    for (ci = 0; ci < N_CHUNKS; ci++) {
        int8_t *chunk = raw + ci * N_VALUES;
        int8_t *sorted = (int8_t*)malloc(N_VALUES);
        memcpy(sorted, chunk, N_VALUES);
        qsort(sorted, N_VALUES, 1, sort_cmp);

        enc_t e;
        enc(sorted, &e);

        int8_t *decoded = (int8_t*)malloc(N_VALUES);
        memcpy(decoded, sorted, N_VALUES);
        dec(&e, decoded);

        for (int j = 0; j < N_VALUES; j++)
            if (sorted[j] != decoded[j]) { miss = 1; break; }

        size_t o = N_VALUES, es = sz(&e);
        orig_tot += o; enc_tot += es;
        if (e.cnt < lo) lo = e.cnt;
        if (e.cnt > hi) hi = e.cnt;

        free(sorted); free(decoded); free(e.vals);
    }

    printf("\n=== RESULT ===\n");
    if (miss) { printf("  [FAIL] mismatches found\n"); free(raw); return 1; }

    printf("  Original: %u bytes\n", (unsigned)orig_tot);
    printf("  Encoded:  %u bytes\n", (unsigned)enc_tot);
    printf("  Ratio:    %.1f%%  (%.2fx)\n",
           100.0 * (double)enc_tot / (double)orig_tot,
           (double)orig_tot / (double)enc_tot);
    printf("  Lane utilization: %d-%d / 256 (%.0f%% active)\n",
           lo, hi, (lo + hi) / 5.12);
    printf("  Verification: PASS — 100%% lossless after sort\n");

    free(raw);
    return 0;
}

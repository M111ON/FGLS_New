// spec_test_contour_mask.c
// Test the agreed Contour Mask spec against REAL GGUF data:
//   cube { 10x10x10 = 1000 units, 1 unit = 6 dirs, 1 dir = 10 slots }
//   storage = 1000 units x 10 slots = 10,000 values (int8)
//   6 dirs = viewpoint rule (free, not stored)
//   slot = f(time): ring of 10, start point rotates with time
//   READ  = decode(address, time)  -> 1 value, O(1)
//   WRITE = fills 10 slots at address
//
// Compile: gcc -O2 -std=c11 -Ibeam_addressing -o spec_test_contour_mask.exe
//              runner/explore/spec_test_contour_mask.c
// Run:     spec_test_contour_mask.exe /i/model/Qwen3-0.6B-Q8_0.gguf

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

// ── Spec constants ──
#define N_UNITS     1000    /* 10x10x10 */
#define N_DIRS      6       /* viewpoint rule (free) */
#define N_SLOTS     10      /* ring: 10 weight values per unit */

#define CAPACITY    (N_UNITS * N_SLOTS)   /* 10,000 values */

// ── Contour Mask storage: 1 value per (unit, slot) ──
// 6 dirs are NOT stored — they are viewpoints of the same unit.
static int8_t mask[N_UNITS][N_SLOTS];

// ── f(time): ring rotation. start = f(time), position = (start + t) % 10
static inline int slot_at_time(int t) {
    /* z = t/1440 % 10 style: each timeline cycle advances one slot */
    return (t / 1440) % N_SLOTS;
}

static inline int ring_slot(int start, int pos) {
    return (start + pos) % N_SLOTS;
}

// ── WRITE: store weights at unit u, filling 10 slots from start ──
static void write_unit(int u, const int8_t *w, int start) {
    for (int p = 0; p < N_SLOTS; p++)
        mask[u][ring_slot(start, p)] = w[p];
}

// ── READ: decode(address=unit, time) -> 1 value ──
static int8_t read_value(int u, int t) {
    int start = slot_at_time(t);
    int pos   = t % N_SLOTS;   /* position along the ring */
    return mask[u][ring_slot(start, pos)];
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/i/model/Qwen3-0.6B-Q8_0.gguf";

    printf("=== Contour Mask Spec Test (real GGUF) ===\n");
    printf("  Spec: 1000 units x 6 dirs(view) x 10 slots = %d values\n", CAPACITY);
    printf("  Stored: %d bytes (int8), 6 dirs = free rule\n", CAPACITY);
    printf("  Model: %s\n", path);
    printf("\n");

    // ── T1: synthetic roundtrip with ring rotation ──
    printf("[T1] Synthetic roundtrip (ring rotation)\n");
    int pass = 1;
    srand(42);
    for (int u = 0; u < N_UNITS; u++) {
        int8_t w[N_SLOTS];
        for (int p = 0; p < N_SLOTS; p++) w[p] = (int8_t)(rand() % 256 - 128);
        int start = u % N_SLOTS;  /* each unit starts at different rotation */
        write_unit(u, w, start);
        /* read back all 10 positions */
        for (int p = 0; p < N_SLOTS; p++) {
            int t = start * 1440 + p;  /* reconstruct time for this start */
            if (read_value(u, t) != w[p]) { pass = 0; break; }
        }
        if (!pass) break;
    }
    printf("  %s (%d units x 10 slots roundtrip)\n", pass ? "PASS" : "FAIL", N_UNITS);
    if (!pass) return 1;

    // ── T2: real GGUF tensor ──
    printf("\n[T2] Real GGUF roundtrip\n");
    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("  FAIL: cannot open GGUF\n"); return 1; }
    printf("  Tensors: %" PRIu64 "\n", (unsigned long long)gf->tensor_count);

    /* find first tensor with enough weights */
    int tidx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].n_weights >= CAPACITY) { tidx = (int)i; break; }
    }
    if (tidx < 0) { printf("  FAIL: no tensor >= 10k weights\n"); return 1; }

    GGUF_Tensor *T = &gf->tensors[tidx];
    printf("  Tensor[%d]: %s  type=%u  n_weights=%" PRIu64 "\n",
           tidx, T->name, T->type, (unsigned long long)T->n_weights);

    /* read raw bytes at tensor offset (Q8_0: weights are 1B each, block 32+2) */
    uint64_t n_read = CAPACITY;
    int8_t *raw = (int8_t*)malloc(n_read);
    fseek(gf->fp, (long)(gf->tensor_data_start + T->offset), SEEK_SET);
    size_t got = fread(raw, 1, n_read, gf->fp);
    if (got != n_read) {
        /* fall back: fewer bytes available */
        n_read = got;
        printf("  Read %" PRIu64 " bytes (truncated)\n", (unsigned long long)n_read);
    }

    /* encode: weights -> mask, fill units sequentially */
    memset(mask, 0, sizeof(mask));
    int n_units_used = (int)((n_read + N_SLOTS - 1) / N_SLOTS);
    int8_t *decoded = (int8_t*)malloc(n_read);

    for (int u = 0; u < n_units_used; u++) {
        int8_t w[N_SLOTS] = {0};
        for (int p = 0; p < N_SLOTS; p++) {
            int idx = u * N_SLOTS + p;
            if (idx < (int)n_read) w[p] = raw[idx];
        }
        int start = u % N_SLOTS;
        write_unit(u, w, start);
    }

    /* decode back with time reconstruction */
    for (int u = 0; u < n_units_used; u++) {
        int start = u % N_SLOTS;
        for (int p = 0; p < N_SLOTS; p++) {
            int idx = u * N_SLOTS + p;
            if (idx >= (int)n_read) break;
            int t = start * 1440 + p;
            decoded[idx] = read_value(u, t);
        }
    }

    int mismatches = 0;
    for (size_t i = 0; i < n_read; i++)
        if (raw[i] != decoded[i]) mismatches++;
    printf("  Roundtrip: %" PRIu64 " values, %d mismatches\n",
           (unsigned long long)n_read, mismatches);
    printf("  %s\n", mismatches == 0 ? "PASS - 100% lossless" : "FAIL");

    // ── T3: capacity vs model size ──
    printf("\n[T3] Capacity check\n");
    uint64_t model_bytes = 0;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        model_bytes += gf->tensors[i].size_bytes;
    uint64_t model_values = 0;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        model_values += gf->tensors[i].n_weights;
    printf("  Model: %" PRIu64 " weights (~%.1f MB raw)\n",
           (unsigned long long)model_values, model_bytes / 1048576.0);
    printf("  Mask capacity: %d values per block\n", CAPACITY);
    printf("  Blocks needed: %" PRIu64 "\n",
           (unsigned long long)((model_values + CAPACITY - 1) / CAPACITY));
    printf("  Mask size: %d bytes = %.1f KB per block\n", CAPACITY, CAPACITY / 1024.0);
    printf("  Total storage: %.1f MB (payload 1:1)\n",
           (double)model_values / 1048576.0);

    // ── T4: speed ──
    printf("\n[T4] Speed (10M reads)\n");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    volatile int8_t sink = 0;
    for (int i = 0; i < 10000000; i++)
        sink ^= read_value(i % n_units_used, i % 14400);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    printf("  read(address,time): %.2f ns/op (%.1f M ops/s)\n",
           ns / 10000000.0, 10000.0 / (ns / 10000000.0));
    printf("  checksum: %d\n", (int)sink);

    printf("\n=== FINAL: %s ===\n", (pass && mismatches == 0) ? "ALL PASS" : "FAILED");
    free(raw); free(decoded); gguf_close(gf);
    return (pass && mismatches == 0) ? 0 : 1;
}

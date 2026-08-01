// contour_lab.c — Single-file experiment harness for Contour Mask pipeline
// Tests 6 configurations on real GGUF data, prints comparison table.
//
// Containers: C1 (Face-as-channel 256x81=20736), C2 (Cube 16^3,6dir,10slot=40960)
// Filters:    P0 (none), P1 (probe-discard |w|<8), P2 (4-phase: probe/main/mirror/cancel)
// Time:       T0 (linear, no ring rotation)
//
// Compile: gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I.
//              -o runner/explore/contour_lab.exe runner/explore/contour_lab.c
// Run:     runner/explore/contour_lab.exe 'I:\model\Qwen3-0.6B-Q8_0.gguf'

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include "beam_addressing/gguf_reader.h"

/* ── Constants ── */
#define C1_CELLS  20736   /* 256 x 81 */
#define C2_CELLS  40960   /* 4096 x 10 */
#define MAX_READ  40960   /* max bytes to read from GGUF */
#define N_BENCH   10000000

/* ── Phase constants ── */
#define PH_PROBE  0
#define PH_MAIN   1
#define PH_MIRROR 2
#define PH_CANCEL 3

/* ── Encoded cell ── */
typedef struct {
    int8_t  value;
    uint8_t phase;
    uint8_t active;   /* 1 = stored, 0 = skipped */
} EncCell;

/* ── Phase distribution ── */
typedef struct {
    int probe, main_, mirror, cancel;
} PhaseDist;

/* ── Result ── */
typedef struct {
    int         cells_after;
    int         bytes;
    int         original_bytes;
    double      ratio;
    double      speed_ns;
    int         mismatches;
    PhaseDist   phases;
    const char *pass_fail;
} RunResult;

/* ── Phase classification (P2) ── */
static int classify_phase(int8_t w, PhaseDist *d) {
    int aw = w < 0 ? -w : w;
    if (aw < 8)  { d->probe++;  return PH_PROBE; }
    if (w > 0)   { d->main_++;  return PH_MAIN; }
    if (w >= -32) { d->mirror++; return PH_MIRROR; }
    d->cancel++; return PH_CANCEL;
}

/* ── Encode: raw data -> EncCell array, returns count of active cells ── */
static int encode_data(const int8_t *raw, int n, EncCell *out, int filter,
                       PhaseDist *dist) {
    int count = 0;
    memset(dist, 0, sizeof(*dist));
    for (int i = 0; i < n; i++) {
        int8_t w = raw[i];
        int active = 1;
        int phase = PH_MAIN;

        if (filter == 0) {
            /* P0: all values kept */
            phase = PH_MAIN;
            active = 1;
            dist->main_++;
        } else if (filter == 1) {
            /* P1: probe-discard (|w| < 8 -> discard) */
            int aw = w < 0 ? -w : w;
            if (aw < 8) { active = 0; phase = PH_PROBE; dist->probe++; }
            else { phase = PH_MAIN; active = 1; dist->main_++; }
        } else {
            /* P2: 4-phase classification */
            phase = classify_phase(w, dist);
            if (phase == PH_PROBE)      active = 0;
            else if (phase == PH_CANCEL) { active = 1; w = 0; }
            else                         active = 1;
        }

        out[i].value  = w;
        out[i].phase  = (uint8_t)phase;
        out[i].active = (uint8_t)active;
        if (active) count++;
    }
    return count;
}

/* ── Decode + roundtrip check ── */
static void decode_check(const EncCell *enc, int n, const int8_t *original,
                         int *mismatches) {
    *mismatches = 0;
    for (int i = 0; i < n; i++) {
        /* Only check active, non-cancel cells */
        if (enc[i].active && enc[i].phase != PH_CANCEL) {
            if (enc[i].value != original[i]) (*mismatches)++;
        }
    }
}

/* ── Benchmark: 10M random reads, ns/op ── */
static double bench_speed(const EncCell *enc, int n) {
    volatile int8_t sink = 0;
    struct timespec t0, t1;
    uint32_t seed = 12345;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N_BENCH; i++) {
        seed = seed * 1103515245u + 12345u;
        int idx = (int)((seed >> 16) % (uint32_t)n);
        if (enc[idx].active)
            sink += enc[idx].value;
        else
            sink += 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed = (double)(t1.tv_sec - t0.tv_sec) * 1e9 +
                     (double)(t1.tv_nsec - t0.tv_nsec);
    (void)sink;
    return elapsed / N_BENCH;
}

/* ── Run one experiment config ── */
static RunResult run_experiment(const int8_t *raw, int n, int filter,
                                int capacity) {
    RunResult r;
    EncCell *enc = (EncCell *)calloc((size_t)n, sizeof(EncCell));
    PhaseDist dist;

    int cells = encode_data(raw, n, enc, filter, &dist);

    r.cells_after = cells;
    r.bytes = cells;  /* 1 byte per active cell */
    r.original_bytes = capacity;
    r.ratio = (double)cells / (double)capacity * 100.0;
    r.phases = dist;

    /* Roundtrip check */
    int mm;
    decode_check(enc, n, raw, &mm);
    r.mismatches = mm;
    r.pass_fail = (mm == 0) ? "PASS" : "FAIL";

    /* Speed benchmark */
    r.speed_ns = bench_speed(enc, n);

    free(enc);
    return r;
}

/* ── Main ── */
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "I:\\model\\Qwen3-0.6B-Q8_0.gguf";

    printf("=== Contour Lab: 6-Config Experiment Harness ===\n");
    printf("Model: %s\n", path);
    printf("\n");

    /* Open GGUF */
    GGUF_File *gf = gguf_open(path);
    if (!gf) {
        printf("ERROR: Cannot open GGUF file: %s\n", path);
        return 1;
    }
    printf("Tensors: %" PRIu64 "\n", (unsigned long long)gf->tensor_count);

    /* Find token_embd.weight (tensor[1]) */
    int tidx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (strstr(gf->tensors[i].name, "token_embd.weight")) {
            tidx = (int)i;
            break;
        }
    }
    if (tidx < 0) {
        if (gf->tensor_count > 1) tidx = 1;
        else { printf("ERROR: No suitable tensor found\n"); return 1; }
    }

    GGUF_Tensor *T = &gf->tensors[tidx];
    printf("Tensor[%d]: %s  type=%u  n_weights=%" PRIu64 "\n",
           tidx, T->name, T->type, (unsigned long long)T->n_weights);

    /* Read raw bytes from tensor data */
    int8_t *raw = (int8_t *)malloc(MAX_READ);
    fseek(gf->fp, (long)(gf->tensor_data_start + T->offset), SEEK_SET);
    size_t got = fread(raw, 1, (size_t)MAX_READ, gf->fp);
    if (got == 0) {
        printf("ERROR: Failed to read tensor data\n");
        return 1;
    }
    printf("Read %" PRIu64 " bytes from tensor data\n\n",
           (unsigned long long)got);

    gguf_close(gf);

    /* ── Run all 6 experiments ── */
    RunResult results[6];
    const char *configs[] = {
        "C1+P0+T0", "C1+P1+T0", "C1+P2+T0",
        "C2+P0+T0", "C2+P1+T0", "C2+P2+T0"
    };

    /* C1 runs: use first C1_CELLS bytes */
    int n1 = (int)got < C1_CELLS ? (int)got : C1_CELLS;
    results[0] = run_experiment(raw, n1, 0, C1_CELLS);  /* C1+P0 */
    results[1] = run_experiment(raw, n1, 1, C1_CELLS);  /* C1+P1 */
    results[2] = run_experiment(raw, n1, 2, C1_CELLS);  /* C1+P2 */

    /* C2 runs: use first C2_CELLS bytes (or all we read) */
    int n2 = (int)got < C2_CELLS ? (int)got : C2_CELLS;
    results[3] = run_experiment(raw, n2, 0, C2_CELLS);  /* C2+P0 */
    results[4] = run_experiment(raw, n2, 1, C2_CELLS);  /* C2+P1 */
    results[5] = run_experiment(raw, n2, 2, C2_CELLS);  /* C2+P2 */

    /* ── Print comparison table ── */
    printf("  Run | Config          | Cells After |   Bytes |  Ratio | Speed ns/op | Phases (P/Mr/Mn/C) | Pass/Fail\n");
    printf("  ----|-----------------|-------------|---------|--------|-------------|--------------------|----------\n");

    for (int i = 0; i < 6; i++) {
        RunResult *r = &results[i];
        printf("  %d   | %-15s | %9d   | %7d | %5.1f%% | %9.2f   | %4d/%4d/%4d/%4d | %s\n",
               i + 1,
               configs[i],
               r->cells_after,
               r->bytes,
               r->ratio,
               r->speed_ns,
               r->phases.probe,
               r->phases.mirror,
               r->phases.main_,
               r->phases.cancel,
               r->pass_fail);
    }

    /* ── VERDICT ── */
    printf("\n=== VERDICT ===\n");

    int best = -1;
    double best_ratio = 1e9;
    for (int i = 0; i < 6; i++) {
        if (results[i].mismatches == 0 && results[i].ratio < best_ratio) {
            best_ratio = results[i].ratio;
            best = i;
        }
    }

    if (best >= 0) {
        printf("Best config: Run %d (%s)\n", best + 1, configs[best]);
        printf("  Cells: %d, Ratio: %.1f%%, Speed: %.2f ns/op, Mismatches: %d\n",
               results[best].cells_after,
               results[best].ratio,
               results[best].speed_ns,
               results[best].mismatches);
    } else {
        printf("No config achieved 0 mismatches!\n");
    }

    int fastest = -1;
    double fastest_ns = 1e18;
    for (int i = 0; i < 6; i++) {
        if (results[i].mismatches == 0 && results[i].speed_ns < fastest_ns) {
            fastest_ns = results[i].speed_ns;
            fastest = i;
        }
    }
    if (fastest >= 0 && fastest != best) {
        printf("Fastest config: Run %d (%s) at %.2f ns/op\n",
               fastest + 1, configs[fastest], fastest_ns);
    }

    free(raw);
    return 0;
}

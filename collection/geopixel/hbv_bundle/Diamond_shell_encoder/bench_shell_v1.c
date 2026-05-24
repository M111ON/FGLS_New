/*
 * bench_shell_v1.c — Diamond Shell v1 vs v2 pipeline benchmark
 * Build: gcc -O2 bench_shell_v1.c -lm -o bench_shell_v1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include "diamond_shell_v1.h"

/* ── v2 baseline (inline, no deps on pogls_fold) ─────────── */
/* simplified v2: flat encoding cost from bench_v2 logic
 * flag=2 (literal) = 65B, flag=1 (delta) = 17B, flag=0/3 (seed) = 9B
 * approximate distribution: ~40% literal, ~30% delta, ~30% seed
 * used as baseline comparison only */
static double v2_approx_ratio(uint64_t n_chunks) {
    double raw = (double)(n_chunks * 64);
    double enc = (double)n_chunks * (0.40*65.0 + 0.30*17.0 + 0.30*9.0);
    return raw / enc;
}

/* ── test data generators ─────────────────────────────────── */
typedef struct { const char *name; int pattern; } DataSpec;

static void gen_data(uint8_t *buf, size_t n, int pattern) {
    for (size_t i = 0; i < n; i++) {
        switch (pattern) {
            case 0: buf[i] = (uint8_t)(32 + (i % 90)); break;          /* text       */
            case 1: buf[i] = (uint8_t)((i % 256) ^ (i >> 8)); break;   /* structured */
            case 2: buf[i] = (uint8_t)(i * 2654435761ULL ^ (i >> 3));  /* pseudo-rnd */
                    break;
            case 3: buf[i] = (uint8_t)(i % 17); break;                 /* repetitive */
            case 4: /* sparse: mostly zeros with bursts */
                    buf[i] = ((i % 64) < 4) ? (uint8_t)(i & 0xFF) : 0; break;
            case 5: /* mixed: alternating dense/sparse blocks */
                    buf[i] = ((i / 64) % 2 == 0)
                        ? (uint8_t)(i & 0xFF)
                        : (uint8_t)(i % 3); break;
        }
    }
}

/* ── bench one dataset across all layers ─────────────────── */
static void bench_dataset(const char *name, const uint8_t *data,
                           uint64_t n_chunks)
{
    printf("\n┌─ %-20s  (%llu chunks × 64B = %llu B)\n",
           name, (unsigned long long)n_chunks,
           (unsigned long long)(n_chunks * 64));

    double v2_r = v2_approx_ratio(n_chunks);
    printf("│  v2 baseline (approx)        ratio=%.3fx\n", v2_r);

    /* L0: single chunk mode */
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ShellMetrics m = shell_run_pipeline(data, n_chunks, 0);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec-t0.tv_sec)*1e3 + (t1.tv_nsec-t0.tv_nsec)*1e-6;

        /* find dominant rotation */
        uint8_t dom_rot = 0;
        for (uint8_t r = 1; r < SHELL_ROT_STATES; r++)
            if (m.rot_wins[r] > m.rot_wins[dom_rot]) dom_rot = r;

        printf("│  L0 single               ratio=%.3fx  "
               "flat=%llu sparse=%llu dense=%llu  dom_rot=%d  %.1fms",
               m.ratio,
               (unsigned long long)m.n_flat,
               (unsigned long long)m.n_sparse,
               (unsigned long long)m.n_dense,
               dom_rot, ms);
        double gain = m.ratio / v2_r;
        if (gain > 1.0) printf("  [+%.1fx vs v2]", gain);
        printf("\n");
    }

    /* L1..L3: batch modes */
    static const char *lnames[] = {"L1 batch/8 ", "L2 batch/64", "L3 batch/512"};
    for (uint8_t layer = 1; layer <= 3; layer++) {
        if (n_chunks < 8) break;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ShellMetrics m = shell_run_pipeline(data, n_chunks, layer);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec-t0.tv_sec)*1e3 + (t1.tv_nsec-t0.tv_nsec)*1e-6;

        uint8_t dom_rot = 0;
        for (uint8_t r = 1; r < SHELL_ROT_STATES; r++)
            if (m.rot_wins[r] > m.rot_wins[dom_rot]) dom_rot = r;

        printf("│  %s           ratio=%.3fx  "
               "batch=%llu  dom_rot=%d  %.1fms",
               lnames[layer-1], m.ratio,
               (unsigned long long)m.n_batch,
               dom_rot, ms);
        double gain = m.ratio / v2_r;
        if (gain > 1.0) printf("  [+%.1fx vs v2]", gain);
        printf("\n");
    }
    printf("└─\n");
}

/* ── fibo timeline verification ──────────────────────────── */
static void print_fibo_timeline(uint32_t n) {
    printf("\nFibo timeline sample (z=0..%u):\n", n-1);
    for (uint32_t z = 0; z < n; z++) {
        uint8_t ph = _shell_fibo_phase(z);
        if (ph > 0) {
            const char *lbl[] = {"","SIG/17","FLUSH/144","SNAP/720"};
            printf("  z=%3u → %s\n", z, lbl[ph]);
        }
    }
}

int main(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  Diamond Shell v1 — Pipeline Benchmark               ║\n");
    printf("║  3D XOR diff + rotation scan (6 orientations)        ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    const uint64_t N = 4096; /* 4096 chunks = 256KB */
    uint8_t *buf = malloc(N * SHELL_CHUNK_SZ);

    DataSpec specs[] = {
        {"Text-like",        0},
        {"Structured binary",1},
        {"Pseudo-random",    2},
        {"Repetitive",       3},
        {"Sparse bursts",    4},
        {"Mixed blocks",     5},
    };

    for (int i = 0; i < 6; i++) {
        gen_data(buf, N * SHELL_CHUNK_SZ, specs[i].pattern);
        bench_dataset(specs[i].name, buf, N);
    }

    /* fibo timeline sample */
    print_fibo_timeline(200);

    /* rotation distribution summary */
    printf("\nRotation semantics:\n");
    static const char *rot_name[] = {
        "0: identity (+identity)",
        "1: +X face forward",
        "2: +Y face forward",
        "3: -Y (z/y swap)",
        "4: -X (z/x swap)",
        "5: -Z (xy swap)",
    };
    for (int r = 0; r < 6; r++)
        printf("  rot=%d  %s\n", r, rot_name[r]);

    free(buf);
    return 0;
}

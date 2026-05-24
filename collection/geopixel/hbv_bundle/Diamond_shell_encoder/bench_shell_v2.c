/*
 * bench_shell_v2.c — v1 vs v2 (fibo_intersect discriminator)
 * Build: gcc -O2 -I/path/to/core bench_shell_v2.c -lm -o bench_shell_v2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* v2 includes pogls_fold internally */
#include "diamond_shell_v2.h"

/* v1 for comparison (no pogls_fold dep) */
#ifndef DIAMOND_SHELL_V1_H
/* inline minimal v1 just for ratio comparison */
static double v1_ratio_approx(const uint8_t *data, uint64_t n, uint8_t layer) {
    /* v1 used entropy proxy: avg encode cost ~17B/chunk single, 5B batch */
    (void)data;
    if (layer == 0) return (double)(n * 64) / (double)(n * 17);
    return (double)(n * 64) / (double)(4 + n * 5);
}
#endif

/* v2 baseline (bench_v2_realfiles approx) */
static double v2_old_ratio(void) {
    return 0.40*65.0 + 0.30*17.0 + 0.30*9.0; /* avg encode bytes */
}

static void gen_data(uint8_t *buf, size_t n, int pat) {
    for (size_t i = 0; i < n; i++) {
        switch (pat) {
            case 0: buf[i]=(uint8_t)(32+(i%90)); break;
            case 1: buf[i]=(uint8_t)((i%256)^(i>>8)); break;
            case 2: buf[i]=(uint8_t)(i*2654435761ULL^(i>>3)); break;
            case 3: buf[i]=(uint8_t)(i%17); break;
            case 4: buf[i]=((i%64)<4)?(uint8_t)(i&0xFF):0; break;
            case 5: buf[i]=((i/64)%2==0)?(uint8_t)(i&0xFF):(uint8_t)(i%3); break;
        }
    }
}

static void bench_one(const char *name, const uint8_t *data, uint64_t n)
{
    double v2old = (double)(n*64) / ((double)n * v2_old_ratio());
    printf("\n┌─ %-22s  %llu chunks / %llu B\n",
           name, (unsigned long long)n, (unsigned long long)(n*64));
    printf("│  v2_old pipeline       ratio=%.3fx\n", v2old);

    static const char *lnames[] = {
        "L0 single      ",
        "L1 batch/8     ",
        "L2 batch/64    ",
        "L3 batch/512   ",
    };

    for (uint8_t layer = 0; layer <= 3; layer++) {
        if (layer > 0 && n < 8) continue;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ShellMetrics m = shell_run_pipeline_v2(data, n, layer);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec-t0.tv_sec)*1e3
                  + (t1.tv_nsec-t0.tv_nsec)*1e-6;

        /* dominant rotation */
        uint8_t dom = 0;
        for (uint8_t r=1; r<SHELL_ROT_STATES; r++)
            if (m.rot_wins[r] > m.rot_wins[dom]) dom = r;

        /* rotation spread (non-zero wins) */
        int spread = 0;
        for (uint8_t r=0; r<SHELL_ROT_STATES; r++)
            if (m.rot_wins[r] > 0) spread++;

        double gain = (v2old > 0) ? m.ratio / v2old : 0;

        if (layer == 0) {
            printf("│  %s  ratio=%6.3fx  "
                   "flat=%llu sp=%llu dn=%llu  "
                   "isect_avg=%.1f  rot_spread=%d/6  dom=%d  "
                   "%.1fms  [%+.2fx]\n",
                   lnames[layer], m.ratio,
                   (unsigned long long)m.n_flat,
                   (unsigned long long)m.n_sparse,
                   (unsigned long long)m.n_dense,
                   (double)m.isect_total / (double)(n ? n : 1),
                   spread, dom, ms, gain);
        } else {
            printf("│  %s  ratio=%6.3fx  "
                   "batch=%llu  isect_acc=%.1f  dom=%d  "
                   "%.1fms  [%+.2fx]\n",
                   lnames[layer], m.ratio,
                   (unsigned long long)m.n_batch,
                   (double)m.isect_total / (double)(n ? n : 1),
                   dom, ms, gain);
        }
    }
    printf("└─\n");
}

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Diamond Shell v2 — fibo_intersect rotation discriminator║\n");
    printf("║  Bench: v2_new vs v2_old (bench_v2_realfiles baseline)   ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    const uint64_t N = 4096;
    uint8_t *buf = malloc(N * SHELL_CHUNK_SZ);

    struct { const char *name; int pat; } specs[] = {
        {"Text-like",          0},
        {"Structured binary",  1},
        {"Pseudo-random",      2},
        {"Repetitive",         3},
        {"Sparse bursts",      4},
        {"Mixed blocks",       5},
    };

    for (int i = 0; i < 6; i++) {
        gen_data(buf, N * SHELL_CHUNK_SZ, specs[i].pat);
        bench_one(specs[i].name, buf, N);
    }

    /* fibo timeline */
    printf("\nFibo timeline boundaries in 256-chunk window:\n");
    for (uint32_t z = 1; z < 256; z++) {
        uint8_t ph = _shell_fibo_phase(z);
        if (ph) {
            const char *lbl[] = {"","SIG/17","FLUSH/144","SNAP/720"};
            printf("  z=%3u → %-10s\n", z, lbl[ph]);
        }
    }

    /* rotation semantics */
    printf("\nRotation → Frustum face:\n");
    const char *rn[] = {
        "0 identity  (+identity, no re-index)",
        "1 +X face   (y→x, z→y, x→z)",
        "2 +Y face   (z→x, x→y, y→z)",
        "3 -Y face   (x→x, z→y, 3-y→z)",
        "4 -X face   (z→x, y→y, 3-x→z)",
        "5 -Z face   (3-y→x, x→y, z→z)",
    };
    for (int r=0; r<6; r++) printf("  rot=%s\n", rn[r]);

    free(buf);
    return 0;
}

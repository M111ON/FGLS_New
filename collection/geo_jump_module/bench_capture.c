/*
 * bench_capture.c — Benchmark: pentagon vs on-demand icosa capture
 *
 * Compares:
 *   PENTA: 10 sectors × 6 slots = 60 positions (existing)
 *   ICOSA: 20 faces × 15 f=4 vertices = 300 keys, 162 unique (on-demand)
 *
 * On-demand means: grid is computed from coordinates (face + barycentric +
 * round to f=4), not from pre-stored position tables.
 *
 * Metrics: μs/call, coverage, χ² uniformity, self-consistency.
 *
 * Build:
 *   gcc -O2 -std=c11 -Iinclude -o bench_capture.exe bench_capture.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#include "icosphere_capture.h"

/* ── Constants (mirror from tw_capture_int.h) ─────────────── */
#define TW_N_SECTORS  10
#define TW_SLOTS_PER   6
#define TW_SCALE    207360

static const int32_t BOUNDARY[TW_N_SECTORS][2] = {
    {0,207360},{121883,167758},{197211,64078},{197211,-64078},{121883,-167758},
    {0,-207360},{-121883,-167758},{-197211,-64078},{-197211,64078},{-121883,167758},
};
static const int32_t HEX_CENT[TW_N_SECTORS][TW_SLOTS_PER][2] = {
  {{0,238464},{-26937,222912},{-26937,191808},{0,176256},{26937,191808},{26937,222912}},
  {{140166,192922},{109232,196172},{90949,171009},{103601,142594},{134534,139342},{152817,164507}},
  {{226792,73689},{203678,94502},{174097,84890},{167629,54466},{190744,33653},{220326,43265}},
  {{226792,-73689},{220326,-43265},{190744,-33653},{167629,-54466},{174097,-84890},{203678,-94502}},
  {{140166,-192922},{152817,-164507},{134534,-139342},{103601,-142594},{90949,-171009},{109232,-196172}},
  {{0,-238464},{26937,-222912},{26937,-191808},{0,-176256},{-26937,-191808},{-26937,-222912}},
  {{-140166,-192922},{-109232,-196172},{-90949,-171009},{-103601,-142594},{-134534,-139342},{-152817,-164507}},
  {{-226792,-73689},{-203678,-94502},{-174097,-84890},{-167629,-54466},{-190744,-33653},{-220326,-43265}},
  {{-226792,73689},{-220326,43265},{-190744,33653},{-167629,54466},{-174097,84890},{-203678,94502}},
  {{-140166,192922},{-152817,164507},{-134534,139342},{-103601,142594},{-90949,171009},{-109232,196172}},
};

/* ── PRNG ─────────────────────────────────────────────────── */
static uint32_t xsr = 123456789;
static inline uint32_t rng_u32(void) {
    xsr ^= xsr << 13; xsr ^= xsr >> 17; xsr ^= xsr << 5; return xsr;
}
static inline double rng_unif(void) {
    return (double)rng_u32() / 4294967296.0;
}
static inline double rng_gauss(void) {
    double u = rng_unif(), v = rng_unif();
    return sqrt(-2.0 * log(u + 1e-30)) * cos(6.283185307 * v);
}

/* ── Timing ───────────────────────────────────────────────── */
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ══════════════════════════════════════════════════════════════
   METHOD: Pentagon — 60 positions
   ══════════════════════════════════════════════════════════════ */
static inline uint32_t method_penta(int64_t vx, int64_t vy) {
    int best_sec = 0;
    int64_t best_dot = INT64_MIN;
    for (int i = 0; i < TW_N_SECTORS; i++) {
        int64_t dot = vx * BOUNDARY[i][0] + vy * BOUNDARY[i][1];
        if (dot > best_dot) { best_dot = dot; best_sec = i; }
    }
    int64_t bd2 = INT64_MAX;
    int best_slot = 0;
    for (int j = 0; j < TW_SLOTS_PER; j++) {
        int64_t dx = vx - HEX_CENT[best_sec][j][0];
        int64_t dy = vy - HEX_CENT[best_sec][j][1];
        int64_t d2 = dx*dx + dy*dy;
        if (d2 < bd2) { bd2 = d2; best_slot = j; }
    }
    return best_sec * TW_SLOTS_PER + best_slot;
}

/* ══════════════════════════════════════════════════════════════
   METHOD: Icosa on-demand — up to 300 keys (162 unique positions)
   ══════════════════════════════════════════════════════════════ */
static inline uint32_t method_icosa(int64_t vx, int64_t vy) {
    return icosa_capture_on_demand(vx, vy);
}

/* ══════════════════════════════════════════════════════════════
   DETERMINISM CHECK
   ══════════════════════════════════════════════════════════════ */
/* Same input → same key? (must be 100%) */
static double test_determinism(int n_tests, int64_t *xs, int64_t *ys,
                                int *n_fail) {
    int fail = 0;
    for (int i = 0; i < n_tests; i++) {
        uint32_t k1 = method_icosa(xs[i], ys[i]);
        uint32_t k2 = method_icosa(xs[i], ys[i]);
        if (k1 != k2) fail++;
    }
    *n_fail = fail;
    return (double)(n_tests - fail) / n_tests * 100.0;
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */

typedef struct { int idx; int cnt; } IdxCnt;

static int idxcnt_cmp_desc(const void *a, const void *b) {
    int ca = ((const IdxCnt*)a)->cnt;
    int cb = ((const IdxCnt*)b)->cnt;
    return (ca < cb) - (ca > cb);
}

int main(int argc, char **argv) {
    int n_points = 500000;
    int mode = 1; /* 0=plane, 1=sphere, 2=gauss */
    double sigma = 0.15;

    if (argc > 1) n_points = atoi(argv[1]);
    if (argc > 2) mode = atoi(argv[2]);
    if (argc > 3) sigma = atof(argv[3]);

    printf("=== Capture Benchmark ===\n");
    printf("Points: %d  Mode: %s  sigma=%.3f\n\n",
           n_points,
           mode == 0 ? "plane uniform" :
           mode == 1 ? "sphere uniform" : "gaussian");

    /* Generate test data */
    int64_t *xs = malloc(n_points * sizeof(int64_t));
    int64_t *ys = malloc(n_points * sizeof(int64_t));
    if (!xs || !ys) { fprintf(stderr, "OOM\n"); return 1; }

    xsr = 123456789;
    if (mode == 0) {
        for (int i = 0; i < n_points; i++) {
            xs[i] = (int64_t)((rng_unif() * 2.0 - 1.0) * TW_SCALE);
            ys[i] = (int64_t)((rng_unif() * 2.0 - 1.0) * TW_SCALE);
        }
    } else if (mode == 1) {
        for (int i = 0; i < n_points; i++) {
            double theta = acos(2.0 * rng_unif() - 1.0);
            double phi   = 6.283185307 * rng_unif();
            double x = sin(theta) * cos(phi);
            double y = sin(theta) * sin(phi);
            double z = cos(theta);
            double denom = 1.0 + z;
            xs[i] = (int64_t)(x / denom * TW_SCALE * 0.8);
            ys[i] = (int64_t)(y / denom * TW_SCALE * 0.8);
        }
    } else {
        for (int i = 0; i < n_points; i++) {
            double gx = rng_gauss() * sigma;
            double gy = rng_gauss() * sigma;
            xs[i] = (int64_t)(gx * TW_SCALE);
            ys[i] = (int64_t)(gy * TW_SCALE);
            if (xs[i] > TW_SCALE) xs[i] = TW_SCALE;
            if (xs[i] < -TW_SCALE) xs[i] = -TW_SCALE;
            if (ys[i] > TW_SCALE) ys[i] = TW_SCALE;
            if (ys[i] < -TW_SCALE) ys[i] = -TW_SCALE;
        }
    }

    /* ── Determinism check ── */
    int n_det = n_points > 100000 ? 100000 : n_points;
    int n_fail_det;
    double det_pct = test_determinism(n_det, xs, ys, &n_fail_det);
    printf("Determinism (same→same): %.2f%%  disagree=%d/%d\n\n",
           det_pct, n_fail_det, n_det);

    /* ── Define methods ── */
    typedef struct {
        const char *name;
        uint32_t (*fn)(int64_t, int64_t);
        int n_pos;
        int n_keys;
    } Method;

    Method methods[] = {
        {"pentagon (60 slots)", method_penta, 60, 60},
        {"icosa od (300 keys)", method_icosa, 300, 162},
    };
    int n_m = 2;

    printf("%-22s %10s  Distribution (of keys)\n", "Method", "Speed");
    printf("------------------------------------------------------------\n");

    for (int m = 0; m < n_m; m++) {
        Method *md = &methods[m];
        int n_keys = md->n_pos; /* max keys to histogram */
        int *hist = calloc(n_keys, sizeof(int));

        /* Warmup */
        int n_warm = n_points < 1000 ? n_points : 1000;
        for (int i = 0; i < n_warm; i++) md->fn(xs[i], ys[i]);

        /* Timed run */
        double t0 = now_s();
        for (int i = 0; i < n_points; i++) {
            uint32_t p = md->fn(xs[i], ys[i]);
            if (p < (uint32_t)n_keys) hist[p]++;
        }
        double t1 = now_s();

        double avg_us = (t1 - t0) * 1e6 / n_points;

        /* Stats */
        int used = 0;
        for (int i = 0; i < n_keys; i++) if (hist[i] > 0) used++;

        /* χ² */
        double ideal = (double)n_points / n_keys;
        double chi2 = 0;
        for (int i = 0; i < n_keys; i++) {
            double dev = hist[i] - ideal;
            chi2 += dev * dev / ideal;
        }

        /* Gini */
        IdxCnt *ic = malloc(n_keys * sizeof(IdxCnt));
        for (int i = 0; i < n_keys; i++) { ic[i].idx = i; ic[i].cnt = hist[i]; }
        qsort(ic, n_keys, sizeof(IdxCnt), idxcnt_cmp_desc);

        double gini_num = 0, gini_den = 0;
        for (int i = 0; i < n_keys; i++) {
            gini_num += (double)(i + 1) * ic[i].cnt;
            gini_den += ic[i].cnt;
        }
        double gini = gini_den > 0
            ? (2.0 * gini_num / (n_keys * gini_den) - (double)(n_keys + 1) / n_keys)
            : 0;

        printf("%-22s %8.3f μs  cov=%5.1f%%(%d/%d) χ²=%9.1f Gini=%6.3f\n",
               md->name, avg_us,
               (double)used / md->n_pos * 100, used, md->n_pos,
               chi2, gini);

        /* Top 5 keys */
        printf("  top: ");
        for (int k = 0; k < 5 && k < n_keys; k++)
            printf("k%u=%d ", ic[k].idx, ic[k].cnt);
        printf("\n");

        if (m == 1) {
            printf("  unique keys used: %d\n", used);
        }

        free(ic);
        free(hist);
    }

    free(xs);
    free(ys);
    return 0;
}

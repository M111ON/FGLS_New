/* bench_priority_coverage.c — benchmark priority capture with Y-triangle node_id
 * Build:
 *   gcc -O2 -DGEO_JUMP_INLINE -I. -I./geo_jump_module/include -I./src -I../src
 *       -o tests/bench_priority_coverage.exe
 *       tests/bench_priority_coverage.c -lm
 * Run:   bench_priority_coverage.exe <tensors_dir>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#include "geom_raw_bridge.h"
#include "sid.h"
#include "geo_jump.h"

static int64_t now_us(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (cnt.QuadPart * 1000000LL) / freq.QuadPart;
}

int main(int argc, char **argv) {
    const char *tensors_dir = argc > 1 ? argv[1] : "../build/smollm2_tensors_raw";

    printf("═══ Priority Capture Coverage (Y-Triangle node_id) ═══\n\n");

    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    if (rb_load(&rb, tensors_dir) != 0) { printf("ERROR: cannot load\n"); return 1; }
    printf("  %u tensors loaded\n\n", rb.n_entries);

    int config_n[] = {1, 2, 3, 4, 5, 6, 7};
    const char *labels[] = {"1f x2", "2f x4", "3f x6", "4f x8", "5f x10", "6f x12", "7f x14"};

    printf("├────────┬──────────┬──────────┬──────────┬──────────┬──────────┤\n");
    printf("│ config │ coverage │ pent used│ pentagons │ time(ms) │ rt_ok    │\n");
    printf("├────────┼──────────┼──────────┼──────────┼──────────┼──────────┤\n");

    for (int ci = 0; ci < 7; ci++) {
        int n_faces = config_n[ci];
        SIDArchConfig cfg;
        cfg.n_faces    = (uint8_t)n_faces;
        cfg.use_tri    = 1;
        cfg.face_order = NULL;

        int n_ok = 0, n_total = 0;
        int pent_used[12] = {0};
        int n_rt_ok = 0;

        int64_t t_start = now_us();

        for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
            if (!rb.entries[i].occupied) continue;
            n_total++;

            SIDCoord fc0;
            if (sid_capture(rb.entries[i].data, rb.entries[i].size,
                            rb.entries[i].dtype, &fc0) != 0) continue;

            int64_t resid0 = (int64_t)fc0.resid_x * fc0.resid_x
                           + (int64_t)fc0.resid_y * fc0.resid_y;

            SIDCoord coord;
            if (sid_capture_with_config(rb.entries[i].data, rb.entries[i].size,
                                         rb.entries[i].dtype, cfg, &coord) != 0) continue;

            int64_t resid_best = (int64_t)coord.resid_x * coord.resid_x
                               + (int64_t)coord.resid_y * coord.resid_y;

            if (resid_best <= resid0) n_ok++;
            if (coord.node_id < GEO_FULL) pent_used[geo_pentagon_id(coord.node_id) - 1]++;

            /* Roundtrip: summon back and verify exact reconstruction */
            {
                int64_t svx, svy;
                sid_summon(&coord, &svx, &svy);
                int64_t vx, vy;
                if (rb.entries[i].dtype == 0)
                    sid_signature_f32(rb.entries[i].data, rb.entries[i].size, &vx, &vy);
                else
                    sid_signature_q80(rb.entries[i].data, rb.entries[i].size, &vx, &vy);
                if (svx == vx && svy == vy) n_rt_ok++;
            }
        }

        int64_t t_end = now_us();
        double elapsed_ms = (double)(t_end - t_start) / 1000.0;

        int n_pents = 0;
        for (int i = 0; i < 12; i++) if (pent_used[i]) n_pents++;

        double cov = 100.0 * n_ok / n_total;
        double plt = 100.0 * n_pents / 12.0;

        printf("│ %6s │ %8.1f%% │ %6d/%d │ %8.1f%% │ %8.1f │ %6d/%d │\n",
               labels[ci], cov, n_pents, 12, plt, elapsed_ms, n_rt_ok, n_total);
    }
    printf("├────────┴──────────┴──────────┴──────────┴──────────┴──────────┤\n");
    printf("│ Coverage: config resid ≤ basic resid (rotation-invariant).     │\n");
    printf("│ Pentagon: how many of 12 pentagons are occupied by captures.   │\n");
    printf("│ rt_ok: roundtrip exact match (summon back = original signature)│\n");
    printf("└──────────────────────────────────────────────────────────────┘\n");

    /* ── Speed comparison: basic capture vs config capture ── */
    printf("\n═══ Speed Comparison (single-pass capture) ═══\n\n");

    enum { N_ITERS = 100 };
    enum { MAX_SIGS = 512 };
    int64_t sigs[MAX_SIGS][2];
    int dtype_buf[MAX_SIGS];
    int n_sigs = 0;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES && n_sigs < MAX_SIGS; i++) {
        if (!rb.entries[i].occupied) continue;
        int64_t vx, vy;
        int rc = (rb.entries[i].dtype == 0)
            ? sid_signature_f32(rb.entries[i].data, rb.entries[i].size, &vx, &vy)
            : sid_signature_q80(rb.entries[i].data, rb.entries[i].size, &vx, &vy);
        if (rc == 0) {
            sigs[n_sigs][0] = vx;
            sigs[n_sigs][1] = vy;
            dtype_buf[n_sigs] = rb.entries[i].dtype;
            n_sigs++;
        }
    }

    /* Basic capture (n_faces=1): single pentagon, fastest */
    SIDArchConfig cfg_fast;
    cfg_fast.n_faces    = 1;
    cfg_fast.use_tri    = 0;
    cfg_fast.face_order = NULL;

    int64_t t0 = now_us();
    for (int iter = 0; iter < N_ITERS; iter++) {
        for (int s = 0; s < n_sigs; s++) {
            SIDCoord coord;
            sid_capture_with_config(rb.entries[s].data, rb.entries[s].size,
                                    dtype_buf[s], cfg_fast, &coord);
        }
    }
    int64_t t1 = now_us();

    /* Config capture (n_faces=4): 4 pentagons, typical quality */
    SIDArchConfig cfg_typ;
    cfg_typ.n_faces    = 4;
    cfg_typ.use_tri    = 0;
    cfg_typ.face_order = NULL;

    int64_t t2 = now_us();
    for (int iter = 0; iter < N_ITERS; iter++) {
        for (int s = 0; s < n_sigs; s++) {
            SIDCoord coord;
            sid_capture_with_config(rb.entries[s].data, rb.entries[s].size,
                                    dtype_buf[s], cfg_typ, &coord);
        }
    }
    int64_t t3 = now_us();

    double time_fast = (double)(t1 - t0) * 1000.0 / N_ITERS / n_sigs;
    double time_typ  = (double)(t3 - t2) * 1000.0 / N_ITERS / n_sigs;
    printf("  Basic (1 pentagon):  %.1f ns/tensor (%.0fK t/s)\n",
           time_fast, 1e6 / time_fast / 1000.0);
    printf("  Config (4 pentagons): %.1f ns/tensor (%.0fK t/s)\n",
           time_typ, 1e6 / time_typ / 1000.0);
    printf("  Overhead:            %.1f%%\n",
           100.0 * (time_typ / time_fast - 1.0));
    printf("  Note: %d tensors × %d iters\n", n_sigs, N_ITERS);

    rb_free(&rb);
    printf("\n═══ Done ═══\n");
    return 0;
}

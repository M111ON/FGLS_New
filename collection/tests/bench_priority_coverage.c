/* bench_priority_coverage.c — benchmark priority capture with real tri grid
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

static int64_t now_us(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (cnt.QuadPart * 1000000LL) / freq.QuadPart;
}

int main(int argc, char **argv) {
    const char *tensors_dir = argc > 1 ? argv[1] : "../build/smollm2_tensors_raw";

    printf("═══ Priority Capture Coverage (Real Tri Grid) ═══\n\n");

    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    if (rb_load(&rb, tensors_dir) != 0) { printf("ERROR: cannot load\n"); return 1; }
    printf("  %u tensors loaded\n\n", rb.n_entries);

    int config_n[] = {1, 2, 3, 4, 5, 6, 7};
    const char *labels[] = {"1f x2", "2f x4", "3f x6", "4f x8", "5f x10", "6f x12", "7f x14"};

    printf("├────────┬──────────┬──────────┬──────────┬──────────┬──────────┤\n");
    printf("│ config │ coverage │ tri used │  slots   │ time(ms) │ face0_ok │\n");
    printf("├────────┼──────────┼──────────┼──────────┼──────────┼──────────┤\n");

    for (int ci = 0; ci < 7; ci++) {
        int n_faces = config_n[ci];
        SIDArchConfig cfg;
        cfg.n_faces    = (uint8_t)n_faces;
        cfg.use_tri    = 1;
        cfg.face_order = NULL;

        int n_ok = 0, n_total = 0, n_tri = 0;
        int slot_used[1440] = {0};
        int n_face0_ok = 0;

        for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
            if (!rb.entries[i].occupied) continue;
            n_total++;

            int64_t vx, vy;
            if (rb.entries[i].dtype == 0) {
                if (sid_signature_f32(rb.entries[i].data, rb.entries[i].size, &vx, &vy) != 0) continue;
            } else {
                if (sid_signature_q80(rb.entries[i].data, rb.entries[i].size, &vx, &vy) != 0) continue;
            }

            TWFaceCapture fc0;
            tw_capture_face(vx, vy, 0, &fc0);
            int64_t resid0 = fc0.resid_x * fc0.resid_x + fc0.resid_y * fc0.resid_y;

            SIDCoord coord;
            if (sid_capture_with_config(rb.entries[i].data, rb.entries[i].size,
                                         rb.entries[i].dtype, cfg, &coord) != 0) continue;

            int64_t resid_best = (int64_t)coord.resid_x * coord.resid_x
                               + (int64_t)coord.resid_y * coord.resid_y;

            if (resid_best <= resid0) n_ok++;
            if (coord.is_tri) n_tri++;
            if (coord.tring_pos < 1440) slot_used[coord.tring_pos]++;

            /* Face-0 roundtrip (exact for face=0, expected quantization error for face≠0) */
            if (coord.face == 0) {
                int64_t svx, svy;
                sid_summon_legacy(&coord, &svx, &svy);
                if (svx == vx && svy == vy) n_face0_ok++;
            }
        }

        int n_slots = 0;
        for (int i = 0; i < 1440; i++) if (slot_used[i]) n_slots++;

        double cov = 100.0 * n_ok / n_total;
        double tri = 100.0 * n_tri / n_total;
        double slt = 100.0 * n_slots / 1440.0;

        printf("│ %6s │ %8.1f%% │ %8.1f%% │ %6d/%d │ %8s │ %8d │\n",
               labels[ci], cov, tri, n_slots, 1440, "-", n_face0_ok);
    }
    printf("├────────┴──────────┴──────────┴──────────┴──────────┴──────────┤\n");
    printf("│ Note: face=0 roundtrip is 100%% exact. face≠0 has integer     │\n");
    printf("│ rotation quantization (RCOS²+RSIN²≠SCALE²). Coverage metric   │\n");
    printf("│ compares resid magnitudes (rotation-invariant = always exact).│\n");
    printf("└──────────────────────────────────────────────────────────────┘\n");

    /* ── Speed comparison: virtual rotation vs real tri grid ── */
    printf("\n═══ Speed Comparison (290 tensors, n_faces=4) ═══\n\n");

    /* Collect sigs once */
    enum { MAX_SIGS = 512 };
    int64_t sigs[MAX_SIGS][2];
    int n_sigs = 0;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES && n_sigs < MAX_SIGS; i++) {
        if (!rb.entries[i].occupied) continue;
        int64_t vx, vy;
        if (rb.entries[i].dtype == 0) {
            if (sid_signature_f32(rb.entries[i].data, rb.entries[i].size, &vx, &vy) == 0)
                { sigs[n_sigs][0] = vx; sigs[n_sigs][1] = vy; n_sigs++; }
        } else {
            if (sid_signature_q80(rb.entries[i].data, rb.entries[i].size, &vx, &vy) == 0)
                { sigs[n_sigs][0] = vx; sigs[n_sigs][1] = vy; n_sigs++; }
        }
    }

    /* Collect sigs once */
    enum { MAX_SIGS = 512 };
    /* Virtual rotation approach (old): rotate 30° then hex capture */
    int64_t t0 = now_us();
    for (int iter = 0; iter < n_iters; iter++) {
        for (int s = 0; s < n_sigs; s++) {
            int64_t vx = sigs[s][0], vy = sigs[s][1];
            static const int32_t RC[12] = {207360,179580,103680,0,-103680,-179580,-207360,-179580,-103680,0,103680,179580};
            static const int32_t RS[12] = {0,103680,179580,207360,179580,103680,0,-103680,-179580,-207360,-179580,-103680};
            static const uint8_t PR[7] = {0,3,5,6,2,1,4};
            int64_t best_mag = -1;
            for (int p = 0; p < 4; p++) {
                uint8_t f = PR[p];
                int64_t rx = (vx * RC[f] - vy * RS[f]) / TW_SCALE;
                int64_t ry = (vx * RS[f] + vy * RC[f]) / TW_SCALE;
                TWCaptureInt hex; tw_capture_int(rx, ry, &hex);
                int64_t hm = hex.resid_x*hex.resid_x + hex.resid_y*hex.resid_y;
                if (best_mag < 0 || hm < best_mag) best_mag = hm;
                /* Virtual tri: 30° rotation then hex capture */
                int64_t tx = (rx * 179580 - ry * 103680) / TW_SCALE;
                int64_t ty = (rx * 103680 + ry * 179580) / TW_SCALE;
                TWCaptureInt tri; tw_capture_int(tx, ty, &tri);
                int64_t tm = tri.resid_x*tri.resid_x + tri.resid_y*tri.resid_y;
                if (tm < best_mag) best_mag = tm;
            }
        }
    }
    int64_t t1 = now_us();

    /* Real tri grid approach (new): no rotation, use tri table directly */
    int64_t t2 = now_us();
    for (int iter = 0; iter < n_iters; iter++) {
        for (int s = 0; s < n_sigs; s++) {
            int64_t vx = sigs[s][0], vy = sigs[s][1];
            static const int32_t RC[12] = {207360,179580,103680,0,-103680,-179580,-207360,-179580,-103680,0,103680,179580};
            static const int32_t RS[12] = {0,103680,179580,207360,179580,103680,0,-103680,-179580,-207360,-179580,-103680};
            static const uint8_t PR[7] = {0,3,5,6,2,1,4};
            int64_t best_mag = -1;
            for (int p = 0; p < 4; p++) {
                uint8_t f = PR[p];
                int64_t rx = (vx * RC[f] - vy * RS[f]) / TW_SCALE;
                int64_t ry = (vx * RS[f] + vy * RC[f]) / TW_SCALE;
                TWCaptureInt hex; tw_capture_int(rx, ry, &hex);
                int64_t hm = hex.resid_x*hex.resid_x + hex.resid_y*hex.resid_y;
                if (best_mag < 0 || hm < best_mag) best_mag = hm;
                /* Real tri: use tri grid directly, no rotation */
                TWCaptureInt tri; tw_capture_int_tri(rx, ry, &tri);
                int64_t tm = tri.resid_x*tri.resid_x + tri.resid_y*tri.resid_y;
                if (tm < best_mag) best_mag = tm;
            }
        }
    }
    int64_t t3 = now_us();

    double time_virtual = (double)(t1 - t0) * 1000.0 / n_iters / n_sigs;
    double time_real    = (double)(t3 - t2) * 1000.0 / n_iters / n_sigs;
    printf("  Virtual rotation (30° rotate + hex grid):  %.1f ns/tensor (%.0fK t/s)\n",
           time_virtual, 1e6 / time_virtual / 1000.0);
    printf("  Real tri grid (direct tri table):         %.1f ns/tensor (%.0fK t/s)\n",
           time_real, 1e6 / time_real / 1000.0);
    printf("  Speed-up:                                 %.1f%%\n",
           100.0 * (1.0 - time_real / time_virtual));
    printf("  Note: 8 directions (4 faces × hex+tri), %d tensors × %d iters\n",
           n_sigs, n_iters);

    rb_free(&rb);
    printf("\n═══ Done ═══\n");
    return 0;
}

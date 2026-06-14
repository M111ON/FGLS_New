/* bench_24dir_coverage.c — benchmark: face-0 vs 24-direction capture
 * ═══════════════════════════════════════════════════════════════════
 * Build:
 *   gcc -O2 -DGEO_JUMP_INLINE -I. -I../geo_jump_module/include
 *       -I../src -o tests/bench_24dir_coverage.exe
 *       tests/bench_24dir_coverage.c
 * Run:   bench_24dir_coverage.exe <tensors_dir>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#include "geom_raw_bridge.h"
#include "sid.h"

static int compare_tensors(const void *a, const void *b) {
    return strcmp(((SIDEntry*)a)->name, ((SIDEntry*)b)->name);
}

int main(int argc, char **argv) {
    const char *tensors_dir = argc > 1 ? argv[1] : "../build/smollm2_tensors_raw";

    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  24-Direction Coverage Benchmark\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    printf("Loading tensors from %s...\n", tensors_dir);
    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    if (rb_load(&rb, tensors_dir) != 0) {
        printf("ERROR: cannot load %s\n", tensors_dir);
        return 1;
    }
    printf("  %u tensors loaded\n\n", rb.n_entries);

    /* Capture each tensor with both methods */
    int n_total = 0, n_face0_lossless = 0, n_24_lossless = 0;
    int n_face0_better = 0, n_24_better = 0, n_tie = 0;
    int n_face0_lossy = 0, n_24_lossy = 0;
    __int128 total_resid_face0 = 0, total_resid_24 = 0;
    int64_t max_improvement = 0;
    char best_improv_name[256] = {0};

    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (!rb.entries[i].occupied) continue;
        n_total++;

        int64_t vx, vy;
        if (rb.entries[i].dtype == 0) {
            if (sid_signature_f32(rb.entries[i].data, rb.entries[i].size, &vx, &vy) != 0) continue;
        } else {
            if (sid_signature_q80(rb.entries[i].data, rb.entries[i].size, &vx, &vy) != 0) continue;
        }

        /* Face-0 capture */
        TWFaceCapture fc0;
        tw_capture_face(vx, vy, 0, &fc0);
        int64_t resid0 = fc0.resid_x * fc0.resid_x + fc0.resid_y * fc0.resid_y;

        /* 24-direction capture */
        TWFaceIter24 r24;
        tw_iterate_faces_24(vx, vy, &r24);
        int best = tw_best_of_24(&r24);
        int64_t resid24 = r24.caps[best].resid_x * r24.caps[best].resid_x
                        + r24.caps[best].resid_y * r24.caps[best].resid_y;

        total_resid_face0 += resid0;
        total_resid_24 += resid24;

        if (resid24 < resid0) {
            n_24_better++;
            int64_t improv = resid0 - resid24;
            if (improv > max_improvement) {
                max_improvement = improv;
                strncpy(best_improv_name, rb.entries[i].name, 255);
            }
        } else if (resid24 > resid0) {
            n_face0_better++;
        } else {
            n_tie++;
        }

        /* Count "high resid" cases (likely noisy capture) */
        if (resid0 > 1000000) n_face0_lossy++;
        if (resid24 > 1000000) n_24_lossy++;
    }

    /* Print summary */
    printf("─── Coverage Comparison ─────────────────────────────────────\n");
    printf("  Tensors:                      %d\n", n_total);
    printf("  24-dir better:                %d  (%.1f%%)\n",
           n_24_better, 100.0 * n_24_better / n_total);
    printf("  Face-0 better:                %d  (%.1f%%)\n",
           n_face0_better, 100.0 * n_face0_better / n_total);
    printf("  Tie (equal resid):            %d  (%.1f%%)\n",
           n_tie, 100.0 * n_tie / n_total);
    printf("  Face-0 high-resid (noisy):    %d  (%.1f%%)\n",
           n_face0_lossy, 100.0 * n_face0_lossy / n_total);
    printf("  24-dir high-resid (noisy):    %d  (%.1f%%)\n",
           n_24_lossy, 100.0 * n_24_lossy / n_total);
    printf("  Max improvement:              %lld (%s)\n",
           (long long)max_improvement, best_improv_name);

    /* Show distribution of best face */
    int face_hist[12] = {0};
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (!rb.entries[i].occupied) continue;
        int64_t vx, vy;
        if (rb.entries[i].dtype == 0) {
            if (sid_signature_f32(rb.entries[i].data, rb.entries[i].size, &vx, &vy) != 0) continue;
        } else {
            if (sid_signature_q80(rb.entries[i].data, rb.entries[i].size, &vx, &vy) != 0) continue;
        }
        TWFaceIter24 r24;
        tw_iterate_faces_24(vx, vy, &r24);
        int best = tw_best_of_24(&r24);
        face_hist[r24.caps[best].face]++;
    }

    printf("\n─── Best Face Distribution (24-dir) ────────────────────────\n");
    for (int f = 0; f < 12; f++) {
        printf("  face %2d: %4d (%5.1f%%)%s\n", f, face_hist[f],
               100.0 * face_hist[f] / n_total,
               face_hist[f] > n_total / 12 ? " ← above avg" : "");
    }

    printf("\n─── is_tri Distribution ────────────────────────────────────\n");
    int tri_hist[2] = {0};
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (!rb.entries[i].occupied) continue;
        int64_t vx, vy;
        if (rb.entries[i].dtype == 0) {
            if (sid_signature_f32(rb.entries[i].data, rb.entries[i].size, &vx, &vy) != 0) continue;
        } else {
            if (sid_signature_q80(rb.entries[i].data, rb.entries[i].size, &vx, &vy) != 0) continue;
        }
        TWFaceIter24 r24;
        tw_iterate_faces_24(vx, vy, &r24);
        int best = tw_best_of_24(&r24);
        tri_hist[r24.caps[best].is_tri]++;
    }
    printf("  hex centroids (is_tri=0):     %d (%.1f%%)\n",
           tri_hist[0], 100.0 * tri_hist[0] / n_total);
    printf("  tri centroids (is_tri=1):     %d (%.1f%%)\n",
           tri_hist[1], 100.0 * tri_hist[1] / n_total);

    /* TRing slot coverage */
    int slot_used[1440] = {0};
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (!rb.entries[i].occupied) continue;
        int64_t vx, vy;
        if (rb.entries[i].dtype == 0) {
            if (sid_signature_f32(rb.entries[i].data, rb.entries[i].size, &vx, &vy) != 0) continue;
        } else {
            if (sid_signature_q80(rb.entries[i].data, rb.entries[i].size, &vx, &vy) != 0) continue;
        }
        TWFaceIter24 r24;
        tw_iterate_faces_24(vx, vy, &r24);
        int best = tw_best_of_24(&r24);
        slot_used[r24.caps[best].tring_pos]++;
    }
    int n_slots_used = 0;
    for (int i = 0; i < 1440; i++) if (slot_used[i]) n_slots_used++;
    printf("\n─── TRing Slot Coverage ───────────────────────────────────\n");
    printf("  Slots used: %d / 1440 (%.1f%%)\n", n_slots_used,
           100.0 * n_slots_used / 1440);

    rb_free(&rb);
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  Done.\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    return 0;
}

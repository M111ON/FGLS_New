/* bench_24dir_coverage.c — benchmark: face-0 vs capo×12 capture
 * ═══════════════════════════════════════════════════════════════════
 * Build:
 *   gcc -O2 -DGEO_JUMP_INLINE -I. -I../geo_jump_module/include
 *       -I../src -o tests/bench_24dir_coverage.exe
 *       tests/bench_24dir_coverage.c
 * Run:   bench_24dir_coverage.exe <tensors_dir>
 *
 * Compares:
 *   face-0 capture (sid_capture)  — single best centroid → one node_id
 *   capo×12 (sid_capture_capo)   — same base capture routed to 12 pentagons
 *
 * Metrics:
 *   - Pentagon distribution: which pentagon gets the most face-0 captures
 *   - Node coverage: how many unique node_ids across all capo×12 outputs
 *   - Resid comparison: face-0 resid vs best-of-capo resid
 *   - Drain count: tensors near sector boundaries
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#include "geom_raw_bridge.h"
#define GEO_JUMP_INLINE
#include "geo_jump.h"
#include "sid.h"

int main(int argc, char **argv) {
    const char *tensors_dir = argc > 1 ? argv[1] : "../build/smollm2_tensors_raw";

    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Face-0 vs Capo×12 Coverage Benchmark\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    printf("Loading tensors from %s...\n", tensors_dir);
    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    if (rb_load(&rb, tensors_dir) != 0) {
        printf("ERROR: cannot load %s\n", tensors_dir);
        return 1;
    }
    printf("  %u tensors loaded\n\n", rb.n_entries);

    /* ── Stats accumulators ── */
    int n_total = 0;
    __int128 total_resid_face0 = 0, total_resid_capo_best = 0;
    int n_face0_better = 0, n_capo_better = 0, n_tie = 0;
    int64_t max_improvement = 0;
    char best_improv_name[256] = {0};
    int n_drain = 0;

    int pentagon_hist[13] = {0};       /* face-0 pentagon distribution, index 1..12 */
    int capo_best_pent_hist[13] = {0}; /* best-capo pentagon distribution */
    uint8_t node_used[(GEO_FULL + 7) / 8];
    memset(node_used, 0, sizeof(node_used));
    uint32_t max_node_id = 0;

    /* ── Per-tensor loop ── */
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (!rb.entries[i].occupied) continue;

        /* Face-0 capture: single best centroid → one node_id */
        SIDCoord coord0;
        if (sid_capture(rb.entries[i].data, rb.entries[i].size, rb.entries[i].dtype, &coord0) != 0)
            continue;
        n_total++;

        int64_t resid0 = coord0.resid_x * coord0.resid_x + coord0.resid_y * coord0.resid_y;
        int face0_pent = (int)geo_pentagon_id(coord0.node_id);
        pentagon_hist[face0_pent]++;
        if (coord0.drain) n_drain++;

        /* Capo×12 capture: 12 pentagon-routed nodes */
        uint32_t capo_nodes[12];
        sid_capture_capo(rb.entries[i].data, rb.entries[i].size, rb.entries[i].dtype, capo_nodes);

        /* All 12 capo nodes share the same base resid.
         * Track which pentagon each node lands on. */
        int best_capo_pent = 0;
        int64_t best_capo_resid = -1;
        for (int p = 0; p < 12; p++) {
            int64_t mag2 = coord0.resid_x * coord0.resid_x
                         + coord0.resid_y * coord0.resid_y;
            if (best_capo_resid < 0 || mag2 < best_capo_resid) {
                best_capo_resid = mag2;
                best_capo_pent = p + 1;  /* pentagon 1..12 */
            }
            /* Track all capo node_ids */
            uint32_t nid = capo_nodes[p];
            if (nid < GEO_FULL) node_used[nid / 8] |= (uint8_t)(1u << (nid % 8));
            if (nid > max_node_id) max_node_id = nid;
        }
        capo_best_pent_hist[best_capo_pent]++;

        total_resid_face0   += resid0;
        total_resid_capo_best += best_capo_resid;

        /* Compare face-0 vs best capo (resid is same for capo, but face-0
         * may differ if the best centroid differs from capo's base) */
        if (best_capo_resid < resid0) {
            n_capo_better++;
            int64_t improv = resid0 - best_capo_resid;
            if (improv > max_improvement) {
                max_improvement = improv;
                strncpy(best_improv_name, rb.entries[i].name, 255);
            }
        } else if (best_capo_resid > resid0) {
            n_face0_better++;
        } else {
            n_tie++;
        }
    }

    /* Count unique nodes used by capo×12 */
    uint32_t n_nodes_used = 0;
    for (uint32_t i = 0; i < GEO_FULL; i++)
        if (node_used[i / 8] & (1u << (i % 8))) n_nodes_used++;

    /* ── Print results ── */
    printf("─── Resid Comparison ───────────────────────────────────────\n");
    printf("  Tensors:                      %d\n", n_total);
    printf("  Face-0 avg resid²:            %.1f\n",
           n_total ? (double)(long long)total_resid_face0 / n_total : 0);
    printf("  Capo best avg resid²:         %.1f\n",
           n_total ? (double)(long long)total_resid_capo_best / n_total : 0);
    printf("  Capo better:                  %d  (%.1f%%)\n",
           n_capo_better, n_total ? 100.0 * n_capo_better / n_total : 0);
    printf("  Face-0 better:                %d  (%.1f%%)\n",
           n_face0_better, n_total ? 100.0 * n_face0_better / n_total : 0);
    printf("  Tie (equal resid):            %d  (%.1f%%)\n",
           n_tie, n_total ? 100.0 * n_tie / n_total : 0);
    printf("  Max improvement:              %lld (%s)\n",
           (long long)max_improvement, best_improv_name[0] ? best_improv_name : "-");
    printf("  Drain tensors:                %d  (%.1f%%)\n",
           n_drain, n_total ? 100.0 * n_drain / n_total : 0);

    printf("\n─── Pentagon Distribution (face-0) ──────────────────────────\n");
    for (int p = 1; p <= 12; p++) {
        printf("  pentagon %2d: %4d (%5.1f%%)%s\n", p, pentagon_hist[p],
               n_total ? 100.0 * pentagon_hist[p] / n_total : 0,
               pentagon_hist[p] > n_total / 12 ? "  <- above avg" : "");
    }

    printf("\n─── Pentagon Distribution (best capo) ──────────────────────\n");
    for (int p = 1; p <= 12; p++) {
        printf("  pentagon %2d: %4d (%5.1f%%)%s\n", p, capo_best_pent_hist[p],
               n_total ? 100.0 * capo_best_pent_hist[p] / n_total : 0,
               capo_best_pent_hist[p] > n_total / 12 ? "  <- above avg" : "");
    }

    printf("\n─── Node Coverage (capo×12) ────────────────────────────────\n");
    printf("  Unique node_ids used:         %u / %u (%.1f%%)\n",
           n_nodes_used, GEO_FULL,
           100.0 * n_nodes_used / GEO_FULL);
    printf("  Max node_id seen:             %u\n", max_node_id);
    printf("  Capo coverage factor:         %.1f nodes/tensor\n",
           n_total ? (double)n_nodes_used / n_total : 0);

    rb_free(&rb);
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  Done.\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    return 0;
}

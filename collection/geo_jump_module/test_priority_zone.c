#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "icosphere_capture.h"
#include "geo_field_icosphere.h"
#include "geo_jump.h"

int main(void) {
    printf("=== PRIORITY ZONE — 60 Trapezoid Towers ===\n\n");

    printf("Constants:\n");
    printf("  GEO_BLOCK             = %u  (4x4x3)\n", GEO_BLOCK);
    printf("  GEO_TOWER             = %u\n", GEO_TOWER);
    printf("  GEO_FULL              = %u\n", GEO_FULL);
    printf("  GF_PRIORITY_TOWER_NODES = %u  (= %u x %u)\n",
           GF_PRIORITY_TOWER_NODES, GEO_BLOCK, GEO_TOWER);
    printf("  GF_PRIORITY_TRAPEZOIDS  = %u\n", GF_PRIORITY_TRAPEZOIDS);
    printf("  GF_PRIORITY_TOTAL       = %u  (= %u x %u)\n",
           GF_PRIORITY_TOTAL, GF_PRIORITY_TRAPEZOIDS, GF_PRIORITY_TOWER_NODES);
    printf("  GF_PRIORITY_TOTAL / GEO_FULL = %u\n",
           GF_PRIORITY_TOTAL / GEO_FULL);
    printf("\n");

    /* ── 1. Check all 300 capture keys map to valid trapezoids ── */
    printf("--- 1. Capture key \xe2\x86\x92 trapezoid mapping ---\n");
    int trap_counts[60] = {0};
    int fails = 0;
    for (uint32_t k = 0; k < 300; k++) {
        uint8_t trap = icosphere_capture_key_to_trapezoid(k);
        if (trap >= 60) { fails++; printf("  FAIL: key %u -> trap %u\n", k, trap); }
        else trap_counts[trap]++;
    }
    if (fails) printf("  %d failures\n", fails);
    else       printf("  All 300 keys -> valid trapezoids (0..59)\n");

    /* Distribution stats */
    int min_c = 999, max_c = 0, empty = 0;
    for (int i = 0; i < 60; i++) {
        if (trap_counts[i] < min_c) min_c = trap_counts[i];
        if (trap_counts[i] > max_c) max_c = trap_counts[i];
        if (trap_counts[i] == 0) empty++;
    }
    printf("  Distribution: min=%d  max=%d  empty=%d  ideal=5\n", min_c, max_c, empty);

    /* Per-face breakdown */
    printf("\n  Per-face trapezoid usage:\n");
    for (int f = 0; f < 12; f++) {
        int total = 0;
        for (int e = 0; e < 5; e++) {
            int idx = f * 5 + e;
            total += trap_counts[idx];
        }
        printf("    face %2d: %d keys across 5 edges", f, total);
        if (total > 0) {
            printf(" (");
            for (int e = 0; e < 5; e++)
                printf("%s%d", e ? "," : "", trap_counts[f*5+e]);
            printf(")");
        }
        printf("\n");
    }

    /* ── 2. Check tower positions in range ── */
    printf("\n--- 2. Capture key \xe2\x86\x92 tower position ---\n");
    int pos_ok = 1;
    for (uint32_t k = 0; k < 300; k++) {
        uint32_t pos = icosphere_capture_key_to_tower_pos(k);
        if (pos >= GF_PRIORITY_TOWER_NODES) {
            printf("  FAIL: key %u -> pos %u (max %u)\n", k, pos, GF_PRIORITY_TOWER_NODES-1);
            pos_ok = 0;
        }
    }
    if (pos_ok) printf("  All 300 keys -> valid tower position (0..%u)\n", GF_PRIORITY_TOWER_NODES-1);

    /* Shell level distribution */
    int shell_counts[12] = {0};
    for (uint32_t k = 0; k < 300; k++) {
        uint32_t pos = icosphere_capture_key_to_tower_pos(k);
        uint32_t shell = pos / 576;
        if (shell < 12) shell_counts[shell]++;
    }
    printf("  Shell level distribution: ");
    for (int s = 0; s < 12; s++) printf("%d ", shell_counts[s]);
    printf("\n");

    /* ── 3. Cross-check: trapezoid + position → GEO_FULL ── */
    printf("\n--- 3. Trapezoid tower \xe2\x86\x92 GEO_FULL ---\n");
    uint32_t geo_nodes[300];
    int geo_ok = 1;
    for (uint32_t k = 0; k < 300; k++) {
        uint8_t trap = icosphere_capture_key_to_trapezoid(k);
        uint32_t pos = icosphere_capture_key_to_tower_pos(k);
        uint32_t gn = icosphere_trapezoid_to_geo_full(trap, pos);
        if (gn >= GEO_FULL) { geo_ok = 0; printf("  FAIL: key %u -> geo %u\n", k, gn); }
        geo_nodes[k] = gn;
    }
    if (geo_ok) printf("  All 300 -> valid GEO_FULL (0..%u)\n", GEO_FULL-1);

    /* Uniqueness */
    int dup = 0;
    for (int i = 0; i < 300; i++)
        for (int j = i+1; j < 300; j++)
            if (geo_nodes[i] == geo_nodes[j]) dup++;
    printf("  Collisions: %d\n", dup);

    /* Compare with existing icosphere_capture_key_to_geo_full */
    int same = 0, diff = 0;
    for (uint32_t k = 0; k < 300; k++) {
        uint32_t old_g = icosphere_capture_key_to_geo_full(k);
        if (geo_nodes[k] == old_g) same++; else diff++;
    }
    printf("  vs old mapping: same=%d  diff=%d\n", same, diff);

    /* ── 4. Sample: print first few keys' full path ── */
    printf("\n--- 4. Sample mapping (first 15 keys) ---\n");
    printf("  %4s %5s %9s %12s %8s\n", "key", "trap", "tower_pos", "geo_full", "old_geo");
    for (uint32_t k = 0; k < 15; k++) {
        uint8_t trap = icosphere_capture_key_to_trapezoid(k);
        uint32_t pos = icosphere_capture_key_to_tower_pos(k);
        uint32_t gn = icosphere_trapezoid_to_geo_full(trap, pos);
        uint32_t old_g = icosphere_capture_key_to_geo_full(k);
        printf("  %4u %5u (f%u,e%u) %9u %8u %8u\n",
               k, trap, gf_trap_face(trap), gf_trap_edge(trap), pos, gn, old_g);
    }

    /* ── 5. Priority zone total capacity ── */
    printf("\n--- 5. Capacity verification ---\n");
    printf("  60 towers x %u = %u\n", GF_PRIORITY_TOWER_NODES, GF_PRIORITY_TOTAL);
    printf("  GEO_FULL = %u\n", GEO_FULL);
    printf("  Ratio = %u / %u = %u (expected %u)\n",
           GF_PRIORITY_TOTAL, GEO_FULL, GF_PRIORITY_TOTAL/GEO_FULL,
           GF_PRIORITY_GEO_FULL_RATIO);
    printf("  geo_jump verification: 48 = GEO_BLOCK = %ux%ux%u\n",
           GEO_METATRON_COLS, GEO_METATRON_ROWS, GEO_METATRON_FLOORS);
    printf("  Each tower = %u x %u = %u geo_jump units\n",
           GEO_BLOCK, GEO_TOWER, GEO_BLOCK * GEO_TOWER);

    /* ── 6. Cold zone (mini pentagon at shell level 0) ── */
    printf("\n--- 6. Cold zone (shell level 0 mini pentagon bottom) ---\n");
    int cold_fails = 0;
    uint32_t cold_set[300];
    for (uint32_t k = 0; k < 300; k++) {
        uint8_t trap = icosphere_capture_key_to_trapezoid(k);
        uint32_t pos = icosphere_capture_key_to_tower_pos(k);
        uint32_t cn = icosphere_trapezoid_to_cold(trap, pos);
        if (cn >= GEO_FULL) { cold_fails++; printf("  FAIL: key %u -> cold %u\n", k, cn); }
        cold_set[k] = cn;
    }
    if (!cold_fails) printf("  All 300 -> valid cold zone (0..%u)\n", GEO_FULL-1);
    /* Cold zone should be at shell level 0 */
    int has_surface = 0;
    for (uint32_t k = 0; k < 300; k++) {
        uint32_t level = (cold_set[k] % 1728u) / 144u;
        if (level != 0) { has_surface++; break; }
    }
    printf("  Cold zone: %s (all at shell level 0)\n",
           has_surface ? "ERROR: not at level 0" : "OK");
    /* Collisions in cold zone */
    int cold_dup = 0;
    for (int i = 0; i < 300; i++)
        for (int j = i+1; j < 300; j++)
            if (cold_set[i] == cold_set[j]) cold_dup++;
    printf("  Cold zone collisions: %d (cold = 1728 slots, 300 keys)\n", cold_dup);

    /* ── 7. Capo rotation demonstration ── */
    printf("\n--- 7. Capo rotation (all 12 positions) ---\n");
    printf("  For key 0 (trap=%u, pos=%u):\n",
           icosphere_capture_key_to_trapezoid(0),
           icosphere_capture_key_to_tower_pos(0));
    uint32_t base_capo_geo[12];
    for (int capo = 0; capo < 12; capo++) {
        uint8_t trap = icosphere_capture_key_to_trapezoid(0);
        uint32_t pos = icosphere_capture_key_to_tower_pos(0);
        uint32_t gn = icosphere_trapezoid_to_geo_full_capo(trap, pos, (uint8_t)capo);
        uint32_t level = (gn % 1728u) / 144u;
        base_capo_geo[capo] = gn;
        printf("    capo=%2d: geo_full=%5u  shell=%2d  slot=%3d\n",
               capo, gn, level, gn % 144u);
    }
    /* Verify capo wraps: capo=0 should equal capo=12 */
    uint8_t trap0 = icosphere_capture_key_to_trapezoid(0);
    uint32_t pos0 = icosphere_capture_key_to_tower_pos(0);
    uint32_t capo12 = icosphere_trapezoid_to_geo_full_capo(trap0, pos0, 12);
    printf("    capo=12: geo_full=%u (match capo=0: %s)\n",
           capo12, capo12 == base_capo_geo[0] ? "YES" : "NO");
    /* Cold zone with capo: cold target changes */
    printf("\n  Cold zone shifts with capo (key 0):\n");
    for (int capo = 0; capo < 12; capo++) {
        uint8_t trap = icosphere_capture_key_to_trapezoid(0);
        uint32_t pos = icosphere_capture_key_to_tower_pos(0);
        uint32_t cn = icosphere_cold_for_capo(trap, pos, (uint8_t)capo);
        uint32_t level = (cn % 1728u) / 144u;
        printf("    capo=%2d: cold_geo=%5u  shell=%2d\n", capo, cn, level);
    }
    /* Capo distribution across all keys */
    int capo_collisions[12] = {0};
    for (int capo = 0; capo < 12; capo++) {
        uint32_t seen[300];
        for (uint32_t k = 0; k < 300; k++) {
            uint8_t trap = icosphere_capture_key_to_trapezoid(k);
            uint32_t pos = icosphere_capture_key_to_tower_pos(k);
            seen[k] = icosphere_trapezoid_to_geo_full_capo(trap, pos, (uint8_t)capo);
        }
        int dup = 0;
        for (int i = 0; i < 300; i++)
            for (int j = i+1; j < 300; j++)
                if (seen[i] == seen[j]) dup++;
        capo_collisions[capo] = dup;
    }
    printf("\n  Collisions across capo positions:\n  ");
    for (int capo = 0; capo < 12; capo++)
        printf(" %d", capo_collisions[capo]);
    printf("  (12 views, same 414720 tower entries, different geo slots)\n");

    /* ── 8. Honeycomb redundancy (30 paired trapezoids) ── */
    printf("\n--- 8. Honeycomb redundancy (30 fast-flip pairs) ---\n");
    /* Verify: each trap's pair's pair = original */
    int pair_ok = 1;
    for (int t = 0; t < 60; t++) {
        uint8_t mate = icosphere_trap_pair((uint8_t)t);
        uint8_t back = icosphere_trap_pair(mate);
        if (back != (uint8_t)t) { pair_ok = 0; printf("  PAIR FAIL: %d -> %d -> %d\n", t, mate, back); }
    }
    printf("  Bidirectional pairs: %s\n", pair_ok ? "ALL OK (60 → 30 pairs)" : "FAIL");
    /* Show sample pairs */
    printf("  Sample pairs (first 5):\n");
    int shown = 0;
    uint8_t paired_show[60] = {0};
    for (int t = 0; t < 60 && shown < 5; t++) {
        uint8_t mate = icosphere_trap_pair((uint8_t)t);
        if (!paired_show[t] && !paired_show[mate]) {
            paired_show[t] = paired_show[mate] = 1;
            printf("    pair %d: trap %2d (f%u,e%u) <--> trap %2d (f%u,e%u)\n",
                   shown, t, gf_trap_face((uint8_t)t), gf_trap_edge((uint8_t)t),
                   mate, gf_trap_face(mate), gf_trap_edge(mate));
            shown++;
        }
    }
    /* Verify: mirrored store produces different GEO_FULL addresses */
    printf("  Primary vs mirror geo_full for first 5 keys:\n");
    for (uint32_t k = 0; k < 5; k++) {
        uint8_t trap = icosphere_capture_key_to_trapezoid(k);
        uint32_t pos = icosphere_capture_key_to_tower_pos(k);
        uint32_t primary = icosphere_trapezoid_to_geo_full(trap, pos);
        uint32_t mirror  = icosphere_trap_fast_flip(trap, pos);
        printf("    key %2d: trap=%2d geo_pri=%5u geo_mir=%5u %s\n",
               k, trap, primary, mirror,
               (primary == mirror) ? "SAME (bad)" : "DIFF (ok)");
    }
    /* Fast-flip recovery: flip → flip = original */
    int flip_ok = 1;
    for (uint32_t k = 0; k < 300; k++) {
        uint8_t trap = icosphere_capture_key_to_trapezoid(k);
        uint32_t pos = icosphere_capture_key_to_tower_pos(k);
        uint32_t flip1 = icosphere_trap_fast_flip(trap, pos);
        uint8_t mate = icosphere_trap_pair(trap);
        uint32_t flip2 = icosphere_trap_fast_flip(mate, pos);
        if (flip2 != icosphere_trapezoid_to_geo_full(trap, pos)) { flip_ok = 0; break; }
    }
    printf("  Fast-flip recovery (double-flip = original): %s\n",
           flip_ok ? "OK" : "FAIL");

    printf("\n=== DONE ===\n");
    return 0;
}

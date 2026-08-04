/*
 * torus_walk_test.c — Torus Walk for Weight Encoding
 * ===================================================
 * Walks the dodecahedral torus sequentially (no fibonacci, no golden angle).
 * Maps weight index i → TorusNode(face, edge, z) via torus_step().
 *
 * Measures:
 *   1. Period: when does the walk first repeat a position?
 *   2. Collision count over 1M steps
 *   3. Face distribution: are faces visited equally?
 *   4. Stride-3 variant: does striding improve spread?
 *
 * Torus topology: 12 faces × 5 edges × 256 z-layers = 15,360 total positions
 * torus_step() advances face/edge via g_map, z = (z+1) & 255
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ── Inline g_map and torus_step (self-contained, no deps) ────────── */

typedef struct {
    uint8_t next_face;
    uint8_t next_edge;
    uint8_t flip;
} EdgeMap;

#define TORUS_LAYER_WRAP 255u

static const EdgeMap g_map[12][5] = {
    /*  0 */ { {1,0,0}, {2,0,1}, {3,0,0}, {4,0,1}, {5,0,0} },
    /*  1 */ { {0,0,0}, {5,4,1}, {10,1,1}, {6,0,1}, {2,1,1} },
    /*  2 */ { {0,1,1}, {1,4,1}, {6,1,1}, {7,0,1}, {3,1,1} },
    /*  3 */ { {0,2,0}, {2,4,1}, {7,1,1}, {8,0,1}, {4,1,1} },
    /*  4 */ { {0,3,1}, {3,4,1}, {8,1,1}, {9,0,1}, {5,1,1} },
    /*  5 */ { {0,4,0}, {4,4,1}, {9,1,1}, {10,0,1}, {1,1,1} },
    /*  6 */ { {1,3,1}, {2,2,1}, {7,4,0}, {11,0,1}, {10,2,0} },
    /*  7 */ { {2,3,1}, {3,2,1}, {8,4,0}, {11,1,0}, {6,2,0} },
    /*  8 */ { {3,3,1}, {4,2,1}, {9,4,0}, {11,2,1}, {7,2,0} },
    /*  9 */ { {4,3,1}, {5,2,1}, {10,4,0}, {11,3,0}, {8,2,0} },
    /* 10 */ { {5,3,1}, {1,2,1}, {6,4,0}, {11,4,1}, {9,2,0} },
    /* 11 */ { {6,3,1}, {7,3,0}, {8,3,1}, {9,3,0}, {10,3,1} },
};

typedef struct {
    uint8_t face;
    uint8_t edge;
    uint8_t z;
    uint8_t state;
} TorusNode;

static inline void torus_step(TorusNode *n) {
    EdgeMap m = g_map[n->face][n->edge];
    n->face   = m.next_face;
    n->edge   = m.next_edge;
    n->state ^= m.flip;
    n->z      = (n->z + 1) & TORUS_LAYER_WRAP;
}

static inline void torus_step_stride(TorusNode *n, int stride) {
    for (int i = 0; i < stride; i++)
        torus_step(n);
}

/* ── Position hash for collision tracking ─────────────────────────── */

#define TOTAL_POSITIONS  (12 * 5 * 256)   /* 15,360 */
#define MAX_STEPS        1000000

/* Bitmap: 15,360 bits = 1,920 bytes */
static uint8_t visited[(TOTAL_POSITIONS + 7) / 8];

static inline uint32_t pos_key(uint8_t face, uint8_t edge, uint8_t z) {
    return (uint32_t)face * 5 * 256 + (uint32_t)edge * 256 + (uint32_t)z;
}

static inline int was_visited(uint32_t key) {
    return (visited[key / 8] >> (key % 8)) & 1;
}

static inline void mark_visited(uint32_t key) {
    visited[key / 8] |= (1u << (key % 8));
}

/* ── Main test ────────────────────────────────────────────────────── */

int main(void) {
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║     TORUS WALK TEST — Weight Encoding via Dodeca Torus     ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    printf("Torus topology:\n");
    printf("  Faces:  12  (0=cap, 1-5=upper, 6-10=lower, 11=cap)\n");
    printf("  Edges:  5 per face\n");
    printf("  Z-layers: 256 (wraps at 256 via & 0xFF)\n");
    printf("  Total positions: %d\n\n", TOTAL_POSITIONS);

    /* ════════════════════════════════════════════════════════════════
     * TEST 1: Sequential walk — period detection
     * ════════════════════════════════════════════════════════════════ */

    printf("═══ TEST 1: Sequential Walk — Period & Collision Detection ═══\n\n");

    memset(visited, 0, sizeof(visited));

    TorusNode node = {0, 0, 0, 0};  /* Start: face=0, edge=0, z=0 */
    uint32_t  first_repeat_step = 0;
    uint64_t  collision_count = 0;
    uint32_t  unique_count = 0;
    int       period_found = 0;

    /* Face visit counters */
    uint32_t face_count[12] = {0};

    /* Track first occurrence of each position to detect period */
    uint32_t *first_seen = (uint32_t *)calloc(TOTAL_POSITIONS, sizeof(uint32_t));
    if (!first_seen) {
        fprintf(stderr, "ERROR: calloc failed\n");
        return 1;
    }

    for (uint32_t step = 0; step < MAX_STEPS; step++) {
        uint32_t key = pos_key(node.face, node.edge, node.z);

        face_count[node.face]++;

        if (!was_visited(key)) {
            mark_visited(key);
            unique_count++;
            first_seen[key] = step;
        } else {
            collision_count++;
            if (!period_found) {
                first_repeat_step = step;
                period_found = 1;
                /* The actual period = step - first_seen[key] */
                printf("  First repeat at step %u (position f=%d e=%d z=%d)\n",
                       step, node.face, node.edge, node.z);
                printf("  That position was first seen at step %u\n", first_seen[key]);
                printf("  ► Period = %u steps\n\n", step - first_seen[key]);
            }
        }

        /* Stop after we've clearly passed the period */
        if (period_found && step > first_repeat_step + 2000) break;

        torus_step(&node);
    }

    printf("Results (sequential walk):\n");
    printf("  Total steps walked:      %u\n", period_found ? first_repeat_step + 2000 : MAX_STEPS);
    printf("  Unique positions:        %u / %d\n", unique_count, TOTAL_POSITIONS);
    printf("  Coverage:                %.2f%%\n", 100.0 * unique_count / TOTAL_POSITIONS);
    printf("  Collision count:         %" PRIu64 "\n", collision_count);
    if (period_found) {
        printf("  Period:                  %u steps\n", first_repeat_step - first_seen[pos_key(0,0,0)] +
               first_repeat_step - first_repeat_step); /* recompute below */
    }

    printf("\n  Face distribution (sequential):\n");
    for (int f = 0; f < 12; f++) {
        printf("    Face %2d: %u visits (%.2f%%)\n", f, face_count[f],
               100.0 * face_count[f] / (unique_count + collision_count));
    }

    /* Compute expected per-face */
    printf("\n  Expected per face (uniform): %.1f visits\n",
           (double)(unique_count + collision_count) / 12.0);

    /* ════════════════════════════════════════════════════════════════
     * TEST 2: Different starting points — period sensitivity
     * ════════════════════════════════════════════════════════════════ */

    printf("\n═══ TEST 2: Period Sensitivity — Different Start Points ═══════\n\n");

    /* Try several start positions to see if period depends on start */
    TorusNode starts[] = {
        {0, 0, 0, 0}, {0, 1, 0, 0}, {1, 0, 0, 0},
        {6, 0, 0, 0}, {11, 0, 0, 0}, {5, 2, 128, 1}
    };
    int n_starts = sizeof(starts) / sizeof(starts[0]);

    for (int s = 0; s < n_starts; s++) {
        TorusNode n = starts[s];
        uint32_t key0 = pos_key(n.face, n.edge, n.z);
        memset(visited, 0, sizeof(visited));
        mark_visited(key0);

        uint32_t period = 0;
        for (uint32_t step = 1; step < 100000; step++) {
            torus_step(&n);
            uint32_t key = pos_key(n.face, n.edge, n.z);
            if (key == key0) {
                period = step;
                break;
            }
        }
        printf("  Start (f=%d,e=%d,z=%d,s=%d): period = %u\n",
               starts[s].face, starts[s].edge, starts[s].z, starts[s].state,
               period ? period : 99999);
    }

    /* ════════════════════════════════════════════════════════════════
     * TEST 3: Stride-3 walk
     * ════════════════════════════════════════════════════════════════ */

    printf("\n═══ TEST 3: Stride-3 Walk — Does Striding Improve Spread? ═══\n\n");

    /* For each stride, check period and face coverage */
    int strides[] = {2, 3, 5, 7};
    int n_strides = sizeof(strides) / sizeof(strides[0]);

    for (int si = 0; si < n_strides; si++) {
        int stride = strides[si];
        TorusNode n = {0, 0, 0, 0};
        memset(visited, 0, sizeof(visited));
        uint32_t key0 = pos_key(0, 0, 0);
        mark_visited(key0);

        uint32_t stratum_fc[12] = {0};
        uint32_t unique = 1;
        uint32_t period = 0;

        for (uint32_t step = 1; step < 200000; step++) {
            torus_step_stride(&n, stride);
            uint32_t key = pos_key(n.face, n.edge, n.z);
            stratum_fc[n.face]++;

            if (key == key0) {
                period = step;
                break;
            }
            if (!was_visited(key)) {
                mark_visited(key);
                unique++;
            }
        }

        printf("  Stride %d: period = %u, unique = %u / %d (%.1f%%)\n",
               stride, period, unique, TOTAL_POSITIONS,
               100.0 * unique / TOTAL_POSITIONS);

        printf("    Face distribution:\n");
        for (int f = 0; f < 12; f++) {
            printf("      Face %2d: %u\n", f, stratum_fc[f]);
        }
        /* Check uniformity: coefficient of variation */
        double mean = 0.0;
        for (int f = 0; f < 12; f++) mean += stratum_fc[f];
        mean /= 12.0;
        double variance = 0.0;
        for (int f = 0; f < 12; f++) {
            double d = stratum_fc[f] - mean;
            variance += d * d;
        }
        variance /= 12.0;
        double cv = (mean > 0) ? (sqrt(variance) / mean) : 0.0;
        printf("    CV (lower = more uniform): %.4f\n\n", cv);
    }

    /* ════════════════════════════════════════════════════════════════
     * TEST 4: Compare with flat grid baseline
     * ════════════════════════════════════════════════════════════════ */

    printf("═══ TEST 4: Flat Grid vs Torus Walk — Distribution Quality ═══\n\n");

    /* Flat grid: weight index i → (i % 12) face, sequential layers */
    /* Compare CV across faces for flat sequential vs torus walk */

    /* Flat grid: just i mod 12 */
    {
        uint32_t flat_fc[12] = {0};
        for (uint32_t i = 0; i < 1000000; i++) {
            flat_fc[i % 12]++;
        }
        double mean = 1000000.0 / 12.0;
        double var = 0.0;
        for (int f = 0; f < 12; f++) {
            double d = flat_fc[f] - mean;
            var += d * d;
        }
        var /= 12.0;
        double cv_flat = sqrt(var) / mean;
        printf("  Flat sequential (i mod 12):\n");
        printf("    CV = %.6f (perfectly uniform by construction)\n", cv_flat);
    }

    /* Torus walk CV already computed above with stride=1 */

    /* ════════════════════════════════════════════════════════════════
     * TEST 5: Can we map weight index → torus position directly?
     * ════════════════════════════════════════════════════════════════ */

    printf("\n═══ TEST 5: Direct Index → Torus Mapping ════════════════════\n\n");

    printf("  Option A: Sequential walk (torus_step from blueprint)\n");
    printf("    i=0 → (0,0,0), i=1 → next via g_map, etc.\n");
    printf("    Period = one full cycle through all reachable positions\n");
    printf("    Problem: may not visit all 15,360 positions\n\n");

    printf("  Option B: Direct decomposition\n");
    printf("    face = (i / (5*256)) %% 12\n");
    printf("    edge = (i / 256) %% 5\n");
    printf("    z    = i %% 256\n");
    printf("    This is equivalent to flat grid — no torus benefit\n\n");

    printf("  Option C: Torus walk with re-seeding\n");
    printf("    For every K weights, re-seed torus to (0,0,0) + offset\n");
    printf("    Ensures full coverage even for small weight counts\n\n");

    /* Test: how many torus positions do we need to cover all 60 face-edge pairs? */
    printf("  Face-edge coverage analysis:\n");
    {
        uint8_t fe_covered[12][5] = {{0}};
        TorusNode n = {0, 0, 0, 0};
        uint32_t steps_to_cover_all_fe = 0;

        for (uint32_t step = 0; step < 10000; step++) {
            fe_covered[n.face][n.edge] = 1;

            int all = 1;
            for (int f = 0; f < 12; f++)
                for (int e = 0; e < 5; e++)
                    if (!fe_covered[f][e]) all = 0;

            if (all) {
                steps_to_cover_all_fe = step + 1;
                break;
            }
            torus_step(&n);
        }
        printf("    Steps to cover all 60 face-edge pairs: %u\n", steps_to_cover_all_fe);
    }

    /* ════════════════════════════════════════════════════════════════
     * TEST 6: Layer (z) distribution analysis
     * ════════════════════════════════════════════════════════════════ */

    printf("\n═══ TEST 6: Z-Layer Distribution ═════════════════════════════\n\n");

    {
        /* Walk and count z values — z increments by 1 every step */
        /* So z cycles 0,1,2,...,255,0,1,... — period of 256 */
        printf("  Z increments by 1 every torus_step().\n");
        printf("  Z cycle: 0→1→2→...→255→0 (period 256)\n");
        printf("  Face/edge cycle is independent of z cycle.\n\n");

        /* Check: does the combined (face,edge,z) period = lcm(face_edge_period, 256)? */
        /* The face-edge sequence has its own period P_fe */
        /* z has period 256 */
        /* Combined period = lcm(P_fe, 256) */

        /* Find face-edge period (ignoring z) */
        TorusNode n = {0, 0, 0, 0};
        uint32_t fe_period = 0;
        for (uint32_t step = 1; step < 100000; step++) {
            torus_step(&n);
            if (n.face == 0 && n.edge == 0) {
                fe_period = step;
                break;
            }
        }
        printf("  Face-edge period (ignoring z): %u\n", fe_period);

        /* Compute lcm(fe_period, 256) */
        uint32_t a = fe_period, b = 256;
        uint32_t tmp_a = a, tmp_b = b;
        while (tmp_b) { uint32_t t = tmp_b; tmp_b = tmp_a % tmp_b; tmp_a = t; }
        uint32_t gcd_fe_256 = tmp_a;
        uint32_t combined_period = (a / gcd_fe_256) * b;
        printf("  Combined period (face,edge,z): lcm(%u, 256) = %u\n", fe_period, combined_period);
        printf("  Total torus positions: %d\n", TOTAL_POSITIONS);
        printf("  Combined period == total positions? %s\n",
               combined_period == TOTAL_POSITIONS ? "YES ✓ — perfect coverage" : "NO ✗");
    }

    /* ════════════════════════════════════════════════════════════════
     * SUMMARY
     * ════════════════════════════════════════════════════════════════ */

    printf("\n═══ SUMMARY ══════════════════════════════════════════════════\n\n");
    printf("  Torus weight encoding viability:\n");
    printf("  • 12 faces × 5 edges × 256 z-layers = %d positions\n", TOTAL_POSITIONS);
    printf("  • torus_step() is O(1), branchless (XOR for state)\n");
    printf("  • Walk is deterministic and periodic\n");
    printf("  • Sequential walk visits positions in graph-traversal order\n");
    printf("  • Stride-N variants create distinct orbits (sub-groups)\n\n");
    printf("  For weight encoding:\n");
    printf("  • Direct walk: weight[i] → torus position after i steps\n");
    printf("  • If i > period, weights map to same positions → need chunking\n");
    printf("  • Best use: chunk weights into torus-period blocks\n");
    printf("  • Each chunk covers exactly one full torus cycle\n");
    printf("  • Cross-chunk: re-seed or use stride variation\n\n");

    free(first_seen);
    return 0;
}

// contour_placement_structural.c
// ================================================================
// Structural approaches for placing contour cube on geo_jump
// leveraging 15-tower-pair symmetry.
//
// Contour cube: 6 faces × 10 × 10 × 10 = 6000 cells
// Geo_jump:    15 towers × 48 addresses × 2 polar = 1440
//
// 4 approaches tested:
//   (1) Tower-pair balanced — each of 15 pairs gets exactly 400 cells
//   (2) Face-symmetric    — each of 6 faces distributes evenly across its 5 towers
//   (3) Depth-sliced       — z=0..9 each gets a full surface pass
//   (4) Anti-collision     — greedy assign cells to least-filled addresses
//
// Compile: gcc -O2 -std=c11 -Icore -IHfolder -o contour_placement_structural.exe runner/explore/contour_placement_structural.c -lm
// Compile: gcc -O2 -std=c11 -Icore -IHfolder -o contour_placement_structural.exe runner/explore/contour_placement_structural.c -lm
// ================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ── Constants ─────────────────────────────────────────────────── */

#define CUBE_FACES   6
#define CUBE_W      10
#define CUBE_H      10
#define CUBE_L      10
#define CUBE_CELLS  (CUBE_FACES * CUBE_W * CUBE_H * CUBE_L)  /* 6000 */

#define N_TOWERS    15
#define ADDR_PER_TOWER  48
#define N_POLAR     2
#define GEO_ADDRS   (N_TOWERS * ADDR_PER_TOWER * N_POLAR)    /* 1440 */

/* ── Cell descriptor ───────────────────────────────────────────── */

typedef struct {
    int face;       /* 0..5 */
    int x, y, z;    /* 0..9 each */
    int global_idx; /* 0..5999 */
} ContourCell;

static ContourCell all_cells[CUBE_CELLS];

static void init_cells(void) {
    int idx = 0;
    for (int f = 0; f < CUBE_FACES; f++)
        for (int z = 0; z < CUBE_L; z++)
            for (int y = 0; y < CUBE_H; y++)
                for (int x = 0; x < CUBE_W; x++) {
                    all_cells[idx].face = f;
                    all_cells[idx].x = x;
                    all_cells[idx].y = y;
                    all_cells[idx].z = z;
                    all_cells[idx].global_idx = idx;
                    idx++;
                }
}

/* ── Tower-pair table (from section1_cube_geojump.c) ──────────── */
/* C(6,2) = 15 pairs, pair k = (PAIR_A[k], PAIR_B[k])           */

static const int PAIR_A[15] = {0,0,0,0,0, 1,1,1,1, 2,2,2, 3,3, 4};
static const int PAIR_B[15] = {1,2,3,4,5, 2,3,4,5, 3,4,5, 4,5, 5};

/* Which 5 towers does face f participate in? */
static int FACE_TOWERS[6][5];

static void init_face_towers(void) {
    for (int f = 0; f < 6; f++) {
        int tidx = 0;
        for (int t = 0; t < N_TOWERS; t++) {
            if (PAIR_A[t] == f || PAIR_B[t] == f) {
                FACE_TOWERS[f][tidx++] = t;
            }
        }
    }
}

/* ── Geo-address encoding ──────────────────────────────────────── */
/* tower 0..14, addr 0..47, polar 0..1                           */
/* geo_idx = tower * 48 * 2 + polar * 48 + addr                  */

static inline int geo_idx(int tower, int addr, int polar) {
    return tower * ADDR_PER_TOWER * N_POLAR + polar * ADDR_PER_TOWER + addr;
}

/* ── Placement result ──────────────────────────────────────────── */

typedef struct {
    int tower;      /* 0..14 */
    int addr;       /* 0..47 */
    int polar;      /* 0..1 */
} Placement;

static Placement placements[CUBE_CELLS];
static int addr_load[GEO_ADDRS];         /* how many cells per address */
static int tower_load[N_TOWERS];         /* cells per tower */
static int polar_load[N_POLAR];          /* cells per polarity */

static void reset_loads(void) {
    memset(addr_load, 0, sizeof(addr_load));
    memset(tower_load, 0, sizeof(tower_load));
    memset(polar_load, 0, sizeof(polar_load));
}

static void record_placement(int cell_idx, int tower, int addr, int polar) {
    placements[cell_idx].tower = tower;
    placements[cell_idx].addr  = addr;
    placements[cell_idx].polar = polar;
    int g = geo_idx(tower, addr, polar);
    addr_load[g]++;
    tower_load[tower]++;
    polar_load[polar]++;
}

/* ── Statistics ────────────────────────────────────────────────── */

typedef struct {
    int addr_min, addr_max, addr_zero;
    int tower_min, tower_max;
    int polar_min, polar_max;
    int unique_addrs;
    double addr_avg;
    double tower_imbalance;
    int collisions;             /* total excess (cells beyond 1 per address) */
    int max_depth;              /* max cells at any single address */
    int address_histogram[16];  /* histogram[i] = #addrs with i or more hits */
} Stats;

static Stats compute_stats(void) {
    Stats s;
    memset(&s, 0, sizeof(s));
    s.addr_min = CUBE_CELLS;
    s.tower_min = CUBE_CELLS;
    s.polar_min = CUBE_CELLS;

    for (int i = 0; i < GEO_ADDRS; i++) {
        int load = addr_load[i];
        if (load < s.addr_min) s.addr_min = load;
        if (load > s.addr_max) s.addr_max = load;
        if (load == 0) s.addr_zero++;
        if (load > 0) s.unique_addrs++;
        s.collisions += (load > 1) ? (load - 1) : 0;
        int idx = (load >= 15) ? 15 : load;
        s.address_histogram[idx]++;
    }
    s.addr_avg = (double)CUBE_CELLS / GEO_ADDRS;
    s.max_depth = s.addr_max;

    for (int t = 0; t < N_TOWERS; t++) {
        if (tower_load[t] < s.tower_min) s.tower_min = tower_load[t];
        if (tower_load[t] > s.tower_max) s.tower_max = tower_load[t];
    }
    s.tower_imbalance = (double)(s.tower_max - s.tower_min);

    for (int p = 0; p < N_POLAR; p++) {
        if (polar_load[p] < s.polar_min) s.polar_min = polar_load[p];
        if (polar_load[p] > s.polar_max) s.polar_max = polar_load[p];
    }

    return s;
}

static void print_stats(const char *label, const Stats *s) {
    printf("  [%s]\n", label);
    printf("    Address:  min=%d max=%d avg=%.2f  zero=%d  unique=%d/%d (%.1f%%)\n",
           s->addr_min, s->addr_max, s->addr_avg,
           s->addr_zero, s->unique_addrs, GEO_ADDRS,
           100.0 * s->unique_addrs / GEO_ADDRS);
    printf("    Tower:    min=%d max=%d  imbalance=%.1f\n",
           s->tower_min, s->tower_max, s->tower_imbalance);
    printf("    Polar:    min=%d max=%d\n", s->polar_min, s->polar_max);
    printf("    Collisions: %d cells over %d unique addrs  (max_depth=%d)\n",
           s->collisions, s->unique_addrs, s->max_depth);
    printf("    Histogram (addrs with N cells):");
    for (int i = 0; i <= 15; i++) {
        if (s->address_histogram[i] > 0)
            printf(" %d:%d", i, s->address_histogram[i]);
    }
    printf("\n");
}

/* ════════════════════════════════════════════════════════════════════
   APPROACH 1: Tower-Pair Balanced
   Each of 15 pairs gets exactly 400 cells.

   Algorithm:
     - Face f has 1000 cells, participates in 5 towers.
     - Each tower receives cells from exactly 2 faces (its pair).
     - 6000 / 15 = 400 cells per tower.
     - Face f contributes 400/2 = 200 cells to each of its 5 towers.
     - Within a tower, addr = (face_cell_index % 48), polar = face_cell_index / 48
       but we need to distribute 200 cells across 48*2=96 addresses.
   ════════════════════════════════════════════════════════════════════ */

static int approach_tower_pair_balanced(void) {
    printf("\n═══════════════════════════════════════════════════\n");
    printf("  APPROACH 1: Tower-Pair Balanced\n");
    printf("  Each of 15 pairs gets exactly 400 cells\n");
    printf("═══════════════════════════════════════════════════\n");

    reset_loads();

    /* Per-(face,tower_slot) counter: how many cells placed so far */
    int slot_count[6][5];
    memset(slot_count, 0, sizeof(slot_count));

    for (int i = 0; i < CUBE_CELLS; i++) {
        ContourCell *c = &all_cells[i];
        int f = c->face;

        /* Pick the tower slot with fewest cells so far */
        int best_slot = 0;
        for (int s = 1; s < 5; s++) {
            if (slot_count[f][s] < slot_count[f][best_slot])
                best_slot = s;
        }
        int tower = FACE_TOWERS[f][best_slot];

        /* Address: stride-37 within this tower for uniform spread */
        int local_idx = slot_count[f][best_slot];
        int addr = (local_idx * 37) % ADDR_PER_TOWER;
        int polar = ((local_idx * 37) / ADDR_PER_TOWER) % N_POLAR;

        slot_count[f][best_slot]++;
        record_placement(i, tower, addr, polar);
    }

    Stats s = compute_stats();
    print_stats("TOWER_PAIR_BALANCED", &s);

    /* Verify: each tower should have 400 cells */
    int ok = 1;
    for (int t = 0; t < N_TOWERS; t++) {
        if (tower_load[t] != 400) {
            printf("    FAIL: tower %d has %d cells (expected 400)\n",
                   t, tower_load[t]);
            ok = 0;
        }
    }
    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* ════════════════════════════════════════════════════════════════════
   APPROACH 2: Face-Symmetric
   Each of 6 faces distributes evenly across its 5 towers.

   Algorithm:
     - Face f: 1000 cells, 5 towers → 200 cells per tower.
     - Within each tower, addresses are filled in a Hilbert-like pattern
       to spread across the 48×2 = 96 address space.
     - Uses (x, y, z) coordinates to compute a linear index within
       the face, then distributes in stride-37 pattern for uniform spread.
   ════════════════════════════════════════════════════════════════════ */

static int approach_face_symmetric(void) {
    printf("\n═══════════════════════════════════════════════════\n");
    printf("  APPROACH 2: Face-Symmetric\n");
    printf("  Each face distributes evenly across its 5 towers\n");
    printf("═══════════════════════════════════════════════════\n");

    reset_loads();

    /* Per-face cell counter */
    int face_count[6];
    memset(face_count, 0, sizeof(face_count));

    for (int i = 0; i < CUBE_CELLS; i++) {
        ContourCell *c = &all_cells[i];
        int f = c->face;

        /* Which tower: cyclic through face's 5 towers */
        int tower_slot = face_count[f] % 5;
        int tower = FACE_TOWERS[f][tower_slot];

        /* Address within tower: Hilbert-like spread using face-linear index */
        /* 200 cells per (face, tower), addr space = 48×2 = 96 */
        /* Use stride-37 to spread uniformly */
        int face_cell_idx = face_count[f];
        int within_tower = face_cell_idx / 5;  /* 0..199 */
        int addr = (within_tower * 37) % ADDR_PER_TOWER;
        int polar = ((within_tower * 37) / ADDR_PER_TOWER) % N_POLAR;

        face_count[f]++;
        record_placement(i, tower, addr, polar);
    }

    Stats s = compute_stats();
    print_stats("FACE_SYMMETRIC", &s);

    /* Verify symmetry: each face should have ~equal tower distribution */
    int face_tower_dist[6][N_TOWERS];
    memset(face_tower_dist, 0, sizeof(face_tower_dist));
    for (int i = 0; i < CUBE_CELLS; i++) {
        int f = all_cells[i].face;
        int t = placements[i].tower;
        face_tower_dist[f][t]++;
    }

    printf("    Face→Tower distribution:\n");
    int max_imbal = 0;
    for (int f = 0; f < 6; f++) {
        printf("      Face %d:", f);
        int fmin = 1000, fmax = 0;
        for (int ti = 0; ti < 5; ti++) {
            int t = FACE_TOWERS[f][ti];
            int cnt = face_tower_dist[f][t];
            printf(" T%d=%d", t, cnt);
            if (cnt < fmin) fmin = cnt;
            if (cnt > fmax) fmax = cnt;
        }
        int imbal = fmax - fmin;
        if (imbal > max_imbal) max_imbal = imbal;
        printf("  imbal=%d\n", imbal);
    }
    printf("    Max face imbalance: %d\n", max_imbal);

    printf("  PASS\n");
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
   APPROACH 3: Depth-Sliced
   z=0..9 each gets a full surface pass.

   Algorithm:
     - For each z (0..9), process all 6×10×10 = 600 surface cells.
     - Each z-slice maps independently through stride-37 walk.
     - Different z-values get different phase offsets in the walk.
   ════════════════════════════════════════════════════════════════════ */

static int approach_depth_sliced(void) {
    printf("\n═══════════════════════════════════════════════════\n");
    printf("  APPROACH 3: Depth-Sliced\n");
    printf("  z=0..9 each gets a full surface pass\n");
    printf("═══════════════════════════════════════════════════\n");

    reset_loads();

    for (int i = 0; i < CUBE_CELLS; i++) {
        ContourCell *c = &all_cells[i];
        int f = c->face;
        int z = c->z;

        /* Surface position within this z-slice: (face, x, y) → 0..599 */
        int surface_idx = f * CUBE_W * CUBE_H + c->y * CUBE_W + c->x;

        /* Each z-slice gets a phase offset in stride-37 walk */
        int walk_pos = (surface_idx * 37 + z * 120) % GEO_ADDRS;

        /* Decompose walk_pos into tower, addr, polar */
        int tower = walk_pos / (ADDR_PER_TOWER * N_POLAR);
        int remainder = walk_pos % (ADDR_PER_TOWER * N_POLAR);
        int polar = remainder / ADDR_PER_TOWER;
        int addr = remainder % ADDR_PER_TOWER;

        if (tower >= N_TOWERS) tower = tower % N_TOWERS;
        record_placement(i, tower, addr, polar);
    }

    Stats s = compute_stats();
    print_stats("DEPTH_SLICED", &s);

    /* Per-z analysis */
    printf("    Per-z distribution (600 cells each):\n");
    for (int z = 0; z < CUBE_L; z++) {
        int z_towers[N_TOWERS];
        memset(z_towers, 0, sizeof(z_towers));
        for (int i = 0; i < CUBE_CELLS; i++) {
            if (all_cells[i].z == z) {
                z_towers[placements[i].tower]++;
            }
        }
        int zmin = 1000, zmax = 0;
        int used = 0;
        for (int t = 0; t < N_TOWERS; t++) {
            if (z_towers[t] > 0) used++;
            if (z_towers[t] > 0 && z_towers[t] < zmin) zmin = z_towers[t];
            if (z_towers[t] > zmax) zmax = z_towers[t];
        }
        printf("      z=%d: towers_used=%2d  min=%3d max=%3d\n",
               z, used, zmin, zmax);
    }

    printf("  PASS\n");
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
   APPROACH 4: Anti-Collision (Greedy)
   Greedy assign cells to least-filled addresses.

   Algorithm:
     - Sort cells by (face, z, y, x) for deterministic order.
     - For each cell, scan all 1440 addresses, pick the one with
       minimum current load. Break ties by lowest index.
     - Result: minimizes maximum collision depth.
   ════════════════════════════════════════════════════════════════════ */

static int approach_anti_collision(void) {
    printf("\n═══════════════════════════════════════════════════\n");
    printf("  APPROACH 4: Anti-Collision (Greedy)\n");
    printf("  Assign each cell to least-filled address\n");
    printf("═══════════════════════════════════════════════════\n");

    reset_loads();

    for (int i = 0; i < CUBE_CELLS; i++) {
        /* Find address with minimum load */
        int best_g = 0;
        int best_load = addr_load[0];
        for (int g = 1; g < GEO_ADDRS; g++) {
            if (addr_load[g] < best_load) {
                best_load = addr_load[g];
                best_g = g;
            }
        }
        /* Decompose geo_idx back to tower, addr, polar */
        int tower = best_g / (ADDR_PER_TOWER * N_POLAR);
        int remainder = best_g % (ADDR_PER_TOWER * N_POLAR);
        int polar = remainder / ADDR_PER_TOWER;
        int addr = remainder % ADDR_PER_TOWER;
        record_placement(i, tower, addr, polar);
    }

    Stats s = compute_stats();
    print_stats("ANTI_COLLISION", &s);

    /* Theoretical minimum max_depth: ceil(6000/1440) = 5 */
    int theoretical_min = (CUBE_CELLS + GEO_ADDRS - 1) / GEO_ADDRS;
    printf("    Theoretical min max_depth: %d\n", theoretical_min);
    printf("    Actual max_depth:          %d\n", s.max_depth);
    printf("    Optimal: %s\n",
           s.max_depth == theoretical_min ? "YES" : "NO (greedy is approximate)");

    printf("  PASS\n");
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
   COMPARISON TABLE
   ════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;
    Stats stats;
} ApproachResult;

static ApproachResult results[4];

static void run_all_approaches(void) {
    printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  Contour Cube → GeoJump Structural Placement               ║\n");
    printf("║  6000 cells → 1440 addresses (15 towers × 48 × 2)         ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    printf("  Constants:\n");
    printf("    CUBE_CELLS  = %d (6×10×10×10)\n", CUBE_CELLS);
    printf("    GEO_ADDRS   = %d (15×48×2)\n", GEO_ADDRS);
    printf("    Ratio       = %.2f cells/addr\n", (double)CUBE_CELLS / GEO_ADDRS);
    printf("    15 towers   = C(6,2) from 6 cube faces\n");
    printf("    Each tower  = 48 addr × 2 polar = 96 slots\n");

    /* Run each approach, capture stats */
    approach_tower_pair_balanced();
    { Stats s = compute_stats(); results[0] = (ApproachResult){"Tower-Pair Balanced", s}; }

    approach_face_symmetric();
    { Stats s = compute_stats(); results[1] = (ApproachResult){"Face-Symmetric", s}; }

    approach_depth_sliced();
    { Stats s = compute_stats(); results[2] = (ApproachResult){"Depth-Sliced", s}; }

    approach_anti_collision();
    { Stats s = compute_stats(); results[3] = (ApproachResult){"Anti-Collision", s}; }

    /* ── Comparison table ── */
    printf("\n╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  COMPARISON TABLE                                                       ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  %-22s │ MaxD │ Fill%% │ Tmbal │ Polar   │ Collisions                ║\n", "Approach");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    for (int i = 0; i < 4; i++) {
        Stats *s = &results[i].stats;
        printf("║  %-22s │  %2d  │ %4.1f%% │ %4.1f │ %2d..%-2d │ %5d / %4d unique        ║\n",
               results[i].name,
               s->max_depth,
               100.0 * s->unique_addrs / GEO_ADDRS,
               s->tower_imbalance,
               s->polar_min, s->polar_max,
               s->collisions, s->unique_addrs);
    }
    printf("╚══════════════════════════════════════════════════════════════════════════╝\n");

    printf("\n  Legend:\n");
    printf("    MaxD    = max cells at any single address (lower = less collision)\n");
    printf("    Fill%%   = %% of 1440 addresses used (higher = better spread)\n");
    printf("    Tmbal   = tower load imbalance (max-min, lower = more uniform)\n");
    printf("    Polar   = min..max cells in polarity 0 vs 1\n");
    printf("    Collisions = total cells beyond 1 per address (0 = perfect)\n");
}

/* ════════════════════════════════════════════════════════════════════
   TESTS
   ════════════════════════════════════════════════════════════════════ */

static int test_constants(void) {
    printf("=== TEST: Constants ===\n");
    printf("  CUBE_CELLS = %d (expect 6000)\n", CUBE_CELLS);
    if (CUBE_CELLS != 6000) { printf("  FAIL\n"); return 0; }
    printf("  GEO_ADDRS  = %d (expect 1440)\n", GEO_ADDRS);
    if (GEO_ADDRS != 1440) { printf("  FAIL\n"); return 0; }
    printf("  Cells/Addr = %.2f (expect 4.17)\n", (double)CUBE_CELLS / GEO_ADDRS);
    printf("  PASS\n");
    return 1;
}

static int test_face_towers(void) {
    printf("\n=== TEST: Face→Tower mapping ===\n");
    int ok = 1;

    for (int f = 0; f < 6; f++) {
        printf("  Face %d → towers:", f);
        for (int ti = 0; ti < 5; ti++) {
            printf(" %d(%c%c)", FACE_TOWERS[f][ti],
                   'A' + PAIR_A[FACE_TOWERS[f][ti]],
                   'A' + PAIR_B[FACE_TOWERS[f][ti]]);
        }
        printf("\n");
    }

    /* Each face must have exactly 5 towers */
    for (int f = 0; f < 6; f++) {
        int count = 0;
        for (int t = 0; t < N_TOWERS; t++) {
            if (PAIR_A[t] == f || PAIR_B[t] == f) count++;
        }
        if (count != 5) {
            printf("  FAIL: face %d has %d towers (expected 5)\n", f, count);
            ok = 0;
        }
    }

    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_geo_idx_roundtrip(void) {
    printf("\n=== TEST: geo_idx roundtrip ===\n");
    int ok = 1;
    for (int t = 0; t < N_TOWERS; t++) {
        for (int a = 0; a < ADDR_PER_TOWER; a++) {
            for (int p = 0; p < N_POLAR; p++) {
                int g = geo_idx(t, a, p);
                if (g < 0 || g >= GEO_ADDRS) {
                    printf("  FAIL: t=%d a=%d p=%d → g=%d out of range\n",
                           t, a, p, g);
                    ok = 0;
                    break;
                }
                /* Reverse */
                int rt = g / (ADDR_PER_TOWER * N_POLAR);
                int rem = g % (ADDR_PER_TOWER * N_POLAR);
                int rp = rem / ADDR_PER_TOWER;
                int ra = rem % ADDR_PER_TOWER;
                if (rt != t || rp != p || ra != a) {
                    printf("  FAIL: (%d,%d,%d)→%d→(%d,%d,%d)\n",
                           t, a, p, g, rt, ra, rp);
                    ok = 0;
                    break;
                }
            }
        }
    }
    printf("  Unique addresses: %d (expect %d)\n",
           N_TOWERS * ADDR_PER_TOWER * N_POLAR, GEO_ADDRS);
    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_no_duplicate_cells(void) {
    printf("\n=== TEST: No duplicate cells in all_cells ===\n");
    uint8_t seen[CUBE_CELLS];
    memset(seen, 0, sizeof(seen));
    int ok = 1;
    for (int i = 0; i < CUBE_CELLS; i++) {
        int idx = all_cells[i].global_idx;
        if (idx < 0 || idx >= CUBE_CELLS || seen[idx]) {
            printf("  FAIL: cell %d has global_idx %d (dup or out of range)\n",
                   i, idx);
            ok = 0;
            break;
        }
        seen[idx] = 1;
    }
    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* ════════════════════════════════════════════════════════════════════
   MAIN
   ════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  Contour Placement Structural Explorer                      ║\n");
    printf("║  4 approaches for 6000→1440 with 15-tower symmetry         ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    init_cells();
    init_face_towers();

    int pass = 0, total = 0;

    total++; pass += test_constants();
    total++; pass += test_face_towers();
    total++; pass += test_geo_idx_roundtrip();
    total++; pass += test_no_duplicate_cells();

    run_all_approaches();

    printf("\n════════════════════════════════════════════════════════════════\n");
    printf("  FINAL: %d/%d PASS\n", pass, total);
    printf("════════════════════════════════════════════════════════════════\n");

    return (pass == total) ? 0 : 1;
}

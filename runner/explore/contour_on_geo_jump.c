// contour_on_geo_jump.c
// ================================================================
// EXPERIMENT: Place contour cube 6000 cells onto geo_jump 1440 addresses
//
// Contour cube: 6 faces × 10 × 10 × 10 = 6000 cells
//   1 cube = 1 unit container (10×10 surface, 10 deep, 6 directions)
//   repeat as container unit in all directions
//
// Geo_jump: 15 towers × 48 addresses × 2 polar = 1440
//   1 island = 720 (on shore), 2 islands = 1440 (+ underwater)
//   C(6,2) = 15 pairs = silk screen pairs
//
// Question: How do 6000 cells distribute onto 1440 addresses?
//   - How many cells per address?
//   - How many cycles to fill all cells?
//   - Is distribution uniform?
// ================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CUBE_FACES   6
#define CUBE_W      10
#define CUBE_H      10
#define CUBE_L      10
#define CUBE_CELLS  (CUBE_FACES * CUBE_W * CUBE_H * CUBE_L)  // 6000

#define N_TOWERS    15
#define TOWER_ADDR  48
#define N_POLAR     2
#define GEO_ADDRS   (N_TOWERS * TOWER_ADDR * N_POLAR)  // 1440

#define GEO_FIBO_CLOCK 1440

// ── Contour cube: face × (x, y, z) ──
// face 0..5 (A-F), x,y,z 0..9
typedef struct {
    int face;
    int x, y, z;
    int global_idx;  // 0..5999
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

// ── Geo_jump: map cell → tower + addr ──
// 6 faces → C(6,2)=15 pairs → 15 towers
// Each cell (face, x, y, z) needs to land on one of 1440 addresses
//
// Strategy 1: face pair determines tower, (x,y,z) determines addr
//   pair = face_pair[face] (which pair this face belongs to)
//   addr = x*10*10 + y*10 + z (linear index within cube face)
//   But 1000 addr/tower > 48 → collision!
//
// Strategy 2: full 3D → 1D mapping through tower
//   Each tower has 48 slots, 2 poles = 96 per tower pair
//   15 towers × 96 = 1440
//   Need: 6000 cells → 1440 addresses (many-to-one expected)
//
// Strategy 3: timeline cycling
//   t = 0..1439 → geo_jump address
//   cell[t % 6000] placed at address t
//   After 1440 steps: 1440 cells placed (24% of 6000)
//   After 6000 steps: all placed, but some addresses hit multiple times

// ── Pair assignment: 6 faces → 15 pairs ──
// Each face appears in 5 pairs (C(5,1) = 5 partners)
// pair_idx = face * 5 + partner_offset
// But only 15 towers, not 30. Each tower handles a pair of faces.

// Tower assignment: face → which towers it participates in
// face 0 participates in pairs: (0,1),(0,2),(0,3),(0,4),(0,5) = towers 0,1,2,3,4
// face 1 participates in pairs: (0,1),(1,2),(1,3),(1,4),(1,5) = towers 0,5,6,7,8
// etc.

// Full pair list:
// pair 0: (0,1)  pair 1: (0,2)  pair 2: (0,3)  pair 3: (0,4)  pair 4: (0,5)
// pair 5: (1,2)  pair 6: (1,3)  pair 7: (1,4)  pair 8: (1,5)
// pair 9: (2,3)  pair 10: (2,4) pair 11: (2,5)
// pair 12: (3,4) pair 13: (3,5)
// pair 14: (4,5)

static int PAIR_A[15] = {0,0,0,0,0, 1,1,1,1, 2,2,2, 3,3, 4};
static int PAIR_B[15] = {1,2,3,4,5, 2,3,4,5, 3,4,5, 4,5, 5};

// Which towers does a face belong to?
// face f → towers where PAIR_A[t]==f or PAIR_B[t]==f
// Each face is in 5 towers

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

// ── Approach A: Direct mapping (face,x,y,z) → geo address ──
// face → 1 of 5 towers it belongs to (select by (x+y+z) % 5)
// (x,y,z) → addr within tower (0..47)
// z → polarity (0=shore, 1=underwater)
typedef struct {
    int tower;    // 0..14
    int addr;     // 0..47
    int polar;    // 0..1
    int geo_idx;  // 0..1439 (tower*48*2 + polar*48 + addr, NOT this)
} GeoAddr;

static GeoAddr map_A(int face, int x, int y, int z) {
    GeoAddr a;
    int tower_local = (x + y + z) % 5;
    a.tower = FACE_TOWERS[face][tower_local];
    a.addr = (x * 10 + y) % TOWER_ADDR;  // 0..99 → mod 48
    a.polar = z % N_POLAR;
    // compute global geo index
    // find pair index for this tower
    a.geo_idx = a.tower * TOWER_ADDR * N_POLAR
              + a.polar * TOWER_ADDR
              + a.addr;
    return a;
}

// ── Approach B: Timeline cycling ──
// t = 0..1439: walk through geo_jump addresses
// cell_index = t % 6000 (which cell to place)
// After N cycles, all cells placed
typedef struct {
    int cell_idx;
    int tower;
    int addr;
    int polar;
    int cycle;
} TimelineMap;

static TimelineMap map_B(int t) {
    TimelineMap m;
    m.cell_idx = t % CUBE_CELLS;
    m.tower = (t / (TOWER_ADDR * N_POLAR)) % N_TOWERS;
    m.polar = (t / TOWER_ADDR) % N_POLAR;
    m.addr = t % TOWER_ADDR;
    m.cycle = t / GEO_FIBO_CLOCK;
    return m;
}

// compute geo_idx from tower+polar+addr
static int tl_geo_idx(TimelineMap *m) {
    return m->tower * TOWER_ADDR * N_POLAR
         + m->polar * TOWER_ADDR
         + m->addr;
}

// ═══════════════════════════════════════════
// TEST A: Direct mapping — distribution
// ═══════════════════════════════════════════

static int test_A_distribution(void) {
    printf("═══ TEST A: Direct Mapping Distribution ═══\n");
    printf("  %d cells → %d geo addresses\n\n", CUBE_CELLS, GEO_ADDRS);

    int addr_count[GEO_ADDRS];
    memset(addr_count, 0, sizeof(addr_count));

    int tower_count[N_TOWERS];
    memset(tower_count, 0, sizeof(tower_count));

    int polar_count[N_POLAR];
    memset(polar_count, 0, sizeof(polar_count));

    int face_tower_hit[6][N_TOWERS];
    memset(face_tower_hit, 0, sizeof(face_tower_hit));

    for (int i = 0; i < CUBE_CELLS; i++) {
        ContourCell *c = &all_cells[i];
        GeoAddr a = map_A(c->face, c->x, c->y, c->z);
        addr_count[a.geo_idx]++;
        tower_count[a.tower]++;
        polar_count[a.polar]++;
        face_tower_hit[c->face][a.tower]++;
    }

    // Address distribution
    int min_hit = CUBE_CELLS, max_hit = 0, zero_addrs = 0;
    int hit_histogram[20] = {0};  // index = hit count
    for (int i = 0; i < GEO_ADDRS; i++) {
        if (addr_count[i] < min_hit) min_hit = addr_count[i];
        if (addr_count[i] > max_hit) max_hit = addr_count[i];
        if (addr_count[i] == 0) zero_addrs++;
        int h = addr_count[i];
        if (h >= 20) h = 19;
        hit_histogram[h]++;
    }

    printf("  Address distribution:\n");
    printf("    Min cells/address:  %d\n", min_hit);
    printf("    Max cells/address:  %d\n", max_hit);
    printf("    Zero-hit addresses: %d / %d\n", zero_addrs, GEO_ADDRS);
    printf("    Avg: %.2f cells/address\n", (double)CUBE_CELLS / GEO_ADDRS);

    printf("\n  Hit histogram (count of addresses with N hits):\n");
    for (int h = 0; h < 20; h++) {
        if (hit_histogram[h] > 0)
            printf("    %2d hits: %4d addresses\n", h, hit_histogram[h]);
    }

    // Tower distribution
    printf("\n  Tower distribution (%d towers):\n", N_TOWERS);
    for (int t = 0; t < N_TOWERS; t++) {
        printf("    Tower %2d: %4d cells  (pair %d-%d)\n",
               t, tower_count[t], PAIR_A[t], PAIR_B[t]);
    }

    // Polar distribution
    printf("\n  Polar distribution:\n");
    printf("    Shore (0): %d cells\n", polar_count[0]);
    printf("    Water (1): %d cells\n", polar_count[1]);

    // Face → tower coverage
    printf("\n  Face → Tower coverage:\n");
    for (int f = 0; f < 6; f++) {
        printf("    Face %d: ", f);
        for (int t = 0; t < N_TOWERS; t++) {
            if (face_tower_hit[f][t] > 0)
                printf("T%d(%d) ", t, face_tower_hit[f][t]);
        }
        printf("\n");
    }

    // Uniqueness check: do any cells collide?
    int collisions = 0;
    memset(addr_count, 0, sizeof(addr_count));
    for (int i = 0; i < CUBE_CELLS; i++) {
        ContourCell *c = &all_cells[i];
        GeoAddr a = map_A(c->face, c->x, c->y, c->z);
        addr_count[a.geo_idx]++;
    }
    for (int i = 0; i < GEO_ADDRS; i++) {
        if (addr_count[i] > 1) collisions += addr_count[i] - 1;
    }
    printf("\n  Total collisions: %d (cells sharing an address)\n", collisions);

    printf("  PASS\n");
    return 1;
}

// ═══════════════════════════════════════════
// TEST B: Timeline cycling — coverage over time
// ═══════════════════════════════════════════

static int test_B_timeline(void) {
    printf("\n═══ TEST B: Timeline Cycling ═══\n");
    printf("  Walk t=0..∞, place cell[t%%6000] at geo address t%%1440\n\n");

    int geo_hit[GEO_ADDRS];
    memset(geo_hit, 0, sizeof(geo_hit));

    int cell_placed[CUBE_CELLS];
    memset(cell_placed, 0, sizeof(cell_placed));

    int max_t = GEO_FIBO_CLOCK * 5;  // 5 cycles = 7200 steps
    int full_cycle = -1;

    for (int t = 0; t < max_t; t++) {
        TimelineMap m = map_B(t);
        geo_hit[tl_geo_idx(&m)]++;
        cell_placed[m.cell_idx]++;

        // Check if all cells placed at least once
        if (full_cycle < 0) {
            int all_placed = 1;
            for (int i = 0; i < CUBE_CELLS; i++) {
                if (!cell_placed[i]) { all_placed = 0; break; }
            }
            if (all_placed) full_cycle = t;
        }
    }

    printf("  Steps to place all %d cells: %d\n", CUBE_CELLS, full_cycle);
    printf("  Cycles needed: %.2f\n", (double)full_cycle / GEO_FIBO_CLOCK);

    // Address hit distribution after full coverage
    int min_hit = max_t, max_hit = 0, zero_addrs = 0;
    for (int i = 0; i < GEO_ADDRS; i++) {
        if (geo_hit[i] < min_hit) min_hit = geo_hit[i];
        if (geo_hit[i] > max_hit) max_hit = geo_hit[i];
        if (geo_hit[i] == 0) zero_addrs++;
    }
    printf("  Address hits after %d steps:\n", max_t);
    printf("    Min: %d, Max: %d, Zero: %d\n", min_hit, max_hit, zero_addrs);

    // Cell placement frequency
    int min_placed = max_t, max_placed = 0;
    for (int i = 0; i < CUBE_CELLS; i++) {
        if (cell_placed[i] < min_placed) min_placed = cell_placed[i];
        if (cell_placed[i] > max_placed) max_placed = cell_placed[i];
    }
    printf("  Cell placement frequency:\n");
    printf("    Min: %d, Max: %d\n", min_placed, max_placed);

    printf("  PASS\n");
    return 1;
}

// ═══════════════════════════════════════════
// TEST C: Reverse lookup — geo address → contour cell
// ═══════════════════════════════════════════

static int test_C_reverse(void) {
    printf("\n═══ TEST C: Reverse Lookup ═══\n");
    printf("  Given geo address, find which contour cell(s) are there\n\n");

    // Build reverse map: geo_idx → list of cell indices
    int *rev_count = (int *)calloc(GEO_ADDRS, sizeof(int));
    if (!rev_count) {
        printf("  FAIL: alloc\n");
        return 0;
    }

    // First pass: count
    for (int i = 0; i < CUBE_CELLS; i++) {
        ContourCell *c = &all_cells[i];
        GeoAddr a = map_A(c->face, c->x, c->y, c->z);
        rev_count[a.geo_idx]++;
    }

    // Show some examples
    printf("  Sample reverse lookups:\n");
    int shown = 0;
    for (int g = 0; g < GEO_ADDRS && shown < 10; g++) {
        if (rev_count[g] > 0) {
            printf("    geo[%3d] = %d cell(s): ", g, rev_count[g]);
            for (int i = 0; i < CUBE_CELLS && shown < 10; i++) {
                ContourCell *c = &all_cells[i];
                GeoAddr a = map_A(c->face, c->x, c->y, c->z);
                if (a.geo_idx == g) {
                    printf("face%d(%d,%d,%d) ", c->face, c->x, c->y, c->z);
                    shown++;
                }
            }
            printf("\n");
        }
    }

    // Coverage: how many geo addresses have at least 1 cell?
    int covered = 0;
    for (int g = 0; g < GEO_ADDRS; g++) {
        if (rev_count[g] > 0) covered++;
    }
    printf("\n  Geo addresses with ≥1 cell: %d / %d (%.1f%%)\n",
           covered, GEO_ADDRS, 100.0 * covered / GEO_ADDRS);
    printf("  Empty addresses: %d (%.1f%%)\n",
           GEO_ADDRS - covered, 100.0 * (GEO_ADDRS - covered) / GEO_ADDRS);

    free(rev_count);
    printf("  PASS\n");
    return 1;
}

// ═══════════════════════════════════════════
// TEST D: Container repeat — 6000 as repeating unit
// ═══════════════════════════════════════════

static int test_D_container(void) {
    printf("\n═══ TEST D: Container Repeat ═══\n");
    printf("  1 cube = 1 unit container = 6000 cells\n");
    printf("  Repeat as container unit in all directions\n\n");

    // How many cubes to fill N timelines?
    printf("  Cycles to exhaust 1 cube:\n");
    printf("    1 cycle  = %d addresses\n", GEO_FIBO_CLOCK);
    printf("    1 cube   = %d cells\n", CUBE_CELLS);
    printf("    Ratio: %.2f cycles per cube\n",
           (double)CUBE_CELLS / GEO_FIBO_CLOCK);
    printf("    Remaining after 1 cube: %d cells unwritten\n",
           CUBE_CELLS - GEO_FIBO_CLOCK);

    printf("\n  Multi-cube scenario:\n");
    for (int ncubes = 1; ncubes <= 5; ncubes++) {
        int total_cells = ncubes * CUBE_CELLS;
        int cycles = (total_cells + GEO_FIBO_CLOCK - 1) / GEO_FIBO_CLOCK;
        printf("    %d cube(s): %5d cells, %d cycles (%d steps)\n",
               ncubes, total_cells, cycles, cycles * GEO_FIBO_CLOCK);
    }

    printf("\n  Address utilization per cube:\n");
    printf("    1 cube fills %.1f%% of one timeline cycle\n",
           100.0 * CUBE_CELLS / GEO_FIBO_CLOCK);
    printf("    Need %.2f cubes to fully utilize one cycle\n",
           (double)GEO_FIBO_CLOCK / CUBE_CELLS);

    printf("  PASS\n");
    return 1;
}

// ═══════════════════════════════════════════

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Contour Cube on GeoJump — Placement Experiment        ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    printf("  Contour: %d cells (6×10×10×10)\n", CUBE_CELLS);
    printf("  GeoJump: %d addresses (15 towers × 48 × 2)\n", GEO_ADDRS);
    printf("  Ratio: %.2f cells per address\n\n",
           (double)CUBE_CELLS / GEO_ADDRS);

    init_cells();
    init_face_towers();

    int pass = 0, total = 0;
    total++; pass += test_A_distribution();
    total++; pass += test_B_timeline();
    total++; pass += test_C_reverse();
    total++; pass += test_D_container();

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("  FINAL: %d/%d PASS\n", pass, total);
    printf("══════════════════════════════════════════════════════════\n");
    return (pass == total) ? 0 : 1;
}

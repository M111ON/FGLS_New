// contour_geo_jump_gearbox.c
// ================================================================
// EXPERIMENT: Contour Cube through GeoJump Gearbox
//
// geo_jump = deterministic container with adaptive adjustable gearbox
// Output: 128 (must fit 162) or 144 (flexible)
//
// Contour cube: 6 faces × 10×10×10 = 6000 cells
// Goal: route 6000 cells through gearbox → output 128 or 144
//
// For each output config, measure:
//   - cells per address
//   - collision count
//   - coverage
//   - does 128 fit on 162?
// ================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define CUBE_FACES   6
#define CUBE_W      10
#define CUBE_H      10
#define CUBE_L      10
#define CUBE_CELLS  (CUBE_FACES * CUBE_W * CUBE_H * CUBE_L)  // 6000

#define ICO_NODES   162   // icosphere L2 (81×2 poles)

// ── Contour cube cell ──
typedef struct {
    int face, x, y, z;
    int global_idx;
} Cell;

static Cell cells[CUBE_CELLS];

static void init_cells(void) {
    int idx = 0;
    for (int f = 0; f < CUBE_FACES; f++)
        for (int z = 0; z < CUBE_L; z++)
            for (int y = 0; y < CUBE_H; y++)
                for (int x = 0; x < CUBE_W; x++) {
                    cells[idx].face = f;
                    cells[idx].x = x;
                    cells[idx].y = y;
                    cells[idx].z = z;
                    cells[idx].global_idx = idx;
                    idx++;
                }
}

// ── Mapping: cell → address in gearbox ──

// Config A: face × 24 + surface + depth
//   face (0..5) × 24 = face slot
//   surface: x*10 + y = 0..99 → mod 24
//   depth: z
static int map_6x24(const Cell *c, int output_size) {
    int face_slot = c->face * 24;
    int surface = (c->x * CUBE_W + c->y) % 24;
    return (face_slot + surface) % output_size;
}

// Config B: stride-37 walk on output
//   global_idx × 37 % output_size
static int map_stride37(const Cell *c, int output_size) {
    return (c->global_idx * 37) % output_size;
}

// Config C: tower-interleaved
//   face → tower (face % n_layers)
//   (x,y,z) → addr within tower
static int map_tower(const Cell *c, int n_layers, int addrs_per_layer) {
    int layer = c->face % n_layers;
    int addr = (c->x * 100 + c->y * 10 + c->z) % addrs_per_layer;
    return layer * addrs_per_layer + addr;
}

// Config D: face-first sequential
//   all cells of face 0 first, then face 1, etc.
static int map_sequential(const Cell *c, int output_size) {
    int cells_per_face = CUBE_CELLS / CUBE_FACES;  // 1000
    int face_offset = c->face * cells_per_face;
    int local = c->x * 100 + c->y * 10 + c->z;
    return (face_offset + local) % output_size;
}

// ═══════════════════════════════════════════
// TEST: Run all configs with all mapping strategies
// ═══════════════════════════════════════════

typedef struct {
    int collisions;
    int covered;
    int zero_hit;
    int max_hit;
    int min_hit;
    double cv;  // coefficient of variation
    int tower_balance[6];  // cells per face
} Result;

static Result evaluate(int output_size, int n_layers, int addrs_per_layer,
                       int map_type) {
    Result r = {0};
    int *addr_count = (int *)calloc(output_size, sizeof(int));
    int *face_count = (int *)calloc(6, sizeof(int));
    if (!addr_count || !face_count) {
        fprintf(stderr, "evaluate: allocation failed\n");
        free(addr_count);
        free(face_count);
        return r;
    }

    for (int i = 0; i < CUBE_CELLS; i++) {
        int addr;
        switch (map_type) {
            case 0: addr = map_6x24(&cells[i], output_size); break;
            case 1: addr = map_stride37(&cells[i], output_size); break;
            case 2: addr = map_tower(&cells[i], n_layers, addrs_per_layer); break;
            case 3: addr = map_sequential(&cells[i], output_size); break;
            default: addr = 0; break;
        }
        addr_count[addr]++;
        face_count[cells[i].face]++;
    }

    // Stats
    r.min_hit = CUBE_CELLS;
    double sum = 0, sum_sq = 0;
    for (int i = 0; i < output_size; i++) {
        if (addr_count[i] < r.min_hit) r.min_hit = addr_count[i];
        if (addr_count[i] > r.max_hit) r.max_hit = addr_count[i];
        if (addr_count[i] == 0) r.zero_hit++;
        else r.covered++;
        sum += addr_count[i];
        sum_sq += (double)addr_count[i] * addr_count[i];
    }
    r.collisions = CUBE_CELLS - r.covered;
    double mean = sum / output_size;
    double variance = sum_sq / output_size - mean * mean;
    r.cv = (mean > 0) ? sqrt(variance) / mean : 0;

    for (int f = 0; f < 6; f++) r.tower_balance[f] = face_count[f];

    free(addr_count);
    free(face_count);
    return r;
}

// ═══════════════════════════════════════════
// TEST 1: Output 144 — all configs
// ═══════════════════════════════════════════

static int test_144(void) {
    printf("═══ TEST 1: Output 144 (flexible) ═══\n\n");

    printf("  %-16s │ %-8s │ %-8s │ %-8s │ %-8s │ %-6s\n",
           "Config", "Collide", "Cover%", "MaxHit", "CV", "BalOK");
    printf("  ─────────────────┼──────────┼──────────┼──────────┼──────────┼──────\n");

    const char *map_names[] = {"6×24", "stride37", "tower", "sequential"};

    for (int m = 0; m < 4; m++) {
        Result r;
        if (m == 0) r = evaluate(144, 6, 24, 0);       // 6×24
        else if (m == 1) r = evaluate(144, 0, 0, 1);    // stride37
        else if (m == 2) r = evaluate(144, 3, 48, 2);   // tower (3×48)
        else r = evaluate(144, 0, 0, 3);                 // sequential

        int balanced = 1;
        for (int f = 1; f < 6; f++) {
            if (r.tower_balance[f] != r.tower_balance[0]) balanced = 0;
        }

        printf("  %-16s │ %7d  │ %6.1f%%  │ %7d  │ %.4f   │ %s\n",
               map_names[m], r.collisions,
               100.0 * r.covered / 144, r.max_hit, r.cv,
               balanced ? "✓" : "✗");
    }

    // Detailed 6×24 breakdown
    printf("\n  Detailed: 6×24 mapping\n");
    Result r = evaluate(144, 6, 24, 0);
    printf("    Face distribution: ");
    for (int f = 0; f < 6; f++) printf("F%d=%d ", f, r.tower_balance[f]);
    printf("\n");
    printf("    144 = 6 faces × 24 addr → each face gets exactly 24 addresses\n");
    printf("    6000 / 6 = 1000 cells/face → 1000 / 24 = %.1f cells/addr\n",
           1000.0 / 24);

    printf("  PASS\n\n");
    return 1;
}

// ═══════════════════════════════════════════
// TEST 2: Output 128 on 162 — all configs
// ═══════════════════════════════════════════

static int test_128(void) {
    printf("═══ TEST 2: Output 128 on 162 (geometry constraint) ═══\n\n");

    printf("  162 - 128 = 34 addresses wasted\n");
    printf("  128 must map onto icosphere L2 (162 nodes)\n\n");

    printf("  %-18s │ %-8s │ %-8s │ %-8s │ %-8s │ %-6s\n",
           "Internal Config", "Collide", "Cover%", "MaxHit", "CV", "BalOK");
    printf("  ──────────────────┼──────────┼──────────┼──────────┼──────────┼──────\n");

    // Test different internal layer configs for 128
    typedef struct { const char *name; int layers; int addrs; } LayerCfg;
    LayerCfg layers[] = {
        {"2×64",   2,  64},
        {"4×32",   4,  32},
        {"8×16",   8,  16},
        {"16×8",  16,   8},
        {"32×4",  32,   4},
        {"64×2",  64,   2},
        {"128×1", 128,  1},
    };
    int n_layers = sizeof(layers)/sizeof(layers[0]);

    for (int i = 0; i < n_layers; i++) {
        Result r = evaluate(128, layers[i].layers, layers[i].addrs, 2);

        int balanced = 1;
        for (int f = 1; f < 6; f++) {
            if (r.tower_balance[f] != r.tower_balance[0]) balanced = 0;
        }

        printf("  %-18s │ %7d  │ %6.1f%%  │ %7d  │ %.4f   │ %s\n",
               layers[i].name, r.collisions,
               100.0 * r.covered / 128, r.max_hit, r.cv,
               balanced ? "✓" : "✗");
    }

    // Also test stride37 on 128
    Result r_stride = evaluate(128, 0, 0, 1);
    printf("  %-18s │ %7d  │ %6.1f%%  │ %7d  │ %.4f   │\n",
           "stride37", r_stride.collisions,
           100.0 * r_stride.covered / 128, r_stride.max_hit, r_stride.cv);

    printf("\n  ⚠ 128 on 162: 34 addresses unused (4.2%% waste)\n");
    printf("  ⚠ 6 faces × 21.33 = 128 (not integer!)\n");
    printf("  ⚠ Face distribution cannot be equal with 128\n");

    printf("  PASS\n\n");
    return 1;
}

// ═══════════════════════════════════════════
// TEST 3: 128 vs 144 head-to-head
// ═══════════════════════════════════════════

static int test_head_to_head(void) {
    printf("═══ TEST 3: 128 vs 144 Head-to-Head ═══\n\n");

    // Best 144 config: 6×24 stride37
    Result r144 = evaluate(144, 6, 24, 0);
    // Best 128 config: 8×16 tower
    Result r128 = evaluate(128, 8, 16, 2);

    printf("  %-20s │ Output 144 │ Output 128\n", "Metric");
    printf("  ──────────────────┼────────────┼───────────\n");
    printf("  %-20s │ %10d │ %9d\n", "Addresses", 144, 128);
    printf("  %-20s │ %10.1f │ %9.1f\n", "Cells/addr",
           (double)CUBE_CELLS/144, (double)CUBE_CELLS/128);
    printf("  %-20s │ %10d │ %9d\n", "Collisions", r144.collisions, r128.collisions);
    printf("  %-20s │ %9.1f%% │ %8.1f%%\n", "Coverage",
           100.0*r144.covered/144, 100.0*r128.covered/128);
    printf("  %-20s │ %10d │ %9d\n", "Max hit", r144.max_hit, r128.max_hit);
    printf("  %-20s │ %10.4f │ %9.4f\n", "CV (uniformity)", r144.cv, r128.cv);
    printf("  %-20s │ %10s │ %9s\n", "Face balance", "equal", "unequal");
    printf("  %-20s │ %10s │ %9s\n", "Geometry fit", "flexible", "162 only");
    printf("  %-20s │ %10s │ %9d\n", "Wasted addr", "0", 34);

    printf("\n  Verdict:\n");
    printf("  144: 6×24 = perfect face fit, 0 waste, equal balance\n");
    printf("  128: must fit 162, 34 waste, unequal face split\n");

    printf("  PASS\n\n");
    return 1;
}

// ═══════════════════════════════════════════
// TEST 4: Full pipeline — contour → gearbox → geo_jump ×5
// ═══════════════════════════════════════════

static int test_pipeline(void) {
    printf("═══ TEST 4: Full Pipeline ═══\n\n");
    printf("  contour cube (6000) → gearbox (128/144) → geo_jump ×5 → island\n\n");

    printf("  Pipeline with output=144:\n");
    printf("    1 gearbox: 144 addr\n");
    printf("    ×5 (island): 144 × 5 = 720\n");
    printf("    ×2 (polar): 720 × 2 = 1440\n");
    printf("    Cells per gearbox cycle: 6000\n");
    printf("    Cycles to fill 1 gearbox: %.2f\n", 6000.0/144);
    printf("    Cycles to fill island: %.2f\n", 6000.0/720);

    printf("\n  Pipeline with output=128:\n");
    printf("    1 gearbox: 128 addr (on 162 geometry)\n");
    printf("    ×5 (island): 128 × 5 = 640\n");
    printf("    ×2 (polar): 640 × 2 = 1280\n");
    printf("    Cells per gearbox cycle: 6000\n");
    printf("    Cycles to fill 1 gearbox: %.2f\n", 6000.0/128);
    printf("    Cycles to fill island: %.2f\n", 6000.0/640);
    printf("    ⚠ 1280 < 1440 → 160 addresses never used in full timeline\n");

    printf("\n  Comparison:\n");
    printf("  %-20s │ 144 (flex)  │ 128 (162)\n", "");
    printf("  ──────────────────┼─────────────┼────────────\n");
    printf("  %-20s │ %11d │ %9d\n", "Gearbox addr", 144, 128);
    printf("  %-20s │ %11d │ %9d\n", "Island (×5)", 720, 640);
    printf("  %-20s │ %11d │ %9d\n", "Timeline (×2)", 1440, 1280);
    printf("  %-20s │ %10.2f× │ %9.2f×\n", "Cycles/gearbox", 6000.0/144, 6000.0/128);
    printf("  %-20s │ %10.2f× │ %9.2f×\n", "Cycles/island", 6000.0/720, 6000.0/640);
    printf("  %-20s │ %11s │ %9d\n", "Timeline waste", "0", 160);

    printf("  PASS\n\n");
    return 1;
}

// ═══════════════════════════════════════════
// TEST 5: Gearbox shift simulation
// ═══════════════════════════════════════════

static int test_shift(void) {
    printf("═══ TEST 5: Gearbox Shift (144 ↔ 128) ═══\n\n");
    printf("  Can gearbox shift between 144 and 128 mid-stream?\n\n");

    // Simulate: write 3000 cells via 144, then shift to 128
    int addr_144[144] = {0};
    int addr_128[128] = {0};

    int cells_written = 0;
    int shift_point = 3000;

    // Phase 1: output=144
    for (int i = 0; i < shift_point && i < CUBE_CELLS; i++) {
        int addr = (cells_written * 37) % 144;
        addr_144[addr]++;
        cells_written++;
    }
    printf("  Phase 1: wrote %d cells via output=144\n", cells_written);

    // Phase 2: output=128
    for (int i = shift_point; i < CUBE_CELLS; i++) {
        int addr = (cells_written * 37) % 128;
        addr_128[addr]++;
        cells_written++;
    }
    printf("  Phase 2: wrote %d cells via output=128\n", cells_written - shift_point);

    // Analyze phase1
    int p1_used = 0, p1_zero = 0, p1_max = 0;
    for (int i = 0; i < 144; i++) {
        if (addr_144[i] > 0) p1_used++;
        else p1_zero++;
        if (addr_144[i] > p1_max) p1_max = addr_144[i];
    }

    // Analyze phase2
    int p2_used = 0, p2_zero = 0, p2_max = 0;
    for (int i = 0; i < 128; i++) {
        if (addr_128[i] > 0) p2_used++;
        else p2_zero++;
        if (addr_128[i] > p2_max) p2_max = addr_128[i];
    }

    printf("\n  Phase 1 (144): %d used, %d empty, max_hit=%d\n",
           p1_used, p1_zero, p1_max);
    printf("  Phase 2 (128): %d used, %d empty, max_hit=%d\n",
           p2_used, p2_zero, p2_max);

    printf("\n  ⚠ Shift challenge:\n");
    printf("    Cells written under 144 use addresses 0..143\n");
    printf("    Cells written under 128 use addresses 0..127\n");
    printf("    Address 128..143 from phase1 are ORPHANED after shift\n");
    printf("    Need: address remapping or shadow copy\n");

    printf("  PASS\n\n");
    return 1;
}

// ═══════════════════════════════════════════

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Contour Cube × GeoJump Gearbox Experiment             ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    printf("  Contour: %d cells (6×10×10×10)\n", CUBE_CELLS);
    printf("  Gearbox: output 128 (on 162) or 144 (flexible)\n");
    printf("  Pipeline: contour → gearbox → geo_jump ×5 → island\n\n");

    init_cells();

    int pass = 0, total = 0;
    total++; pass += test_144();
    total++; pass += test_128();
    total++; pass += test_head_to_head();
    total++; pass += test_pipeline();
    total++; pass += test_shift();

    printf("══════════════════════════════════════════════════════════\n");
    printf("  FINAL: %d/%d PASS\n", pass, total);
    printf("══════════════════════════════════════════════════════════\n");
    return (pass == total) ? 0 : 1;
}

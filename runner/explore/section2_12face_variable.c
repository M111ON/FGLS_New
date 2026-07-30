// section2_12face_variable.c
// Section 2: 12-face Variable Cube × GeoJump Capacity Analysis
//
// Tests multiple configurations:
//   Faces:   6 (cube), 12 (dodeca)
//   X×Y×Z:   5×5×5, 10×10×10, 20×20×20, 50×50×50
//   Pairs:   C(6,2)=15, C(12,2)=330
//   Timeline: 1440 (GEO_FIBO_CLOCK)
//
// For each config:
//   - Total capacity cells
//   - Surface coverage per timeline cycle
//   - GeoJump address space required
//   - Efficiency ratio
//   - Optimal face count at this resolution
//
// Compile: gcc -O2 -std=c11 -Icollection/dgls/geo/include -Icore \ 
//              -IHfolder -o section2.exe runner/explore/section2_12face_variable.c \
//              collection/dgls/geo/src/geo_jump.c -lm
// Run:     section2.exe
//
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <assert.h>

#include "geo_jump.h"   // GEO_FULL, GEO_TOWER, GEO_BLOCK, GEO_FIBO_CLOCK

// ── Test configurations ──

typedef struct {
    const char *name;
    int faces;       // 6 or 12
    int x, y, z;     // cube dimensions
    int pairs;       // C(faces, 2)
} CubeConfig;

#define N_CONFIGS 8
static const CubeConfig CONFIGS[N_CONFIGS] = {
    {"6-face 5x5x5",    6,  5, 5, 5,  15},
    {"6-face 10x10x10", 6, 10,10,10,  15},
    {"6-face 20x20x20", 6, 20,20,20,  15},
    {"6-face 50x50x50", 6, 50,50,50,  15},
    {"12-face 5x5x5",  12,  5, 5, 5, 330},
    {"12-face 10x10x10",12,10,10,10, 330},
    {"12-face 20x20x20",12,20,20,20, 330},
    {"12-face 50x50x50",12,50,50,50, 330},
};

// ── Helpers ──

// C(n, 2) = n*(n-1)/2
static int nC2(int n) { return n * (n - 1) / 2; }

// ── Test: Capacity analysis for all configs ──

static void t1_capacity_table(void)
{
    printf("=== T1: Capacity table (all configs) ===\n");
    printf("\n");
    printf("%-22s | %5s | %7s | %9s | %6s | %6s | %6s | %s\n",
           "Config", "cells", "surface", "C(f,2)", "towers", "geo_jmp", "eff%", "notes");
    printf("----------------------+-------+---------+---------+--------+--------+--------+------------------\n");

    for (int i = 0; i < N_CONFIGS; i++) {
        const CubeConfig *c = &CONFIGS[i];
        uint64_t cells  = (uint64_t)c->faces * c->x * c->y * c->z;
        int surface     = c->faces * c->x * c->y;  // per-tick surface
        int pairs       = nC2(c->faces);
        int towers      = pairs;              // one tower per pair
        uint64_t geo    = (uint64_t)pairs * GEO_BLOCK * 2;  // 48 addr × 2 polar
        double eff      = 100.0 * cells / geo;
        int cycles      = (int)ceil((double)cells / surface);
        int depth_z     = c->z;
        int overlap     = cycles / depth_z + ((cycles % depth_z) ? 1 : 0);

        char notes[64];
        if (c->faces == 6)
            snprintf(notes, 64, "%dx%dx%d=%d, 10cyc",
                     c->x, c->y, c->z, cells);
        else
            snprintf(notes, 64, "%dx%dx%d=%d, C(%d,2)=%d",
                     c->x, c->y, c->z, cells, c->faces, pairs);

        printf("%-22s | %5llu | %7d | %7d | %6d | %6llu | %5.1f%% | %s\n",
               c->name,
               (unsigned long long)cells,
               surface,
               pairs,
               towers,
               (unsigned long long)geo,
               eff,
               notes);
    }
    printf("\n");
}

// ── Test: Per-config timeline coverage ──

static void t2_per_config_coverage(void)
{
    printf("=== T2: Per-config coverage ===\n");

    for (int i = 0; i < N_CONFIGS; i++) {
        const CubeConfig *c = &CONFIGS[i];
        int surface = c->faces * c->x * c->y;
        uint64_t cells = (uint64_t)c->faces * c->x * c->y * c->z;
        int timeline = GEO_FIBO_CLOCK;  // 1440

        printf("\n  %s:\n", c->name);
        printf("    Cells:     %llu\n", (unsigned long long)cells);
        printf("    Surface:   %d (per tick)\n", surface);
        printf("    Timeline:  %d per cycle\n", timeline);
        printf("    1-cycle:  %s\n",
               surface <= timeline ? "Surface fully covered" : "Surface partially covered");
        printf("    Depth z:  %d\n", c->z);

        if (surface <= timeline) {
            // Surface fully covered in 1 cycle, dups spread z across cycles
            int dups_per_cycle = timeline - surface;
            printf("    Dups/cyc: %d\n", dups_per_cycle);
            printf("    Fill cyc: %d (z layers)\n", c->z);
        } else {
            // Surface overflows timeline
            double pct = 100.0 * timeline / surface;
            printf("    Coverage: %.1f%% per cycle\n", pct);
            double cycles = (double)cells / timeline;
            printf("    Cycles:   ~%.1f to cover all\n", cycles);
        }

        // Show geo_jump capacity match
        int pairs = nC2(c->faces);
        uint64_t geo_addr = (uint64_t)pairs * GEO_BLOCK * 2;
        printf("    C(%d,2)=%d pairs × 48×2 = %llu geo_jump addr\n",
               c->faces, pairs, (unsigned long long)geo_addr);
        printf("    VS %llu cube cells: %s\n",
               (unsigned long long)cells,
               geo_addr >= cells ? "geo_jump fits ✓" : "geo_jump OVERFLOW ✗");
    }
    printf("\n");
}

// ── Test: 12-face C(12,2) = 330 pair distribution ──

static void t3_pair_distribution(void)
{
    printf("=== T3: C(12,2) vs C(6,2) distribution ===\n");

    // 6-face: 15 pairs, each gets full tower (144 addr) → 48 used + 96 spare per tower
    // 12-face: 330 pairs, 20736 total geo_jump addr / 330 pairs ≈ 62.8 addr/pair
    //          With 48 addr/pair: 330 × 48 × 2 = 31680 > 20736? No, 48×2=96 per pair
    //          330 × 96 = 31680 > 20736! This doesn't fit.
    //
    // Actually the 48 addr × 2 polar was for C(6,2). For C(12,2) we have more pairs
    // and need to distribute differently.

    printf("\n  6-face address space:\n");
    printf("    Pairs:     C(6,2) = %d\n", nC2(6));
    printf("    Addr/pair: %d (addr) × 2 (polar) = %d slot\n", GEO_BLOCK, GEO_BLOCK*2);
    printf("    Total:     %d × %d = %d  (= GEO_FIBO_CLOCK)\n",
           nC2(6), GEO_BLOCK*2, nC2(6)*GEO_BLOCK*2);

    printf("\n  12-face address space:\n");
    printf("    Pairs:     C(12,2) = %d\n", nC2(12));
    printf("    Each pair needs %d (addr) × 2 (polar) = %d\n", GEO_BLOCK, GEO_BLOCK*2);
    uint64_t needed = (uint64_t)nC2(12) * GEO_BLOCK * 2;
    printf("    Total:     %d × %d = %llu  (vs GEO_FULL=%u)\n",
           nC2(12), GEO_BLOCK*2, (unsigned long long)needed, GEO_FULL);
    printf("    Fits in GEO_FULL: %s\n", needed <= GEO_FULL ? "YES ✓" : "NO ✗");

    // Address per pair if spread evenly
    double addr_per_pair = (double)GEO_FULL / nC2(12);
    printf("    Even spread: %.1f addr/pair (%.1f addr + %.1f polar)\n",
           addr_per_pair, addr_per_pair/2, addr_per_pair/2);

    // How many pairs can fit with 48 addr each?
    int pairs_with_48 = GEO_FULL / (GEO_BLOCK * 2);
    printf("    Pairs with %d addr×2: %d (out of %d)\n",
           GEO_BLOCK, pairs_with_48, nC2(12));

    printf("\n  Strategy: C(6,2) 15 pairs + C(12,2) 330 pairs\n");
    printf("    C(6,2)  uses 15 × 96 = %d geo_jump slots\n", 15*96);
    printf("    C(12,2) uses all %d geo_jump slots\n", GEO_FULL);
    printf("    Ratio: C(12,2)/C(6,2) = 22× more pairs\n");

    printf("\n  Address allocation per pair:\n");
    printf("    6-face: %d addr/tower/polar → 15 towers\n", GEO_BLOCK);
    printf("    12-face: need %.1f addr/pair → packs multiple pairs per tower\n",
           (double)GEO_FULL / nC2(12));
    printf("\n");
}

// ── Test: Cube size scaling ──

static void t4_size_scaling(void)
{
    printf("=== T4: Size scaling — when does 48-tower addressing break? ===\n");

    int sizes[] = {5, 10, 20, 30, 40, 50, 100, 200};
    int n_sizes = sizeof(sizes) / sizeof(sizes[0]);

    printf("\n  For each cube size, cells = faces × X×Y×Z vs geo_jump capacity:\n");
    printf("  %-8s | %10s | %10s | %10s | %10s\n",
           "size", "6-face", "12-face", "C(6,2)=15", "C(12,2)=330");
    printf("----------+------------+------------+------------+------------\n");

    uint64_t geo_6  = (uint64_t)nC2(6)  * GEO_BLOCK * 2;  // 1440
    uint64_t geo_12 = (uint64_t)nC2(12) * GEO_BLOCK * 2;  // 31680
    uint64_t geo_full = GEO_FULL;                           // 20736

    for (int i = 0; i < n_sizes; i++) {
        int s = sizes[i];
        uint64_t cells_6  = (uint64_t)6  * s * s * s;
        uint64_t cells_12 = (uint64_t)12 * s * s * s;

        char buf_6[12], buf_12[12];
        snprintf(buf_6, 12, cells_6 <= geo_6 ? "%llu ✓" : "%llu ✗",
                 (unsigned long long)cells_6);
        snprintf(buf_12, 12, cells_12 <= geo_12 ? "%llu ✓" : "%llu ✗",
                 (unsigned long long)cells_12);

        printf("  %-8s | %10s | %10s | %10s | %10s\n",
               s == 100 ? "100" : (s == 200 ? "200" : (char[]){s/10+'0','x',s/10+'0','x',s/10+'0',0}),
               buf_6, buf_12,
               cells_6 <= geo_6 ? "OK" : "OVERFLOW",
               cells_12 <= geo_full ? "OK" : "OVERFLOW");
    }

    // Find break point: size where 6-face overflows C(6,2) geo_jump
    int break_6 = 0;
    for (int s = 1; s < 200; s++) {
        if ((uint64_t)6 * s * s * s > geo_6) { break_6 = s; break; }
    }
    int break_12 = 0;
    for (int s = 1; s < 200; s++) {
        if ((uint64_t)12 * s * s * s > geo_full) { break_12 = s; break; }
    }
    printf("\n  Break points (cells > geo_jump capacity):\n");
    printf("    6-face  C(6,2)  (1440 slots): size > %d  (cells=%llu)\n",
           break_6, (unsigned long long)6 * break_6 * break_6 * break_6);
    printf("    12-face C(12,2) (20736 slots): size > %d  (cells=%llu)\n",
           break_12, (unsigned long long)12 * break_12 * break_12 * break_12);
    printf("\n");
}

// ── Test: Orthogonality (6-face vs 12-face projection) ──

static void t5_orthogonality(void)
{
    printf("=== T5: Orthogonality test (6 vs 12 face) ===\n");

    // Test: generate some deterministic addresses
    // and check if 6-face and 12-face projections are orthogonal

    printf("\n  6-face: %d faces, %d pairs, %d addr/pair\n",
           6, nC2(6), GEO_BLOCK);
    printf("  12-face: %d faces, %d pairs\n", 12, nC2(12));
    printf("\n");

    // Build 6-face pair→tower mapping
    // Each of the 15 pairs maps to tower 0..14
    printf("  6-face: pair→tower mapping:\n");
    for (int d1 = 0; d1 < 6; d1++) {
        for (int d2 = d1+1; d2 < 6; d2++) {
            int idx = 0;
            for (int i = 0; i < d1; i++) idx += (6 - 1 - i);
            idx += (d2 - d1 - 1);
            printf("    %c%c → tower %2d  (addr 0..%d, polar=0..1)\n",
                   'A'+d1, 'A'+d2, idx, GEO_BLOCK-1);
        }
    }

    printf("\n  12-face: pair→tower mapping (sample first 15 of 330):\n");
    printf("    (12 faces × 12 faces, skipping same+mirror)\n");
    for (int d1 = 0; d1 < 6; d1++) {
        for (int d2 = d1+1; d2 < 12; d2++) {
            // Compute C(12,2) index
            int idx = 0;
            for (int i = 0; i < d1; i++) idx += (12 - 1 - i);
            idx += (d2 - d1 - 1);
            // Map to tower: pair idx * slots_per_pair / GEO_TOWER
            double slots_each = (double)GEO_FULL / nC2(12);
            int tower = (int)(idx * slots_each / GEO_TOWER);
            int offset = (int)((idx * slots_each - tower * GEO_TOWER));
            int polar_slots = (int)(slots_each / 2);

            printf("    %c%c → pair %3d → tower %3d, offset %3d (polar:%d)\n",
                   'A'+d1, d1 < 6 ? 'A'+d2 : 'A'+(d2-6), idx, tower, offset, polar_slots);
            if (idx >= 14) break;
        }
        if (d1 >= 1) break; // Show only first ~15 of 330
    }
    printf("    ... (%d pairs total)\n", nC2(12));

    // Orthogonality: check that 6-face and 12-face address spaces don't overlap
    printf("\n  Address space overlap:\n");

    // 6-face uses: towers 0..14 (144 addr each = 2160 total)
    uint64_t addr_6_start = 0;
    uint64_t addr_6_end   = (uint64_t)nC2(6) * GEO_TOWER;  // 15*144 = 2160

    // 12-face uses: all 20736 addresses (spread across 330 pairs)
    printf("    6-face  range: [0, %llu)\n", (unsigned long long)addr_6_end);
    printf("    12-face range: [0, %u)\n", GEO_FULL);
    printf("    Overlap: 6-face is subset of 12-face space\n");
    printf("    Orthogonality comes from PAIR DIRECTION, not address space\n");
    printf("\n");
}

// ── Test: Flex point (when 12-face beats 6-face) ──

static void t6_flex_point(void)
{
    printf("=== T6: Flex point — when does 12-face become beneficial? ===\n");

    printf("\n  Metrics comparison:\n");
    printf("  %-10s | %10s | %10s | %10s | %10s\n",
           "size", "6-cells", "12-cells", "6/resol", "12/resol");
    printf("------------+------------+------------+------------+------------\n");

    int sizes[] = {3, 5, 7, 10, 12, 15, 20};
    for (int i = 0; i < 7; i++) {
        int s = sizes[i];
        uint64_t c6  = (uint64_t)6  * s * s * s;
        uint64_t c12 = (uint64_t)12 * s * s * s;

        // Resolution = cells per geo_jump address
        double r6  = (double)c6  / (nC2(6)  * GEO_BLOCK * 2);
        double r12 = (double)c12 / GEO_FULL;

        char sname[12];
        if (s < 10) snprintf(sname, 12, "%dx%dx%d", s, s, s);
        else {
            int t = s/10;
            snprintf(sname, 12, "%dx%dx%d", s, s, s);
        }

        printf("  %-10s | %10llu | %10llu | %9.1f | %9.1f\n",
               sname,
               (unsigned long long)c6, (unsigned long long)c12,
               r6, r12);
    }

    printf("\n");
    printf("  Interpretation:\n");
    printf("    Resolution = cells per geo_jump address\n");
    printf("    Higher resolution = finer granularity = better\n");
    printf("    12-face has 22× more pairs → higher resolution at same cube size\n");
    printf("    6-face uses only 7%% of geo_jump space (1440/20736)\n");
    printf("    12-face uses 100%% of geo_jump space — full utilization\n");
    printf("\n");
}

// ── Main ──

int main(void)
{
    printf("============================================================\n");
    printf("  Section 2: 12-face Variable Cube × GeoJump\n");
    printf("============================================================\n");
    printf("\n");
    printf("  Constants:  GEO_FULL=%u, GEO_TOWER=%u, GEO_BLOCK=%u\n",
           GEO_FULL, GEO_TOWER, GEO_BLOCK);
    printf("             GEO_FIBO_CLOCK=%u\n", GEO_FIBO_CLOCK);
    printf("  Pairs:      C(6,2)=%d, C(12,2)=%d\n", nC2(6), nC2(12));
    printf("  6-face geo: %d × %d × 2 = %d\n",
           nC2(6), GEO_BLOCK, nC2(6) * GEO_BLOCK * 2);
    printf("  12-face geo: %d pairs spreading across %d\n",
           nC2(12), GEO_FULL);
    printf("  12-face max cells: %llu (at 48 addr/pair)\n",
           (unsigned long long)nC2(12) * GEO_BLOCK * 2);
    printf("\n");

    t1_capacity_table();
    t2_per_config_coverage();
    t3_pair_distribution();
    t4_size_scaling();
    t5_orthogonality();
    t6_flex_point();

    printf("============================================================\n");
    printf("  FINAL: All tests informational — no pass/fail\n");
    printf("============================================================\n");
    return 0;
}

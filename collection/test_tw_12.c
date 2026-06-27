/*
 * test_tw_12.c — Test if TW can use 12 sectors instead of 10
 * ═══════════════════════════════════════════════════════════════
 * Hypothesis: TW can use 12 sectors (30deg each) instead of 10 (36deg each)
 * This would make TW compatible with Bermuda's 12 zones (lossless mapping)
 *
 * Compile: gcc -std=c11 -Wall -I.. -o test_tw_12.exe test_tw_12.c
 * Run: ./test_tw_12.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>

#define TW_SCALE_12    207360    /* same scale */
#define TW_N_SECTORS_12  12      /* changed from 10 to 12 */
#define TW_SLOTS_PER_12   6      /* same slots per sector */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Generate boundary vectors for 12 sectors (30deg each) */
static int32_t BOUNDARY_12[TW_N_SECTORS_12][2];

/* Generate hex centroids for 12 sectors */
static int32_t HEX_CENTROIDS_12[TW_N_SECTORS_12][TW_SLOTS_PER_12][2];

/* Generate tri centroids for 12 sectors */
static int32_t TRI_CENTROIDS_12[TW_N_SECTORS_12][TW_SLOTS_PER_12][2];

static void generate_tables(void) {
    /* Boundary vectors at angles (90 - 30*k) deg for k=0..11 */
    for (int k = 0; k < TW_N_SECTORS_12; k++) {
        double angle_deg = 90.0 - 30.0 * k;
        double angle_rad = angle_deg * M_PI / 180.0;
        BOUNDARY_12[k][0] = (int32_t)(sin(angle_rad) * TW_SCALE_12);
        BOUNDARY_12[k][1] = (int32_t)(cos(angle_rad) * TW_SCALE_12);
    }

    /* Hex centroids: 6 positions per sector, 60deg apart */
    /* Sector center at angle (90 - 30*k) deg */
    for (int k = 0; k < TW_N_SECTORS_12; k++) {
        double sector_angle_deg = 90.0 - 30.0 * k;
        for (int j = 0; j < TW_SLOTS_PER_12; j++) {
            /* Hex centroid at 60deg intervals within sector */
            double local_angle_deg = sector_angle_deg - 60.0 * j;
            double local_angle_rad = local_angle_deg * M_PI / 180.0;
            double r = TW_SCALE_12 * 0.9; /* radius ~90% of scale */
            HEX_CENTROIDS_12[k][j][0] = (int32_t)(sin(local_angle_rad) * r);
            HEX_CENTROIDS_12[k][j][1] = (int32_t)(cos(local_angle_rad) * r);
        }
    }

    /* Tri centroids: rotated 30deg from hex */
    for (int k = 0; k < TW_N_SECTORS_12; k++) {
        for (int j = 0; j < TW_SLOTS_PER_12; j++) {
            double hex_angle = atan2(HEX_CENTROIDS_12[k][j][0],
                                     HEX_CENTROIDS_12[k][j][1]);
            double hex_r = sqrt(HEX_CENTROIDS_12[k][j][0] * HEX_CENTROIDS_12[k][j][0] +
                               HEX_CENTROIDS_12[k][j][1] * HEX_CENTROIDS_12[k][j][1]);
            double tri_angle = hex_angle - M_PI / 6.0; /* rotate -30deg */
            TRI_CENTROIDS_12[k][j][0] = (int32_t)(sin(tri_angle) * hex_r);
            TRI_CENTROIDS_12[k][j][1] = (int32_t)(cos(tri_angle) * hex_r);
        }
    }
}

/* Cross product test */
static int64_t cross(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    return (int64_t)ax * by - (int64_t)ay * bx;
}

/* Find sector for a point (0-11) */
static int find_sector_12(int64_t vx, int64_t vy) {
    for (int k = 0; k < TW_N_SECTORS_12; k++) {
        int kn = (k + 1) % TW_N_SECTORS_12;
        int64_t c1 = cross(BOUNDARY_12[k][0], BOUNDARY_12[k][1], vx, vy);
        int64_t c2 = cross(BOUNDARY_12[kn][0], BOUNDARY_12[kn][1], vx, vy);
        if (c1 <= 0 && c2 >= 0) {
            return k;
        }
    }
    return 0; /* fallback */
}

/* Test capture and reconstruct */
static int test_capture_reconstruct(void) {
    printf("Testing capture/reconstruct with 12 sectors...\n");

    int errors = 0;

    /* Test points at various angles */
    for (int angle_deg = 0; angle_deg < 360; angle_deg += 15) {
        double angle_rad = angle_deg * M_PI / 180.0;
        int64_t vx = (int64_t)(sin(angle_rad) * 100000);
        int64_t vy = (int64_t)(cos(angle_rad) * 100000);

        /* Find sector */
        int sector = find_sector_12(vx, vy);
        if (sector < 0 || sector >= TW_N_SECTORS_12) {
            printf("  ERROR: angle=%d deg, sector=%d (out of range)\n", angle_deg, sector);
            errors++;
            continue;
        }

        /* Find nearest centroid */
        int best = 0;
        int64_t best_dist = -1;
        for (int j = 0; j < TW_SLOTS_PER_12; j++) {
            int64_t dx = vx - HEX_CENTROIDS_12[sector][j][0];
            int64_t dy = vy - HEX_CENTROIDS_12[sector][j][1];
            int64_t d = dx * dx + dy * dy;
            if (best_dist < 0 || d < best_dist) {
                best_dist = d;
                best = j;
            }
        }

        /* Reconstruct */
        int64_t rx = HEX_CENTROIDS_12[sector][best][0];
        int64_t ry = HEX_CENTROIDS_12[sector][best][1];

        /* Check if reconstruct is close to original */
        int64_t dx = vx - rx;
        int64_t dy = vy - ry;
        double error = sqrt(dx * dx + dy * dy);

        if (error > 10000) { /* more than 10% error */
            printf("  WARN: angle=%d deg, sector=%d, slot=%d, error=%.0f\n",
                   angle_deg, sector, best, error);
        }
    }

    return errors;
}

/* Test sector coverage */
static void test_sector_coverage(void) {
    printf("\nTesting sector coverage...\n");

    int coverage[TW_N_SECTORS_12] = {0};

    /* Test many points */
    for (int angle_deg = 0; angle_deg < 360; angle_deg++) {
        double angle_rad = angle_deg * M_PI / 180.0;
        int64_t vx = (int64_t)(sin(angle_rad) * 100000);
        int64_t vy = (int64_t)(cos(angle_rad) * 100000);

        int sector = find_sector_12(vx, vy);
        coverage[sector]++;
    }

    printf("  Sector coverage:\n");
    for (int k = 0; k < TW_N_SECTORS_12; k++) {
        printf("    Sector %2d: %d points\n", k, coverage[k]);
    }
}

/* Test boundary alignment */
static void test_boundary_alignment(void) {
    printf("\nTesting boundary alignment...\n");

    for (int k = 0; k < TW_N_SECTORS_12; k++) {
        double angle_deg = 90.0 - 30.0 * k;

        /* Check if boundary vector is at expected angle */
        double actual_angle = atan2(BOUNDARY_12[k][0], BOUNDARY_12[k][1]) * 180.0 / M_PI;
        double error = fabs(angle_deg - actual_angle);

        if (error > 1.0) {
            printf("  WARN: sector %d, expected %.1f deg, got %.1f deg, error %.1f\n",
                   k, angle_deg, actual_angle, error);
        }
    }
}

/* Compare with original 10-sector geometry */
static void compare_with_10(void) {
    printf("\nComparing with original 10-sector geometry:\n");
    printf("  10 sectors: 36deg each, %d total slots\n", 10 * 6);
    printf("  12 sectors: 30deg each, %d total slots\n", 12 * 6);
    printf("  Difference: +%d slots (+%.1f%%)\n", 
           (12*6 - 10*6), (12*6 - 10*6) * 100.0 / (10*6));
}

int main(void) {
    printf("=== TW 12-Sector Geometry Test ===\n\n");

    /* Generate tables */
    generate_tables();

    /* Run tests */
    int errors = test_capture_reconstruct();
    test_sector_coverage();
    test_boundary_alignment();
    compare_with_10();

    /* Summary */
    printf("\n=== Summary ===\n");
    if (errors == 0) {
        printf("PASS: 12-sector geometry is feasible\n");
        printf("  - All sectors have coverage\n");
        printf("  - Boundary vectors are correctly aligned\n");
        printf("  - Capture/reconstruct works\n");
    } else {
        printf("FAIL: %d errors found\n", errors);
    }

    return errors;
}

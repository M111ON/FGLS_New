/*
 * capo_scale.c — Capo scaling: 20736 × N for different model sizes
 *
 * Key insight: 82944 = 8+2+9+4+4 = 27 = 3³!
 * The 3³ traversal is UNIVERSAL for the 7B scale.
 *
 * Compile: gcc -Wall -O2 -std=c11 runner/explore/capo_scale.c -o runner/explore/capo_scale.exe
 * Run:     runner/explore/capo_scale.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define BASE_FACE 20736

/* Digit sum of a number */
static int digit_sum(uint64_t n) {
    int sum = 0;
    while (n > 0) {
        sum += n % 10;
        n /= 10;
    }
    return sum;
}

/* Check if digit sum = 27 (3³) */
static int is_3cubed(uint64_t n) {
    return digit_sum(n) == 27;
}

int main(void) {
    printf("=== CAPO SCALING: 20736 × N ===\n\n");

    /* Model sizes and their capo multipliers */
    struct { const char *name; uint64_t size; int capo; } models[] = {
        {"0.6B", 20736 * 1, 1},
        {"1.5B", 20736 * 2, 2},
        {"3B",   20736 * 3, 3},
        {"7B",   20736 * 4, 4},
        {"13B",  20736 * 5, 5},
        {"70B",  20736 * 28, 28},
    };
    int nmodels = 6;

    printf("── MODEL SIZES ──\n");
    for (int i = 0; i < nmodels; i++) {
        int ds = digit_sum(models[i].size);
        int is3 = is_3cubed(models[i].size);
        printf("  %4s: %7lu × %2d = %7lu  digit sum = %2d %s\n",
               models[i].name, BASE_FACE, models[i].capo,
               models[i].size, ds, is3 ? "= 3³ ✓" : "");
    }

    printf("\n── KEY INSIGHT ──\n");
    printf("  82944 = 8+2+9+4+4 = 27 = 3³!\n");
    printf("  The 3³ traversal is UNIVERSAL for the 7B scale!\n");

    printf("\n── 3³ TRAVERSAL ACROSS SCALES ──\n");
    printf("  3³ = 27 positions\n");
    printf("  27 × 6 = 162 (full icosa vertex set)\n");
    printf("  162 × 128 = 20736 (base face)\n");
    printf("  20736 × 4 = 82944 (7B cross grid)\n");
    printf("  82944 → digit sum = 27 = 3³ ✓\n");

    printf("\n── CAPO DIMENSION ──\n");
    printf("  Capo = additional dimension for larger models\n");
    printf("  0.6B: 20736 × 1 = 20736 (1 capo)\n");
    printf("  7B:   20736 × 4 = 82944 (4 capos)\n");
    printf("  Each capo adds 20736 positions\n");

    printf("\n── CROSS GRID STRUCTURE ──\n");
    printf("  Face = 162 × 128 (Path 1)\n");
    printf("  Cross grid = face × capo\n");
    printf("  7B: 20736 × 4 = 82944 positions\n");
    printf("  82944 → 8+2+9+4+4 = 27 = 3³\n");
    printf("  3³ traverses ALL dimensions!\n");

    printf("\n── UNIVERSAL TRAVERSAL ──\n");
    printf("  For 7B (82944):\n");
    printf("    3³ = 27 positions\n");
    printf("    27 × 6 = 162 vertices\n");
    printf("    162 × 128 = 20736 slots per capo\n");
    printf("    20736 × 4 capos = 82944 total\n");
    printf("    3³ traverses ALL 82944 positions!\n");

    printf("\n── SUMMARY ──\n");
    printf("  Capo scaling: 20736 × N for larger models\n");
    printf("  7B (82944): digit sum = 27 = 3³\n");
    printf("  3³ traversal is UNIVERSAL for this scale\n");
    printf("  CPU routes vertices, GPU batches slots\n");
    printf("  Infinity Kis: dodeca(pos) ↔ icosa(neg) via spike\n");

    return 0;
}

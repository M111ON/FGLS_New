/* cylinder_intersect.c — Two helices on cylinder, intersection pattern */
#include <stdio.h>
#include <math.h>

#define C 6        /* circumference (faces) */
#define H 3456     /* height */

/* Helix on unrolled rectangle: z = (angle * theta) mod H
   Two helices intersect when:
   (a1 * t) mod H = (a2 * t) mod H
   => (a1 - a2) * t ≡ 0 (mod H)
   => t = H / gcd(a1 - a2, H)  — spacing between intersections
*/

int gcd(int a, int b) {
    while (b) { int t = b; b = a % b; a = t; }
    return a;
}

int main() {
    printf("╔══ Cylinder Intersection Pattern ══╗\n");
    printf("Circumference: %d (faces)\n", C);
    printf("Height: %d\n\n", H);

    /* Test 1: angle difference vs intersection count */
    printf("═══ Test 1: Angle Difference → Intersection Count ═══\n\n");
    printf("%-12s %-12s %-12s %-12s\n", "Δangle", "gcd", "intersections", "spacing");
    printf("─────────────────────────────────────────────────\n");
    
    for (int da = 1; da <= 36; da++) {
        int g = gcd(da, H);
        int count = g;           /* number of intersection points */
        int spacing = H / g;     /* distance between intersections */
        printf("%-12d %-12d %-12d %-12d\n", da, g, count, spacing);
    }

    /* Test 2: Visualize two helices on small cylinder */
    printf("\n═══ Test 2: Visualize (C=6, H=48 for visibility) ═══\n\n");
    int h2 = 48;
    int a1 = 8, a2 = 3;  /* two different angles */
    printf("Helix A: angle=%d, Helix B: angle=%d\n\n", a1, a2);
    
    /* Draw unrolled rectangle */
    printf("     ");
    for (int t = 0; t < C * 8; t++) printf("%d", t % 10);
    printf("\n");
    
    for (int z = h2 - 1; z >= 0; z--) {
        printf("%3d |", z);
        for (int t = 0; t < C * 8; t++) {
            int za = (a1 * t) % h2;
            int zb = (a2 * t) % h2;
            if (za == z && zb == z) printf("*");  /* intersection */
            else if (za == z) printf("A");
            else if (zb == z) printf("B");
            else printf(".");
        }
        printf("\n");
    }

    /* Test 3: Intersection points for C=6, H=3456 */
    printf("\n═══ Test 3: Intersection Points (C=6, H=3456) ═══\n\n");
    a1 = 8; a2 = 3;
    int da = a1 - a2;
    if (da < 0) da = -da;
    int g = gcd(da, H);
    int n_intersections = g;
    int spacing = H / g;
    
    printf("Δangle = %d - %d = %d\n", a1, a2, da);
    printf("gcd(%d, %d) = %d\n", da, H, g);
    printf("Intersection count: %d\n", n_intersections);
    printf("Spacing: %d steps\n\n", spacing);
    
    printf("First 20 intersection points (θ, z):\n");
    for (int i = 0; i < 20 && i < n_intersections; i++) {
        int t = i * spacing;
        int z = (a1 * t) % H;
        printf("  [%2d] θ=%5d, z=%5d\n", i, t, z);
    }

    /* Test 4: All angle differences and their intersection counts */
    printf("\n═══ Test 4: Full Distribution ═══\n\n");
    printf("Angle diff distribution for H=%d:\n\n", H);
    int buckets[37] = {0}; /* max gcd = 36 for practical angles */
    for (int d = 1; d <= H; d++) {
        int g = gcd(d, H);
        if (g <= 36) buckets[g]++;
    }
    printf("%-12s %-12s\n", "gcd (count)", "angle diffs with this gcd");
    printf("─────────────────────────────\n");
    for (int i = 1; i <= 36; i++) {
        if (buckets[i] > 0) {
            printf("%-12d %-12d\n", i, buckets[i]);
        }
    }

    /* Test 5: Weight encoding concept */
    printf("\n═══ Test 5: Weight → Angle → Intersection ═══\n\n");
    printf("Weight value (0-255) → angle = value * 360 / 256\n");
    printf("Two weights w1, w2 → angle difference → intersection count\n\n");
    
    printf("%-8s %-8s %-10s %-12s %-12s\n", "w1", "w2", "Δangle", "intersections", "spacing");
    printf("──────────────────────────────────────────────\n");
    
    int test_weights[][2] = {{0,0},{0,1},{0,128},{64,128},{128,255},{0,255}};
    int n_tests = sizeof(test_weights) / sizeof(test_weights[0]);
    
    for (int i = 0; i < n_tests; i++) {
        int w1 = test_weights[i][0], w2 = test_weights[i][1];
        int ang1 = w1 * 360 / 256;
        int ang2 = w2 * 360 / 256;
        int diff = ang1 - ang2; if (diff < 0) diff = -diff;
        if (diff == 0) {
            printf("%-8d %-8d %-10d %-12s %-12s\n", w1, w2, diff, "∞ (same)", "0");
        } else {
            int g2 = gcd(diff, H);
            printf("%-8d %-8d %-10d %-12d %-12d\n", w1, w2, diff, g2, H/g2);
        }
    }

    return 0;
}

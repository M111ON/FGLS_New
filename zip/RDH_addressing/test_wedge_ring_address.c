/*
 * test_wedge_ring_address.c
 * Build:  gcc -O2 -o test_wedge test_wedge_ring_address.c -lm
 * Run:    ./test_wedge
 */

#include <stdio.h>
#include <stdlib.h>
#include "wedge_ring_address.h"

static int64_t *g_keys;
static int64_t g_count = 0;

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

int main(void) {
    printf("N_WEDGES=%d (wedge angle=%d deg), N_RINGS=%d\n",
           N_WEDGES, WEDGE_ANGLE_DEG, N_RINGS);
    printf("Total address space = %lld points\n", (long long)total_address_space());

    /* sample lookups matching the Python demo */
    struct { int32_t ring, wedge, mirror, u, v; } samples[] = {
        {0, 0, 0, 0, 0},
        {0, 0, 1, 0, 0},
        {5, 23, 0, 3, 3},
    };

    for (int i = 0; i < 3; i++) {
        Address addr; Coord coord;
        build_address(samples[i].ring, samples[i].wedge, samples[i].mirror,
                      samples[i].u, samples[i].v, &addr, &coord);
        int64_t key = address_key(&addr);
        printf("ring=%d wedge=%2d mirror=%d u=%d v=%d -> key=%8lld -> coord=(%lld, %lld)\n",
               addr.ring_index, addr.wedge_index, addr.mirror_flag, addr.u, addr.v,
               (long long)key, (long long)coord.x, (long long)coord.y);
    }

    /* full uniqueness verification, same as Python enumerate_all() check */
    int64_t total = total_address_space();
    g_keys = malloc(sizeof(int64_t) * total);
    g_count = 0;

    for (int r = 0; r < N_RINGS; r++)
        for (int w = 0; w < N_WEDGES; w++)
            for (int m = 0; m < 2; m++)
                for (int u = 0; u < POINTS_PER_WEDGE_EDGE; u++)
                    for (int v = 0; v < POINTS_PER_WEDGE_EDGE; v++) {
                        Address addr; Coord coord;
                        build_address(r, w, m, u, v, &addr, &coord);
                        g_keys[g_count++] = address_key(&addr);
                    }

    qsort(g_keys, g_count, sizeof(int64_t), cmp_i64);

    int collision = 0;
    for (int64_t i = 1; i < g_count; i++) {
        if (g_keys[i] == g_keys[i - 1]) { collision = 1; break; }
    }

    if (collision) {
        printf("COLLISION DETECTED in address keys!\n");
        free(g_keys);
        return 1;
    }

    printf("Verified: all %lld address keys are unique.\n", (long long)g_count);
    free(g_keys);
    return 0;
}

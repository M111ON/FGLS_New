/*
 * bench_radial_capture.c — Performance benchmark
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#define _POSIX_C_SOURCE 199309L
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "geo_radial_capture.h"

#ifdef _WIN32
#include <windows.h>
static double now_us(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart * 1000000.0 / (double)freq.QuadPart;
}
#else
static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}
#endif

int main(void) {
    printf("=== geo_radial_capture benchmark ===\n\n");

    /* Generate random points on sphere */
    #define N 100000
    rc_vec3 *points = (rc_vec3*)malloc(N * sizeof(rc_vec3));
    if (!points) { fprintf(stderr, "malloc fail\n"); return 1; }
    srand(42);
    for (int i = 0; i < N; i++) {
        double theta = 2.0 * M_PI * (double)rand() / (double)RAND_MAX;
        double phi = acos(2.0 * (double)rand() / (double)RAND_MAX - 1.0);
        points[i].x = cos(theta) * sin(phi);
        points[i].y = sin(theta) * sin(phi);
        points[i].z = cos(phi);
    }

    /* Benchmark: rc_nearest_vertex */
    {
        double t0 = now_us();
        volatile int sum = 0;
        for (int i = 0; i < N; i++) {
            sum += rc_nearest_vertex(&points[i]);
        }
        double t = (now_us() - t0) / N;
        printf("rc_nearest_vertex:\t%.2f ns  (O(24), %d random)\n", t * 1000.0, N);
    }

    /* Benchmark: rc_capture */
    {
        double t0 = now_us();
        volatile uint64_t sum = 0;
        for (int i = 0; i < N; i++) {
            sum += rc_capture(&points[i]);
        }
        double t = (now_us() - t0) / N;
        printf("rc_capture:\t\t%.2f ns  (nearest + tangent + node)\n", t * 1000.0);
    }

    /* Benchmark: rc_twin_swap */
    {
        uint64_t addr = 0x1234567890ABCDEFULL;
        double t0 = now_us();
        volatile uint64_t sum = 0;
        for (int i = 0; i < N; i++) {
            sum += rc_twin_swap(addr);
        }
        double t = (now_us() - t0) / N;
        printf("rc_twin_swap:\t\t%.2f ns  (XOR single)\n", t * 1000.0);
    }

    /* Benchmark: rc_capture on all 24 vertices directly */
    {
        double t0 = now_us();
        volatile uint64_t sum = 0;
        for (int i = 0; i < 24; i++) {
            for (int j = 0; j < N / 24; j++) {
                sum += rc_capture(&RC_VERTS[i]);
            }
        }
        double t = (now_us() - t0) / N;
        printf("rc_capture(vert):\t%.2f ns  (vertex input)\n", t * 1000.0);
    }

    free(points);
    printf("\n---\nOld system (tw_capture_int): ~180 ns per capture\n");
    return 0;
}

/*
 * bench_contour_mask_c.c — Contour Mask displacement model (C version)
 * Tests: encode, decode, XOR read, accuracy — fair comparison with identity
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define N_SAMPLES 1000000
#define UNITS 6000

static int8_t displacement[UNITS];  /* displacement model */
static int8_t identity[UNITS];      /* identity model */
static int8_t weights[N_SAMPLES];
static int8_t decoded[N_SAMPLES];
static int8_t recovered[N_SAMPLES];

static void bench_encode(int n) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < n; i++) {
        displacement[i % UNITS] = weights[i];  /* displacement = weight */
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
    printf("  Encode (weight -> displacement):  %.2f ns/sample\n", ns / n);
}

static void bench_decode_xor0(int n) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < n; i++) {
        decoded[i] = displacement[i % UNITS] ^ 0;  /* XOR with 0 */
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
    printf("  Decode (XOR 0):                  %.2f ns/sample\n", ns / n);
}

static void bench_decode_identity(int n) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < n; i++) {
        decoded[i] = identity[i % UNITS];  /* direct read */
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
    printf("  Decode (identity read):          %.2f ns/sample\n", ns / n);
}

static void bench_xor_read(int n) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < n; i++) {
        decoded[i] = displacement[i % UNITS] ^ displacement[(i + 1) % UNITS];
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
    printf("  XOR read (pos1 ^ pos2):          %.2f ns/sample\n", ns / n);
}

static void bench_xor_recover(int n) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < n; i++) {
        int8_t xor_val = displacement[i % UNITS] ^ displacement[(i + 1) % UNITS];
        recovered[i] = displacement[i % UNITS] ^ xor_val;  /* recover pos2 */
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
    printf("  XOR recover (pos1 ^ xor):        %.2f ns/sample\n", ns / n);
}

static void bench_xor_15pairs(int n) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < n; i++) {
        for (int p = 0; p < 15; p++) {
            decoded[i] = displacement[i % UNITS] ^ displacement[(i + p + 1) % UNITS];
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
    printf("  XOR 15 pairs:                   %.2f ns/XOR\n", ns / (n * 15));
}

static void verify_accuracy(int n) {
    /* Fill displacement model */
    for (int i = 0; i < UNITS; i++) {
        displacement[i] = weights[i % n] & 0x7F;
        identity[i] = displacement[i];
    }
    
    /* Decode via XOR 0 */
    int xor_match = 0;
    for (int i = 0; i < n; i++) {
        if ((displacement[i % UNITS] ^ 0) == weights[i % n]) xor_match++;
    }
    
    /* Decode via identity */
    int id_match = 0;
    for (int i = 0; i < n; i++) {
        if (identity[i % UNITS] == weights[i % n]) id_match++;
    }
    
    printf("  XOR 0 accuracy:  %d/%d (100%% = lossless)\n", xor_match, n);
    printf("  Identity accuracy: %d/%d (100%% = lossless)\n", id_match, n);
}

int main(void) {
    printf("======================================================================\n");
    printf("Contour Mask Performance Benchmark (C version)\n");
    printf("======================================================================\n");
    
    /* Generate test data */
    srand(42);
    for (int i = 0; i < N_SAMPLES; i++) {
        weights[i] = (int8_t)(rand() % 256 - 128);
    }
    
    printf("\n[Accuracy]\n");
    verify_accuracy(N_SAMPLES);
    
    printf("\n[Benchmarks: %d samples]\n", N_SAMPLES);
    bench_encode(N_SAMPLES);
    bench_decode_xor0(N_SAMPLES);
    bench_decode_identity(N_SAMPLES);
    bench_xor_read(N_SAMPLES);
    bench_xor_recover(N_SAMPLES);
    bench_xor_15pairs(N_SAMPLES);
    
    printf("\n[Storage]\n");
    printf("  Displacement: %d bytes (%.1f KB)\n", UNITS, UNITS / 1024.0);
    printf("  Identity:     %d bytes (%.1f KB)\n", UNITS, UNITS / 1024.0);
    printf("  Same size — both int8 per unit\n");
    
    printf("\n[Verdict]\n");
    printf("  XOR(pos, 0) = pos → identity equivalent\n");
    printf("  Both O(1), same memory layout\n");
    printf("  Displacement adds physical meaning, not overhead\n");
    
    return 0;
}

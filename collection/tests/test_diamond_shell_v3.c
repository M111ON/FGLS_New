/*
 * test_diamond_shell_v3.c — Performance test for Diamond Shell v3
 * 
 * Compile: gcc -O3 -std=c11 -I. -I./dgls/diamond/include -I./core/core \
 *          test_diamond_shell_v3.c dgls/diamond/src/diamond_shell_v3.c dgls/diamond/src/pogls_fold.c -o test_diamond_shell_v3.exe
 * 
 * Run: ./test_diamond_shell_v3.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "dgls/diamond/include/diamond_shell_v3.h"
#include "dgls/diamond/include/pogls_fold.h"

/* ── Test data generation ──────────────────────────────────── */

static void generate_test_chunks(uint8_t *data, uint32_t n_chunks, int pattern)
{
    unsigned int seed = 0xDEADBEEF;
    
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint8_t *chunk = data + i * 64;
        
        switch (pattern) {
            case 0: /* All zeros */
                memset(chunk, 0, 64);
                break;
            case 1: /* Repeating pattern */
                for (int j = 0; j < 64; j++) chunk[j] = (uint8_t)(j & 0xFF);
                break;
            case 2: /* Random */
                for (int j = 0; j < 64; j++) {
                    seed = seed * 1103515245 + 12345;
                    chunk[j] = (uint8_t)(seed >> 16);
                }
                break;
            case 3: /* Structured: low entropy with variations */
                for (int j = 0; j < 64; j++) {
                    chunk[j] = (uint8_t)((i * 7 + j * 13) & 0xFF);
                }
                break;
            case 4: /* Realistic: clustered values around centroids */
                for (int j = 0; j < 64; j++) {
                    seed = seed * 1103515245 + 12345;
                    float u = (float)(seed & 0x7FFFFFFF) / 0x7FFFFFFF;
                    if (u < 0.7f) {
                        chunk[j] = (uint8_t)(i % 256);
                    } else {
                        chunk[j] = (uint8_t)((seed >> 8) & 0xFF);
                    }
                }
                break;
        }
    }
}

/* ── Timing helpers ────────────────────────────────────────── */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void print_metrics(const char *name, const ShellMetrics *m, double elapsed_ms)
{
    printf("\n=== %s === (%.2f ms)\n", name, elapsed_ms);
    printf("  Chunks:     %llu\n", (unsigned long long)m->n_chunks);
    printf("  FLAT:       %llu (%.1f%%)\n", (unsigned long long)m->n_flat, 
           m->n_chunks ? 100.0 * m->n_flat / m->n_chunks : 0);
    printf("  SPARSE:     %llu (%.1f%%)\n", (unsigned long long)m->n_sparse,
           m->n_chunks ? 100.0 * m->n_sparse / m->n_chunks : 0);
    printf("  DENSE:      %llu (%.1f%%)\n", (unsigned long long)m->n_dense,
           m->n_chunks ? 100.0 * m->n_dense / m->n_chunks : 0);
    printf("  Raw bytes:  %llu\n", (unsigned long long)m->raw_bytes);
    printf("  Enc bytes:  %llu\n", (unsigned long long)m->enc_bytes);
    printf("  Ratio:      %.4fx\n", m->ratio);
    printf("  Throughput: %.2f MB/s\n", m->raw_bytes / (elapsed_ms / 1000.0) / 1e6);
    printf("  Avg isect pc: %.2f\n", m->n_chunks ? (double)m->isect_total / m->n_chunks : 0);
    printf("  Rot wins: ");
    for (int r = 0; r < 6; r++) {
        printf("R%d=%llu ", r, (unsigned long long)m->rot_wins[r]);
    }
    printf("\n");
}

int main(void)
{
    const uint32_t N_CHUNKS = 100000;
    printf("Diamond Shell v3 Performance Test\n");
    printf("Chunks: %u (%.2f MB)\n", N_CHUNKS, N_CHUNKS * 64.0 / 1048576.0);
    printf("═══════════════════════════════════════════════════════\n\n");

    uint8_t *data = (uint8_t*)malloc(N_CHUNKS * 64);
    if (!data) { fprintf(stderr, "OOM\n"); return 1; }

    int patterns[] = {0, 1, 2, 3, 4};
    const char *pattern_names[] = {"Zeros", "Sequential", "Random", "Structured", "Realistic"};
    int n_patterns = 5;

    for (int p = 0; p < n_patterns; p++) {
        printf("\n╔══════════════════════════════════════════════════════════════╗\n");
        printf("║  Pattern: %-12s                                        ║\n", pattern_names[p]);
        printf("╚══════════════════════════════════════════════════════════════╝\n");
        
        generate_test_chunks(data, N_CHUNKS, patterns[p]);

        /* Test v3 scalar */
        double t0 = now_ms();
        ShellMetrics m3s = v3_shell_encode(data, N_CHUNKS, 0, 0);
        double t1 = now_ms();
        print_metrics("v3 (scalar)", &m3s, t1 - t0);

        /* Test v3 batch (4 chunks at once) */
        double t2 = now_ms();
        ShellMetrics m3b = v3_shell_encode(data, N_CHUNKS, 0, 1);
        double t3 = now_ms();
        print_metrics("v3 (batch×4)", &m3b, t3 - t2);
    }

    /* Test fold_fibo_intersect directly */
    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("  fold_fibo_intersect micro-benchmark\n");
    printf("══════════════════════════════════════════════════════════════\n");

    DiamondBlock test_blocks[1000];
    for (int i = 0; i < 1000; i++) {
        test_blocks[i] = fold_block_init(i & 0x1F, i & 0x7F, i * 13, 1, i & 0xFF);
    }

    uint64_t sum = 0;
    double t6 = now_ms();
    for (int iter = 0; iter < 10000; iter++) {
        for (int i = 0; i < 1000; i++) {
            sum ^= fold_fibo_intersect(&test_blocks[i]);
        }
    }
    double t7 = now_ms();
    printf("  10M fold_fibo_intersect calls: %.2f ms\n", t7 - t6);
    printf("  Per call: %.2f ns\n", (t7 - t6) * 1e6 / 10000000.0);
    printf("  Sum (to prevent optimization): %llu\n", (unsigned long long)sum);

    free(data);
    printf("\n=== ALL TESTS COMPLETE ===\n");
    return 0;
}
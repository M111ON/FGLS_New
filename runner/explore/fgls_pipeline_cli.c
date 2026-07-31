/* fgls_pipeline_cli.c — CLI for FGLS Geometric Weight Storage Pipeline
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Build (Windows):
 *   gcc -O2 -std=c11 -I.. -I../.. -I../../collection -I../../collection/src \
 *       -I../../collection/core/pogls_engine/twin_core -I../../collection/core/pogls_engine \
 *       -I../../collection/core/pogls_engine/core -I../../collection/core/core \
 *       -I../../collection/rdh -I../../runner \
 *       -DFGLS_PIPELINE_IMPLEMENTATION \
 *       -o fgls_pipeline.exe fgls_pipeline_cli.c \
 *       ../../ext/fgls_pipeline.h ../../runner/dramtile_store.c \
 *       ../../collection/dgls/geo/src/geo_jump.c \
 *       ../../collection/src/lc_twin_gate.c \
 *       ../../collection/src/twin_gear_bridge.c -lm
 *
 * Usage:
 *   fgls_pipeline.exe encode [strategy]       # encode test data
 *   fgls_pipeline.exe decode [strategy]       # decode and verify
 *   fgls_pipeline.exe bench [iterations]      # run benchmark
 *   fgls_pipeline.exe stats                   # show pipeline stats
 *   fgls_pipeline.exe pull [output_file]      # GPU pull
 */

#define FGLS_PIPELINE_IMPLEMENTATION
#include "fgls_pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_usage(const char *prog) {
    printf("FGLS Geometric Weight Storage Pipeline\n");
    printf("Usage: %s <command> [args...]\n\n", prog);
    printf("Commands:\n");
    printf("  encode [strategy]     Encode test data (default: stride37)\n");
    printf("  decode [strategy]     Decode and verify (default: stride37)\n");
    printf("  bench [iterations]    Run benchmark (default: 100)\n");
    printf("  stats                 Show pipeline configuration\n");
    printf("  pull [output_file]    GPU pull (simulated)\n\n");
    printf("Strategies: sequential, stride37, face_region, grid\n");
    printf("Default strategy: stride37\n");
}

static fgls_strategy parse_strategy(const char *s) {
    if (!s) return FGLS_STRIDE37;
    if (strcmp(s, "sequential") == 0) return FGLS_SEQUENTIAL;
    if (strcmp(s, "stride37") == 0) return FGLS_STRIDE37;
    if (strcmp(s, "face_region") == 0) return FGLS_FACE_REGION;
    if (strcmp(s, "grid") == 0) return FGLS_GRID;
    return FGLS_STRIDE37;
}

static void generate_test_cells(fgls_cell *cells, int n, int seed) {
    srand((unsigned)seed);
    for (int i = 0; i < n; i++) {
        int face, x, y, z;
        int face_cells = FGLS_W * FGLS_H * FGLS_L;
        face = i / face_cells;
        int rem = i % face_cells;
        z = rem / (FGLS_W * FGLS_H);
        rem %= (FGLS_W * FGLS_H);
        y = rem / FGLS_W;
        x = rem % FGLS_W;
        cells[i].face = face;
        cells[i].x = x;
        cells[i].y = y;
        cells[i].z = z;
        cells[i].value = (int8_t)((i * 37 + 13 + seed) % 256 - 128);
        if (cells[i].value == 0) cells[i].value = 1;
        cells[i].global_idx = i;
    }
}

static int verify_cells(const fgls_cell *a, const fgls_cell *b, int n) {
    int mismatches = 0;
    for (int i = 0; i < n; i++) {
        if (a[i].value != b[i].value) {
            if (mismatches < 5) {
                printf("  Mismatch at %d: face=%d x=%d y=%d z=%d, got=%d expected=%d\n",
                       i, a[i].face, a[i].x, a[i].y, a[i].z, b[i].value, a[i].value);
            }
            mismatches++;
        }
    }
    return mismatches;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];
    fgls_strategy strat = (argc > 2) ? parse_strategy(argv[2]) : FGLS_STRIDE37;
    int iterations = (argc > 2) ? atoi(argv[2]) : 100;

    fgls_config cfg = fgls_default_config();
    cfg.strategy = strat;
    cfg.use_geojump = 1;

    fgls_ctx *ctx = fgls_init(&cfg);
    if (!ctx) {
        fprintf(stderr, "Failed to initialize pipeline\n");
        return 1;
    }

    printf("Initialized: %s strategy, GeoJump=%s\n",
           fgls_strategy_name(strat),
           cfg.use_geojump ? "on" : "off");

    if (strcmp(cmd, "encode") == 0) {
        fgls_cell cells[FGLS_CELLS];
        generate_test_cells(cells, FGLS_CELLS, 42);
        
        int collisions = fgls_encode(ctx, cells, FGLS_CELLS);
        printf("Encoded %d cells, collisions: %d\n", FGLS_CELLS, collisions);
        
        // Quick verify
        fgls_cell out[FGLS_CELLS];
        int n = fgls_decode(ctx, out, FGLS_CELLS);
        int mismatches = verify_cells(cells, out, n);
        printf("Decoded %d cells, mismatches: %d %s\n", n, mismatches, mismatches == 0 ? "✓" : "✗");
    }
    else if (strcmp(cmd, "decode") == 0) {
        fgls_cell cells[FGLS_CELLS];
        generate_test_cells(cells, FGLS_CELLS, 42);
        
        fgls_encode(ctx, cells, FGLS_CELLS);
        fgls_cell out[FGLS_CELLS];
        int n = fgls_decode(ctx, out, FGLS_CELLS);
        int mismatches = verify_cells(cells, out, n);
        printf("Decoded %d cells, mismatches: %d %s\n", n, mismatches, mismatches == 0 ? "✓" : "✗");
    }
    else if (strcmp(cmd, "bench") == 0) {
        printf("Running benchmark (%d iterations)...\n", iterations);
        double ns = fgls_benchmark(ctx, iterations);
        if (ns > 0) {
            printf("Average: %.2f ns/op (%.2f M cells/s)\n", ns, 1e9 / ns);
        } else {
            printf("Benchmark failed\n");
        }
    }
    else if (strcmp(cmd, "stats") == 0) {
        fgls_stats(ctx, stdout);
    }
    else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        print_usage(argv[0]);
        fgls_free(ctx);
        return 1;
    }

    fgls_free(ctx);
    return 0;
}
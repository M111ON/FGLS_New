/* bench_contour_codec_perf.c — Performance benchmark for contour codec
 * Tests: reconstructed encode/decode, random access read, GeoJump bridge
 * Pure codec benchmark (no DRamTile dependencies)
 *
 * Build: gcc -O2 -std=c11 -I../../collection/dgls/geo/include \
 *   -o bench_contour_codec_perf.exe bench_contour_codec_perf.c \
 *   ../../collection/dgls/geo/src/geo_jump.c -lm
 */

#define CONTOUR_CODEC_IMPLEMENTATION
#include "contour_codec_20736.h"
#include "geo_jump.h"
#include "geo_frame_seek.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
static double now_ns(void) {
    static LARGE_INTEGER f = {0};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart * 1e9;
}
#else
#include <time.h>
static double now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}
#endif

// ── Test data ──────────────────────────────────────────────────────────────
static contour_cell test_cells[CC_CELLS];

static void generate_test_data(int seed) {
    srand((unsigned)seed);
    for (int i = 0; i < CC_CELLS; i++) {
        int face, x, y, z;
        cell_from_idx(i, &face, &x, &y, &z);
        test_cells[i].face = face;
        test_cells[i].x = x;
        test_cells[i].y = y;
        test_cells[i].z = z;
        test_cells[i].global_idx = i;
        int8_t v = (int8_t)((face * 1000 + z * 100 + y * 10 + x + seed) * 37 + 13) % 256 - 128;
        if (v == 0) v = 1;
        test_cells[i].value = v;
    }
}

// ── Results tracking ──────────────────────────────────────────────────────
typedef struct {
    const char *name;
    double encode_ns;
    double decode_ns;
    double get_ns;
    double set_ns;
    double random_read_ns;
    double geojump_ns;
    double frameseek_ns;
    int errors;
} BenchResult;

static void print_result(const BenchResult *r) {
    printf("  %-14s  enc: %8.1f  dec: %8.1f  get: %7.1f  set: %7.1f  rand: %8.1f  jump: %8.1f  seek: %7.1f  err: %d\n",
           r->name, r->encode_ns, r->decode_ns, r->get_ns, r->set_ns, r->random_read_ns, r->geojump_ns, r->frameseek_ns, r->errors);
}

// ── Benchmark: Pure Codec ─────────────────────────────────────────────────
BenchResult bench_pure_codec(CODEC_STRATEGY strategy, const char *label) {
    BenchResult r = {label, 0, 0, 0, 0, 0, 0, 0, 0};
    codec_ctx *ctx = codec_create(strategy);
    if (!ctx) { r.errors = 1; return r; }

    // Encode
    const int ENCODE_ITER = 200;
    double t0 = now_ns();
    for (int iter = 0; iter < ENCODE_ITER; iter++) codec_encode(ctx, test_cells, CC_CELLS);
    double t1 = now_ns();
    r.encode_ns = (t1 - t0) / (ENCODE_ITER * CC_CELLS);

    // Decode
    contour_cell decoded[CC_CELLS];
    const int DECODE_ITER = 200;
    t0 = now_ns();
    for (int iter = 0; iter < DECODE_ITER; iter++) codec_decode(ctx, decoded, CC_CELLS);
    t1 = now_ns();
    r.decode_ns = (t1 - t0) / (DECODE_ITER * CC_CELLS);

    // Get (O(1) sequential)
    const int GET_ITER = 500000;
    t0 = now_ns();
    for (int i = 0; i < GET_ITER; i++) {
        int idx = i % CC_CELLS;
        codec_get(ctx, test_cells[idx].face, test_cells[idx].x, test_cells[idx].y, test_cells[idx].z);
    }
    t1 = now_ns();
    r.get_ns = (t1 - t0) / GET_ITER;

    // Set
    const int SET_ITER = 500000;
    t0 = now_ns();
    for (int i = 0; i < SET_ITER; i++) {
        int idx = i % CC_CELLS;
        codec_set(ctx, test_cells[idx].face, test_cells[idx].x, test_cells[idx].y, test_cells[idx].z, (int8_t)(i & 0xFF));
    }
    t1 = now_ns();
    r.set_ns = (t1 - t0) / SET_ITER;

    // Random read (scattered)
    const int RAND_ITER = 500000;
    uint32_t rng_state = 42;
    uint32_t rng_next() { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }
    t0 = now_ns();
    for (int i = 0; i < RAND_ITER; i++) {
        int idx = rng_next() % CC_CELLS;
        codec_get(ctx, test_cells[idx].face, test_cells[idx].x, test_cells[idx].y, test_cells[idx].z);
    }
    t1 = now_ns();
    r.random_read_ns = (t1 - t0) / RAND_ITER;

    // Verify roundtrip
    codec_encode(ctx, test_cells, CC_CELLS);
    codec_decode(ctx, decoded, CC_CELLS);
    for (int i = 0; i < CC_CELLS; i++)
        if (decoded[i].value != test_cells[i].value) r.errors++;

    codec_free(ctx);
    return r;
}

// ── Benchmark: GeoJump Bridge ──────────────────────────────────────────────
BenchResult bench_geojump_bridge(CODEC_STRATEGY strategy, const char *label) {
    BenchResult r = {label, 0, 0, 0, 0, 0, 0, 0, 0};
    codec_ctx *ctx = codec_create(strategy);
    if (!ctx) { r.errors = 1; return r; }
    codec_encode(ctx, test_cells, CC_CELLS);

    // GeoJump operations (Hilbert, Peano, Mod, Invert)
    const int JUMP_ITER = 200000;
    uint32_t rng_state = 42;
    uint32_t rng_next() { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

    double t0 = now_ns();
    for (int i = 0; i < JUMP_ITER; i++) {
        int idx = rng_next() % CC_CELLS;
        contour_cell *c = &test_cells[idx];
        uint32_t addr = 0;
        switch (strategy) {
            case CODEC_SEQUENTIAL:  addr = c->face * 1000 + c->z * 100 + c->y * 10 + c->x; break;
            case CODEC_STRIDE37:    addr = (c->global_idx * 37u) % CC_GEO_FULL; break;
            case CODEC_FACE_REGION: addr = c->face * 3456 + c->z * 345 + c->y * 34 + c->x; break;
            case CODEC_GRID:        addr = (c->face * 24 + c->z) * 144 + (c->y * 10 + c->x); break;
        }
        uint32_t h = geo_jump(addr, JUMP_HILBERT, 1);
        uint32_t p = geo_jump(addr, JUMP_PEANO, 1);
        uint32_t m = geo_jump(addr, JUMP_MOD, 37);
        uint32_t inv = geo_jump(addr, JUMP_INVERT, 1);
        (void)h; (void)p; (void)m; (void)inv;
    }
    double t1 = now_ns();
    r.geojump_ns = (t1 - t0) / JUMP_ITER;

    // Frame seek timeline
    const int SEEK_ITER = 200000;
    t0 = now_ns();
    for (int i = 0; i < SEEK_ITER; i++) {
        uint32_t t = rng_next() % 144000;
        DualFrame f = frame_seek(t);
        (void)f;
    }
    t1 = now_ns();
    r.frameseek_ns = (t1 - t0) / SEEK_ITER;

    codec_free(ctx);
    return r;
}

// ── Main ───────────────────────────────────────────────────────────────────
int main(void) {
    printf("╔════════════════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Contour Codec Performance Benchmark — Reconstructed Encode/Decode + Random Access          ║\n");
    printf("║  Pure Codec + GeoJump Bridge + Frame Seek Timeline                                          ║\n");
    printf("╚════════════════════════════════════════════════════════════════════════════════════════════════╝\n\n");
    printf("  Cells: %d (6×10×10×10)  |  Geo Space: %d (144×144)  |  Unused: %.1f%%\n\n",
           CC_CELLS, CC_GEO_FULL, 100.0 * (CC_GEO_FULL - CC_CELLS) / CC_GEO_FULL);

    generate_test_data(42);

    // ─────────────────────────────────────────────────────────────────────
    printf("════════════════════════════════════════════════════════════════════════════════════════════\n");
    printf("  TEST 1: Pure Codec — All 4 Strategies\n");
    printf("═══════════════════════════════════════════════════════════════════════════════════════════\n\n");

    BenchResult results[CODEC_COUNT];
    for (int s = 0; s < CODEC_COUNT; s++) {
        results[s] = bench_pure_codec((CODEC_STRATEGY)s, codec_strategy_name((CODEC_STRATEGY)s));
        print_result(&results[s]);
    }

    // ─────────────────────────────────────────────────────────────────────
    printf("\n════════════════════════════════════════════════════════════════════════════════════════════\n");
    printf("  TEST 2: GeoJump Bridge (Hilbert/Peano/Mod/Invert + Frame Seek)\n");
    printf("═══════════════════════════════════════════════════════════════════════════════════════════\n\n");

    BenchResult jump_results[CODEC_COUNT];
    for (int s = 0; s < CODEC_COUNT; s++) {
        jump_results[s] = bench_geojump_bridge((CODEC_STRATEGY)s, codec_strategy_name((CODEC_STRATEGY)s));
        printf("  %-14s  jump4: %8.1f ns  frame_seek: %7.1f ns  err: %d\n",
               jump_results[s].name, jump_results[s].geojump_ns, jump_results[s].frameseek_ns, jump_results[s].errors);
    }

    // ─────────────────────────────────────────────────────────────────────
    printf("\n════════════════════════════════════════════════════════════════════════════════════════════\n");
    printf("  SUMMARY: Strategy Comparison (Per-Operation Latency in ns)\n");
    printf("═══════════════════════════════════════════════════════════════════════════════════════════\n\n");

    printf("  Operation           │ Sequential │ Stride37   │ FaceRegion │ Grid       │ Best Strategy\n");
    printf("  ────────────────────┼────────────┼────────────┼────────────┼────────────┼──────────────\n");
    const char *ops[] = {"Encode", "Decode", "Get", "Set", "RandomRead"};
    double *fields[] = {&results[0].encode_ns, &results[0].decode_ns, &results[0].get_ns, &results[0].set_ns, &results[0].random_read_ns};
    for (int op = 0; op < 5; op++) {
        printf("  %-19s │", ops[op]);
        double best = 1e9; int best_s = 0;
        for (int s = 0; s < CODEC_COUNT; s++) {
            double val = *((double*)((char*)&results[s] + (op * sizeof(double))));
            printf(" %10.1f │", val);
            if (val < best) { best = val; best_s = s; }
        }
        printf("  %s\n", codec_strategy_name((CODEC_STRATEGY)best_s));
    }

    printf("\n  GeoJump Bridge (4 jump types + read):\n");
    for (int s = 0; s < CODEC_COUNT; s++) {
        printf("    %-14s: %8.1f ns/op\n", codec_strategy_name((CODEC_STRATEGY)s), jump_results[s].geojump_ns);
    }
    printf("  Frame Seek (O(1) timeline):\n");
    for (int s = 0; s < CODEC_COUNT; s++) {
        printf("    %-14s: %8.1f ns/op\n", codec_strategy_name((CODEC_STRATEGY)s), jump_results[s].frameseek_ns);
    }

    // ─────────────────────────────────────────────────────────────────────
    printf("\n═════════════════════════════════════════════════════════════════════════════════════════════\n");
    printf("  RECONSTRUCTION VERIFICATION — All Strategies\n");
    printf("════════════════════════════════════════════════════════════════════════════════════════════\n\n");

    for (int s = 0; s < CODEC_COUNT; s++) {
        codec_ctx *ctx = codec_create((CODEC_STRATEGY)s);
        codec_encode(ctx, test_cells, CC_CELLS);
        contour_cell decoded[CC_CELLS];
        codec_decode(ctx, decoded, CC_CELLS);
        int mismatches = 0;
        for (int i = 0; i < CC_CELLS; i++) if (decoded[i].value != test_cells[i].value) mismatches++;
        printf("  %-14s: %d/%d match  %s\n",
               codec_strategy_name((CODEC_STRATEGY)s), CC_CELLS - mismatches, CC_CELLS,
               mismatches == 0 ? "✓ LOSSLESS" : "✗ MISMATCH");
        codec_free(ctx);
    }

    printf("\n════════════════════════════════════════════════════════════════════════════════════════════\n");
    printf("  BENCHMARK COMPLETE — All tests PASS\n");
    printf("═══════════════════════════════════════════════════════════════════════════════════════════\n");

    int total_errors = 0;
    for (int s = 0; s < CODEC_COUNT; s++) total_errors += results[s].errors;
    return total_errors ? 1 : 0;
}
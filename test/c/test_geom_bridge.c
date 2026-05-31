/*
 * test_geom_bridge.c — GeomBridge end-to-end test
 *
 * Pipeline:
 *   build_geom_tile_store.py → .gsten files
 *   → gb_load() → gb_decode_tile() / gb_decode_tensor()
 *   → verify lossless vs original .qdat
 *
 * Compile:
 *   cd collection
 *   python build_geom_tile_store.py ...
 *   gcc -O2 -I. -DGEOM_RAW_BRIDGE_IMPLEMENTATION -D__USE_MINGW_ANSI_STDIO
 *       -o test_geom_bridge tests/test_geom_bridge.c
 *
 * Run:
 *   ./test_geom_bridge <gsten_dir> <qdat_dir>
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#include "geom_raw_bridge.h"
#include "hex_tile.h"

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); _fail++; } \
} while (0)

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    const char *gsten_dir = argc > 1 ? argv[1] : "build/qwen25_gsten";
    const char *qdat_dir  = argc > 2 ? argv[2] : "build/qwen25_tensors_raw";

    printf("=== GeomBridge End-to-End Test ===\n");
    printf("  Gsten dir: %s\n", gsten_dir);
    printf("  Qdat  dir: %s\n\n", qdat_dir);

    /* ── Load .gsten files ── */
    GeomBridge gb;
    int ret = gb_load(&gb, gsten_dir);
    CHECK(ret == RB_OK, "gb_load: loaded .gsten directory");
    printf("  Loaded %u .gsten files\n\n", gb.n_entries);

    if (gb.n_entries == 0) {
        printf("  No .gsten files found. Run first:\n");
        printf("    python build_geom_tile_store.py --tensor-dir %s --out-dir build --model-name test\n", qdat_dir);
        return 1;
    }

    /* ── Load .qdat for reference ── */
    RawBridge rb;
    ret = rb_load(&rb, qdat_dir);
    CHECK(ret == RB_OK, "rb_load: loaded .qdat directory");
    printf("  Loaded %u .qdat files\n\n", rb.n_entries);

    /* ── Test each tensor ── */
    int n_tested = 0;
    double total_enc_ms = 0, total_dec_ms = 0;
    size_t total_orig = 0, total_enc_sz = 0;

    for (uint32_t i = 0; i < RB_MAX_ENTRIES && n_tested < 20; i++) {
        if (!gb.entries[i].occupied) continue;

        const char *name = gb.entries[i].name;
        GstenEntry *ge = &gb.entries[i];

        /* Lookup original .qdat */
        void *qdat_data;
        size_t qdat_sz;
        ret = rb_get(&rb, name, &qdat_data, &qdat_sz);
        if (ret != RB_OK) {
            printf("  SKIP  %s (no .qdat)\n", name);
            continue;
        }

        n_tested++;
        total_orig += qdat_sz;
        total_enc_sz += ge->store_size;

        /* ── Verify single tile random access ── */
        int n_tiles_ok = 0;
        for (uint32_t ti = 0; ti < ge->n_tiles; ti++) {
            uint8_t dec_tile[7];
            if (gb_decode_tile(ge, ti, dec_tile) != RB_OK) continue;

            /* Compare with original */
            size_t orig_off = (size_t)ti * GSTEN_TILE_SZ;
            int cmp_n = GSTEN_TILE_SZ;
            if (orig_off + cmp_n > qdat_sz)
                cmp_n = (int)(qdat_sz - orig_off);

            if (memcmp(dec_tile, (uint8_t *)qdat_data + orig_off, cmp_n) == 0)
                n_tiles_ok++;
        }

        /* ── Full tensor decode bench ── */
        size_t dec_sz = (size_t)ge->n_tiles * GSTEN_TILE_SZ;
        uint8_t *dec_full = (uint8_t *)malloc(dec_sz);

        double t0 = now_ms();
        ret = gb_decode_tensor(ge, dec_full, dec_sz);
        double t_dec = now_ms() - t0;
        total_dec_ms += t_dec;

        /* Verify full tensor */
        int full_ok = 0;
        if (ret == RB_OK) {
            size_t cmp_sz = dec_sz < qdat_sz ? dec_sz : qdat_sz;
            full_ok = (memcmp(dec_full, qdat_data, cmp_sz) == 0);
        }

        printf("  %-40s  tiles=%6u  "
               "orig=%5.0fKB  enc=%5.0fKB  ratio=%5.3f  "
               "dec=%5.1fms  tile_ok=%d/%u  full=%s\n",
               name, ge->n_tiles,
               qdat_sz / 1024.0, ge->store_size / 1024.0,
               (double)ge->store_size / qdat_sz,
               t_dec, n_tiles_ok, ge->n_tiles,
               full_ok ? "OK" : "FAIL");

        if (!full_ok) _fail++;

        free(dec_full);
    }

    /* ── Summary ── */
    printf("\n────────────────────────────────────────────────\n");
    printf("  Tested:    %d tensors\n", n_tested);
    printf("  Original:  %.1f KB\n", total_orig / 1024.0);
    printf("  Encoded:   %.1f KB\n", total_enc_sz / 1024.0);
    printf("  Ratio:     %.3f\n", (double)total_enc_sz / total_orig);
    printf("  Decode:    %.2f ms total (%.1f µs/byte)\n",
           total_dec_ms, total_dec_ms * 1000.0 / total_orig);
    printf("\n  Results: %d pass, %d fail\n", _pass, _fail);

    gb_free(&gb);
    rb_free(&rb);
    return _fail;
}

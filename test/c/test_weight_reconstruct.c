/*
 * test_weight_reconstruct.c — Weight reconstruction: geometry → float32
 *
 * Compile:
 *   gcc -O2 -I.. -o test_weight_reconstruct test_weight_reconstruct.c
 *
 * Run:
 *   ./test_weight_reconstruct <gsten_dir> <qdat_dir> [tensor_name]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <windows.h>

#ifndef GEOM_RAW_BRIDGE_IMPLEMENTATION
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#endif
#include "geom_raw_bridge.h"
#include "geom_weight_reconstruct.h"

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); _fail++; } \
} while(0)

static double now_ms(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart * 1000.0 / (double)freq.QuadPart;
}

/* ── Reference Q8_0 dequant (same algorithm, independent impl) ── */
static float _ref_fp16_to_fp32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp  = (uint32_t)((h >> 10) & 0x1F);
    uint32_t mant = (uint32_t)(h & 0x3FF);
    if (exp == 0) {
        if (mant == 0) { uint32_t v = sign; float f; memcpy(&f, &v, 4); return f; }
        int shift = 10;
        while ((mant & 0x400) == 0) { mant <<= 1; shift--; }
        exp = 1 - shift + 127 - 10;
        mant = (mant & 0x7FF) << 13;
        uint32_t v = sign | (exp << 23) | mant; float f; memcpy(&f, &v, 4); return f;
    }
    if (exp == 31) { exp = 255; mant <<= 13; uint32_t v = sign | (exp << 23) | mant; float f; memcpy(&f, &v, 4); return f; }
    exp = exp - 15 + 127; mant <<= 13;
    uint32_t v = sign | (exp << 23) | mant; float f; memcpy(&f, &v, 4); return f;
}

static void _ref_q8_bulk(const uint8_t *raw, size_t sz, float *out) {
    size_t n = sz / 34;
    for (size_t bi = 0; bi < n; bi++) {
        uint16_t sh; memcpy(&sh, raw + bi * 34, 2);
        float s = _ref_fp16_to_fp32(sh);
        const int8_t *qs = (const int8_t *)(raw + bi * 34 + 2);
        for (int i = 0; i < 32; i++) out[bi * 32 + i] = (float)qs[i] * s;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: test_weight_reconstruct <gsten_dir> <qdat_dir> [tensor]\n");
        return 1;
    }
    const char *gsten_dir = argv[1];
    const char *qdat_dir  = argv[2];
    const char *tname     = argc > 3 ? argv[3] : "blk.0.ffn_down.weight";

    printf("=== Weight Reconstruction: Geometry → Float32 ===\n");
    printf("  Tensor: %s\n\n", tname);

    /* Load .gsten */
    GeomBridge gb;
    CHECK(gb_load(&gb, gsten_dir) == RB_OK, "gb_load OK");
    GstenEntry *ge = NULL;
    CHECK(gb_get(&gb, tname, &ge) == RB_OK && ge, "gb_get OK");

    /* Load .qdat */
    RawBridge rb;
    CHECK(rb_load(&rb, qdat_dir) == RB_OK, "rb_load OK");
    void *qdat_data; size_t qdat_sz;
    CHECK(rb_get(&rb, tname, &qdat_data, &qdat_sz) == RB_OK, "rb_get OK");
    printf("  Q8_0: %zu bytes  (%zu KB)\n\n", qdat_sz, qdat_sz / 1024);

    /* ── Bulk test ── */
    printf("─── Bulk dequant: gwr_q8_bulk ───\n");

    size_t n_blocks = qdat_sz / 34;
    size_t n_floats = n_blocks * 32;

    /* Reference dequant */
    double t0 = now_ms();
    float *ref_f32 = (float *)malloc(n_floats * sizeof(float));
    _ref_q8_bulk((const uint8_t *)qdat_data, qdat_sz, ref_f32);
    double t_ref = now_ms() - t0;

    /* Geometry decode + dequant */
    t0 = now_ms();
    size_t dec_sz = (size_t)ge->n_tiles * GSTEN_TILE_SZ;
    uint8_t *dec_raw = (uint8_t *)malloc(dec_sz);
    CHECK(gb_decode_tensor(ge, dec_raw, dec_sz) == RB_OK, "gb_decode_tensor OK");

    float *geom_f32 = (float *)malloc(n_floats * sizeof(float));
    int n_blocks_dec = gwr_q8_bulk(dec_raw, dec_sz, geom_f32);
    double t_geom = now_ms() - t0;

    CHECK(n_blocks_dec == (int)n_blocks, "bulk: block count matches");
    printf("  Blocks: %zu  Values: %zu\n", n_blocks, n_floats);
    printf("  Q8_0→f32:  %.3f ms  (ref)\n", t_ref);
    printf("  Geo→f32:  %.3f ms  (decode + dequant)\n", t_geom);

    /* Verify float values */
    uint64_t n_match = 0, n_mismatch = 0;
    double max_err = 0.0;
    for (size_t i = 0; i < n_floats; i++) {
        double err = fabs((double)ref_f32[i] - (double)geom_f32[i]);
        if (err > max_err) max_err = err;
        if (err < 1e-6) n_match++;
        else n_mismatch++;
    }

    printf("  Float match: %llu / %zu  (%.2f%%)\n",
           (unsigned long long)n_match, n_floats,
           100.0 * n_match / n_floats);
    printf("  Max error: %e\n", max_err);
    CHECK(n_match == n_floats, "bulk: all float values match");

    /* Spot-check first 5 values */
    printf("\n  First 5 floats (ref vs geo):\n");
    for (int i = 0; i < 5 && i < (int)n_floats; i++)
        printf("    [%d]  ref=%+.6f  geo=%+.6f  err=%e\n",
               i, ref_f32[i], geom_f32[i], fabs(ref_f32[i] - geom_f32[i]));

    /* ── Random access test ── */
    printf("\n─── Random access: gwr_q8_block ───\n");

    uint32_t n_blocks_u32 = (uint32_t)n_blocks;
    uint32_t sample = n_blocks_u32 < 100 ? n_blocks_u32 : 100;
    uint32_t n_rnd_ok = 0;

    t0 = now_ms();
    for (uint32_t bi = 0; bi < sample; bi++) {
        float block_f32[32];
        if (gwr_q8_block(ge, bi, block_f32) != RB_OK) continue;

        int ok = 1;
        for (int i = 0; i < 32; i++) {
            if (fabs(block_f32[i] - ref_f32[bi * 32 + i]) >= 1e-6f) {
                ok = 0; break;
            }
        }
        if (ok) n_rnd_ok++;
    }
    double t_rnd = now_ms() - t0;

    printf("  Random access (sample %u blocks): %u / %u match\n",
           sample, n_rnd_ok, sample);
    printf("  Time: %.3f ms  (%.1f ns/block)\n",
           t_rnd, t_rnd * 1e6 / sample);
    CHECK(n_rnd_ok == sample, "random: all sampled blocks match ref");

    /* ── Summary ── */
    printf("\n──────────────────────────────\n");
    size_t geom_sz = ge->store_size;
    printf("  Q8_0:    %zu KB\n", qdat_sz / 1024);
    printf("  .gsten:  %zu KB  (ratio %.3f)\n", geom_sz / 1024,
           (double)geom_sz / qdat_sz);
    printf("  Float:   %zu KB  (%.1f× Q8_0)\n",
           n_floats * 4 / 1024, (float)(n_floats * 4) / qdat_sz);
    printf("  Dequant speed: %.0f MB/s (bulk)\n",
           (n_floats * 4.0) / (t_geom / 1000.0) / 1e6);
    printf("\n  Results: %d pass, %d fail\n", _pass, _fail);

    free(ref_f32); free(dec_raw); free(geom_f32);
    gb_free(&gb); rb_free(&rb);
    return _fail;
}

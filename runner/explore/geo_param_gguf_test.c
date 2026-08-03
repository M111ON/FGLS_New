/* ═══════════════════════════════════════════════════════════════════════════
 * geo_param_gguf_test.c — geo_param_grid on real GGUF (lossless roundtrip)
 * ═══════════════════════════════════════════════════════════════════════════
 * Compile:
 *   gcc -O2 -std=c11 -Wall -Wextra -I. -Irunner/explore \
 *       -o runner/explore/geo_param_gguf_test.exe \
 *       runner/explore/geo_param_gguf_test.c -lm
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "core/geo_param_grid.h"
#include "gguf_reader.h"

/* pick smallest geometry whose capacity (verts×256) covers distinct count */
static GeoType pick_geo(uint32_t distinct)
{
    static const GeoType order[] = {
        GEO_DODEC_BASE, GEO_ICO_BASE, GEO_COMPOUND_24, GEO_DODEC_EDGES,
        GEO_COMPOUND_60, GEO_PENTAKIS_72, GEO_GOLDBERG_92, GEO_COMP_SPIKE_120,
        GEO_GOLDBERG_132, GEO_COMPOUND_144, GEO_GOLDBERG_192
    };
    for (size_t i = 0; i < sizeof(order)/sizeof(order[0]); i++) {
        GeoProps pr = geo_props(order[i]);
        if ((uint64_t)pr.verts * pr.slot_cap >= distinct)
            return order[i];
    }
    return GEO_GOLDBERG_192;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("Usage: %s <model.gguf> [max_weights]\n", argv[0]); return 1; }
    uint64_t max_w = (argc >= 3) ? strtoull(argv[2], NULL, 10) : 1000000ULL;

    printf("===============================================================\n");
    printf("  GEO Parametric Grid — Real GGUF Roundtrip\n");
    printf("===============================================================\n");
    GGUF_File *gf = gguf_open(argv[1]);
    if (!gf) { printf("ERROR: cannot open %s\n", argv[1]); return 1; }
    printf("  GGUF v%u, %llu tensors\n", gf->version,
           (unsigned long long)gf->tensor_count);

    /* dequantize Q8_0 blocks into float weights */
    float *weights = NULL;
    uint64_t n_w = 0;
    for (uint64_t ti = 0; ti < gf->tensor_count && n_w < max_w; ti++) {
        GGUF_Tensor *t = &gf->tensors[ti];
        if (t->n_weights == 0 || t->type != GGML_TYPE_Q8_0) continue;
        uint64_t need = t->n_weights;
        if (n_w + need > max_w) need = max_w - n_w;
        weights = (float*)realloc(weights, (n_w + need) * sizeof(float));
        if (!weights) { printf("malloc fail\n"); gguf_close(gf); return 1; }

        fseek(gf->fp, gf->tensor_data_start + t->offset, SEEK_SET);
        uint64_t pos = 0;
        while (pos < need) {
            uint8_t blk[34];
            if (fread(blk, 1, 34, gf->fp) != 34) break;
            /* Q8_0 block = [u16 d][int8 ×32].
               Repo convention (see kis_real_gguf_test.c): scale is
               15-bit fixed point /1024, sign in bit 15. */
            uint16_t u = (uint16_t)(blk[0] | (blk[1] << 8));
            float d = (float)(u & 0x7FFF) / 1024.0f;
            if (u & 0x8000) d = -d;
            int8_t *qs = (int8_t*)(blk + 2);        /* 32 int8 weights */
            uint32_t take = (need - pos < 32) ? (uint32_t)(need - pos) : 32;
            for (uint32_t k = 0; k < take; k++)
                weights[n_w + pos + k] = d * qs[k];
            pos += take;
        }
        n_w += pos;
        printf("  tensor %llu: %s loaded (%I64u total so far)\n",
               (unsigned long long)ti, t->name, n_w);
    }
    gguf_close(gf);
    printf("  Total weights: %I64u\n", n_w);
    if (n_w == 0) { free(weights); return 1; }

    /* run codec per window, auto geometry */
    const uint32_t WIN = 300000;
    uint64_t off = 0;
    uint32_t wins = 0, fails = 0;
    double ratio_sum = 0, ratio_min = 1e9, ratio_max = 0;
    clock_t t0 = clock();

    while (off < n_w) {
        uint32_t win = (uint32_t)((n_w - off < WIN) ? (n_w - off) : WIN);
        GeoCodec gc;
        if (geo_codec_init(&gc, GEO_AUTO, &weights[off], win) != 0) { printf("init fail\n"); break; }
        /* auto-select type by distinct count */
        if (gc.type == GEO_AUTO) {
            /* re-init with picked geometry */
            GeoType pick = pick_geo(gc.n_uniq);
            geo_codec_free(&gc);
            if (geo_codec_init(&gc, pick, &weights[off], win) != 0) { printf("re-init fail\n"); break; }
        }
        if (geo_codec_verify(&gc) != 0) {
            printf("  [win %u] ROUNDTRIP FAILED\n", wins);
            fails++;
        }
        ratio_sum += gc.ratio;
        if (gc.ratio < ratio_min) ratio_min = gc.ratio;
        if (gc.ratio > ratio_max) ratio_max = gc.ratio;

        if (wins % 10 == 0 || wins == 0)
            geo_codec_stats(&gc);

        geo_codec_free(&gc);
        off += win; wins++;
    }
    double el = (double)(clock()-t0)/CLOCKS_PER_SEC;

    printf("\n===============================================================\n");
    printf("  RESULTS\n");
    printf("===============================================================\n");
    printf("  Windows: %u, weights: %I64u\n", wins, n_w);
    if (fails) printf("  ❌ ROUNDTRIP FAILED (%u)\n", fails);
    else       printf("  ✅ LOSSLESS ROUNDTRIP (%u windows)\n", wins);
    printf("  Avg ratio: %.3fx (min %.2f, max %.2f)\n",
           wins ? ratio_sum/wins : 0, ratio_min, ratio_max);
    printf("  Time: %.3f s\n", el);
    printf("===============================================================\n");
    free(weights);
    return fails ? 1 : 0;
}
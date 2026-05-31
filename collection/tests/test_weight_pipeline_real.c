/*
 * test_weight_pipeline_real.c — Full pipeline test with real Q8_0 weight
 *
 * Loads a .gsten tensor + matching .qdat, splits into 64B chunks,
 * runs gsp_push() on each, verifies HOT→tile lossless, COLD→ring.
 *
 * Compile:
 *   gcc -O2 -I.. -o test_weight_pipeline_real test_weight_pipeline_real.c
 *
 * Run:
 *   ./test_weight_pipeline_real <gsten_dir> <qdat_dir> [tensor_name]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifndef GEOM_RAW_BRIDGE_IMPLEMENTATION
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#endif
#include "geom_raw_bridge.h"

#ifndef GEOM_ROUTER_BRIDGE_IMPLEMENTATION
#define GEOM_ROUTER_BRIDGE_IMPLEMENTATION
#endif
#include "geom_router_bridge.h"

#ifndef GEOM_SHADOW_PIPE_IMPLEMENTATION
#define GEOM_SHADOW_PIPE_IMPLEMENTATION
#endif
#include "geom_shadow_pipe.h"

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); _fail++; } \
} while(0)

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: test_weight_pipeline_real <gsten_dir> <qdat_dir> [tensor_name]\n");
        return 1;
    }

    const char *gsten_dir = argv[1];
    const char *qdat_dir  = argv[2];
    const char *tensor_name = argc > 3 ? argv[3] : "blk.0.attn_k.weight";

    printf("=== GeomShadowPipe: Real Weight Pipeline ===\n");
    printf("  Gsten: %s\n", gsten_dir);
    printf("  Qdat:  %s\n", qdat_dir);
    printf("  Tensor: %s\n\n", tensor_name);

    /* ── Load .gsten ── */
    GeomBridge gb;
    int ret = gb_load(&gb, gsten_dir);
    CHECK(ret == RB_OK, "gb_load OK");
    printf("  Loaded %u .gsten files\n\n", gb.n_entries);

    /* ── Find tensor ── */
    GstenEntry *ge = NULL;
    ret = gb_get(&gb, tensor_name, &ge);
    CHECK(ret == RB_OK && ge != NULL, "gb_get: found tensor");
    if (!ge) { gb_free(&gb); return 1; }
    printf("  Tensor: %s  tiles=%u  tile_sz=%u  store=%zuB\n\n",
           ge->name, ge->n_tiles, ge->tile_sz, ge->store_size);

    /* ── Load matching .qdat ── */
    RawBridge rb;
    ret = rb_load(&rb, qdat_dir);
    CHECK(ret == RB_OK, "rb_load OK");

    void *qdat_data;
    size_t qdat_sz;
    ret = rb_get(&rb, tensor_name, &qdat_data, &qdat_sz);
    CHECK(ret == RB_OK, "rb_get: found .qdat");
    if (ret != RB_OK) { gb_free(&gb); rb_free(&rb); return 1; }
    printf("  Q8_0 size: %zu bytes  (%zu KB)\n\n", qdat_sz, qdat_sz / 1024);

    /* ── Build GeomRouterBridge ── */
    GeomRouterBridge grb;
    ret = grb_build(&grb, &gb);
    CHECK(ret == GRB_OK, "grb_build OK");

    /* ── Init shadow ring + GspCtx ── */
    BermudaShadowRing ring;
    bermuda_shadow_ring_init(&ring);
    GspCtx ctx;
    gsp_init(&ctx, &grb, &ring);

    /* ── Split qdat into 64B chunks, run gsp_push ── */
    uint32_t n_chunks = (uint32_t)((qdat_sz + BERMUDA_CHUNK - 1) / BERMUDA_CHUNK);
    printf("Chunks: %u  (each %uB)\n\n", n_chunks, BERMUDA_CHUNK);

    /* We'll process in mini-batches to avoid huge stack */
    uint32_t batch_sz = 256;
    uint8_t  *chunks = (uint8_t *)malloc((size_t)batch_sz * BERMUDA_CHUNK);
    uint16_t *idxs   = (uint16_t *)malloc((size_t)batch_sz * sizeof(uint16_t));
    uint64_t *keys   = (uint64_t *)malloc((size_t)batch_sz * sizeof(uint64_t));
    GspResult *results = (GspResult *)malloc((size_t)batch_sz * sizeof(GspResult));
    CHECK(chunks && idxs && keys && results, "batch buffers allocated");

    uint32_t n_hot = 0, n_cold = 0, n_lossless = 0, n_mismatch = 0;

    double t0 = now_ms();

    for (uint32_t ci = 0; ci < n_chunks; ci += batch_sz) {
        uint32_t n_this = (ci + batch_sz <= n_chunks) ? batch_sz : (n_chunks - ci);

        /* Pack chunks */
        for (uint32_t i = 0; i < n_this; i++) {
            uint32_t ch = ci + i;
            size_t off = (size_t)ch * BERMUDA_CHUNK;
            size_t copy = BERMUDA_CHUNK;
            if (off + copy > qdat_sz) copy = qdat_sz - off;
            memset(chunks + (size_t)i * BERMUDA_CHUNK, 0, BERMUDA_CHUNK);
            if (copy > 0)
                memcpy(chunks + (size_t)i * BERMUDA_CHUNK,
                       (uint8_t *)qdat_data + off, copy);
            idxs[i] = (uint16_t)(ch % 65536);
            keys[i] = (uint64_t)ch;
        }

        /* Dispatch */
        uint32_t n_hot_batch = gsp_push_batch(&ctx, chunks, idxs, NULL, keys,
                                              2, 0, results, n_this);
        n_hot += n_hot_batch;
        n_cold += n_this - n_hot_batch;

        /* Verify HOT chunks: decoded tile matches original */
        for (uint32_t i = 0; i < n_this; i++) {
            if (!results[i].valid) continue;

            /* Map tring_slot → tile_idx → byte offset */
            uint32_t tile_idx = results[i].decode.tile_idx;
            size_t orig_off = (size_t)tile_idx * GSTEN_TILE_SZ;

            int match = 1;
            int cmp_n = GSTEN_TILE_SZ;
            if (orig_off + cmp_n > qdat_sz)
                cmp_n = (int)(qdat_sz - orig_off);

            if (memcmp(results[i].tile, (uint8_t *)qdat_data + orig_off, cmp_n) != 0)
                match = 0;

            if (match) n_lossless++;
            else       n_mismatch++;
        }
    }

    double t_total = now_ms() - t0;

    /* ── Results ── */
    printf("─── Pipeline Results ───\n");
    printf("  Total chunks: %u\n", n_chunks);
    printf("  HOT:  %u  (%.1f%%)\n", n_hot, 100.0 * n_hot / n_chunks);
    printf("  COLD: %u  (%.1f%%)\n", n_cold, 100.0 * n_cold / n_chunks);
    printf("  Lossless (HOT→tile match): %u / %u\n", n_lossless, n_hot);
    printf("  Mismatch: %u\n", n_mismatch);
    printf("  Time: %.2f ms  (%.1f chunks/s)\n", t_total, n_chunks / (t_total / 1000.0));

    /* Shadow ring stats */
    GspStats s = gsp_stats(&ctx);
    printf("  Ring: %u/%u  evictions=%u\n", s.ring_count, BERMUDA_SHADOW_RING, s.evictions);

    /* ── Direct verify: gb_decode_tile for every tile ── */
    /* (bypass router + shadow — direct geometry decode) */
    printf("\n─── Direct gb_decode_tile (all tiles) ───\n");

    uint32_t n_direct_ok = 0;
    for (uint32_t ti = 0; ti < ge->n_tiles; ti++) {
        uint8_t dec_tile[GSTEN_TILE_SZ];
        if (gb_decode_tile(ge, ti, dec_tile) != RB_OK) continue;

        size_t orig_off = (size_t)ti * GSTEN_TILE_SZ;
        int cmp_n = GSTEN_TILE_SZ;
        if (orig_off + cmp_n > qdat_sz) cmp_n = (int)(qdat_sz - orig_off);

        if (memcmp(dec_tile, (uint8_t *)qdat_data + orig_off, cmp_n) == 0)
            n_direct_ok++;
    }
    printf("  gb_decode_tile: %u / %u lossless\n", n_direct_ok, ge->n_tiles);
    CHECK(n_direct_ok == ge->n_tiles, "all tiles lossless via gb_decode_tile");

    /* ── Assertions ── */
    printf("\n─── Checks ───\n");
    CHECK(n_hot + n_cold == n_chunks, "HOT + COLD == total chunks");
    CHECK(n_lossless == n_hot, "all HOT tiles lossless");
    CHECK(n_mismatch == 0, "zero tile mismatches");
    CHECK(s.total_hot == n_hot, "stats total_hot matches");
    CHECK(s.total_cold == n_cold, "stats total_cold matches");

    printf("\n────────────────────────\n");
    printf("  Results: %d pass, %d fail\n", _pass, _fail);

    free(chunks); free(idxs); free(keys); free(results);
    gb_free(&gb); rb_free(&rb);
    return _fail;
}

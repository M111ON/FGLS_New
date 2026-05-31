/*
 * test_gsp.c — GeomShadowPipe integration test
 *
 * Tests:
 *   T1: HOT chunk (int-like) → classify HOT → decode tile → valid==1
 *   T2: COLD chunk (float-like) → classify COLD → valid==0, ring pushed
 *   T3: COLD retrievable via bond_key after push
 *   T4: batch 8 HOT + 4 COLD → correct n_hot count
 *   T5: ring eviction after 144 COLD pushes
 *   T6: gsp_stats reflects counters correctly
 *
 * Compile:
 *   gcc -O2 -DGEOM_RAW_BRIDGE_IMPLEMENTATION \
 *           -DGEOM_ROUTER_BRIDGE_IMPLEMENTATION \
 *           -DGEOM_SHADOW_PIPE_IMPLEMENTATION \
 *           -I. -o test_gsp test_gsp.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

/* ── test helpers ────────────────────────────────────────────── */
static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); _fail++; } \
} while(0)

/* synthetic GeomBridge: N entries, each with 720 FLAT-encoded tiles */
static void _make_gb(GeomBridge *gb, uint32_t n) {
    memset(gb, 0, sizeof(*gb));
    for (uint32_t e = 0; e < n && e < RB_MAX_ENTRIES; e++) {
        GstenEntry *ge = &gb->entries[e];
        snprintf(ge->name, GSTEN_NAME_MAX, "tensor_%04u", e);
        ge->n_tiles   = 720;
        ge->tile_sz   = GSTEN_TILE_SZ;
        ge->occupied  = 1;
        ge->index     = malloc(720 * sizeof(GstenIndexEntry));
        ge->data      = malloc(720 * 2);
        ge->data_size = 720 * 2;
        ge->store_blob = malloc(1);
        ge->store_size = 1;
        uint8_t val = (uint8_t)(e % 255);
        for (uint32_t ti = 0; ti < 720; ti++) {
            ge->index[ti].tile_offset = ti * 2;
            ge->index[ti].enc_size    = 2;
            ge->index[ti].type        = HENC_FLAT;
            ge->data[ti*2]   = HENC_FLAT;
            ge->data[ti*2+1] = val;
        }
    }
    gb->n_entries = n;
}

static void _free_gb(GeomBridge *gb, uint32_t n) {
    for (uint32_t i = 0; i < n && i < RB_MAX_ENTRIES; i++) {
        if (gb->entries[i].occupied) {
            free(gb->entries[i].index);
            free(gb->entries[i].data);
            free(gb->entries[i].store_blob);
            gb->entries[i].occupied = 0;
        }
    }
}

/* HOT chunk: integers 0..63 (range=63 < 64 → HOT) */
static void _make_hot_chunk(uint8_t *out) {
    for (int i = 0; i < 64; i++) out[i] = (uint8_t)i;
}

/* COLD chunk: float32 bytes (range ≥ 64, transitions ≥ 96 → COLD) */
static void _make_cold_chunk(uint8_t *out) {
    float fv[16];
    for (int i = 0; i < 16; i++) fv[i] = (float)(i * 0.314f + 1.5f);
    memcpy(out, fv, 64);
}

/* ── T1: HOT → valid tile ───────────────────────────────────── */
static void test_hot_decode(GspCtx *ctx) {
    printf("\n[T1] HOT chunk → tile decode\n");

    uint8_t chunk[64]; _make_hot_chunk(chunk);
    GspResult r;
    int ret = gsp_push(ctx, chunk, 42, 2, 0, 0xAA00u, 0xBEEF01u, &r);

    CHECK(ret == GSP_OK,                        "gsp_push returns OK");
    CHECK(r.temperature == BERMUDA_SHADOW_HOT,  "temperature == HOT");
    CHECK(r.valid == 1,                         "valid == 1");
    /* FLAT tile: all bytes equal */
    int flat_ok = 1;
    for (int i = 1; i < GSTEN_TILE_SZ; i++)
        if (r.tile[i] != r.tile[0]) { flat_ok = 0; break; }
    CHECK(flat_ok, "decoded tile is FLAT");
    printf("  tile[0]=%u  zone=%u  tring_slot=%u\n",
           r.tile[0], r.route.zone, r.route.tring_slot);
}

/* ── T2: COLD → valid==0, ring pushed ──────────────────────── */
static void test_cold_skip(GspCtx *ctx) {
    printf("\n[T2] COLD chunk (float-like) → skip decode, ring push\n");

    uint8_t chunk[64]; _make_cold_chunk(chunk);
    GspResult r;
    gsp_push(ctx, chunk, 10, 2, 0, 0xCC00u, 0xDEAD01u, &r);

    CHECK(r.temperature == BERMUDA_SHADOW_COLD, "temperature == COLD");
    CHECK(r.valid == 0,                         "valid == 0 (temperature gate)");
    CHECK(r.bond_key == 0xDEAD01u,              "bond_key preserved");
    /* polarity may be anything — routing lane independent of decode gate */
}

/* ── T3: COLD retrievable ────────────────────────────────────── */
static void test_cold_retrieve(GspCtx *ctx) {
    printf("\n[T3] COLD retrieval via bond_key\n");

    uint8_t chunk[64]; _make_cold_chunk(chunk);
    uint64_t key = 0xC01D1234u;
    GspResult r;
    gsp_push(ctx, chunk, 7, 2, 0, 0xBB00u, key, &r);

    const BermudaShadowEntry *e = gsp_retrieve_cold(ctx, key);
    CHECK(e != NULL, "gsp_retrieve_cold: found entry");
    if (e) {
        CHECK(e->bond_key == key,                 "bond_key matches");
        CHECK(e->temperature == BERMUDA_SHADOW_COLD, "temperature == COLD");
        CHECK(memcmp(e->data, chunk, 64) == 0,    "original chunk preserved");
    }
}

/* ── T4: batch 8 HOT + 4 COLD ───────────────────────────────── */
static void test_batch(GspCtx *ctx) {
    printf("\n[T4] Batch: 8 HOT + 4 COLD\n");

    uint8_t   chunks[12 * 64];
    uint16_t  idxs[12];
    uint64_t  keys[12];
    GspResult results[12];

    for (int i = 0; i < 8; i++) {
        _make_hot_chunk(chunks + i * 64);
        idxs[i] = (uint16_t)(i * 37);
        keys[i] = (uint64_t)(0xAA00 + i);
    }
    for (int i = 8; i < 12; i++) {
        _make_cold_chunk(chunks + i * 64);
        idxs[i] = (uint16_t)(i * 37);
        keys[i] = (uint64_t)(0xCC00 + i);
    }

    uint32_t n_hot = gsp_push_batch(ctx, chunks, idxs,
                                    NULL, keys, 2, 0, results, 12);

    CHECK(n_hot == 8, "batch: 8 HOT tiles decoded");

    int cold_ok = 1;
    for (int i = 8; i < 12; i++)
        if (results[i].valid != 0) { cold_ok = 0; break; }
    CHECK(cold_ok, "batch: 4 COLD have valid==0");

    int hot_ok = 1;
    for (int i = 0; i < 8; i++)
        if (results[i].valid != 1) { hot_ok = 0; break; }
    CHECK(hot_ok, "batch: 8 HOT have valid==1");
}

/* ── T5: ring eviction after 144+ COLD ──────────────────────── */
static void test_ring_eviction(GspCtx *ctx) {
    printf("\n[T5] Ring eviction after 144 COLD pushes\n");

    uint8_t chunk[64]; _make_cold_chunk(chunk);
    /* push 150 COLD entries — 6 should evict */
    for (int i = 0; i < 150; i++) {
        GspResult r;
        gsp_push(ctx, chunk, (uint16_t)(i % 720), 2, 0,
                 (uint64_t)(0xE000 + i), (uint64_t)(0xE000 + i), &r);
    }
    GspStats s = gsp_stats(ctx);
    CHECK(s.ring_count == BERMUDA_SHADOW_RING, "ring at capacity (144)");
    CHECK(s.evictions > 0,                    "evictions > 0");
    printf("  evictions=%u  ring_count=%u\n", s.evictions, s.ring_count);
}

/* ── T6: stats ───────────────────────────────────────────────── */
static void test_stats(GspCtx *ctx) {
    printf("\n[T6] Stats reflect HOT/COLD counts\n");
    GspStats s = gsp_stats(ctx);
    CHECK(s.total_hot  > 0, "total_hot > 0");
    CHECK(s.total_cold > 0, "total_cold > 0");
    printf("  total_hot=%u  total_cold=%u  evictions=%u  ring=%u\n",
           s.total_hot, s.total_cold, s.evictions, s.ring_count);
}

/* ── main ────────────────────────────────────────────────────── */
int main(void) {
    printf("=== GeomShadowPipe Test ===\n");

    /* Setup */
    GeomBridge gb;
    _make_gb(&gb, 24);

    GeomRouterBridge grb;
    grb_build(&grb, &gb);

    BermudaShadowRing ring;
    bermuda_shadow_ring_init(&ring);

    /* Verify shadow classify works correctly */
    int sv = bermuda_shadow_verify();
    CHECK(sv == 0, "bermuda_shadow_verify passes");

    GspCtx ctx;
    gsp_init(&ctx, &grb, &ring);

    test_hot_decode(&ctx);
    test_cold_skip(&ctx);
    test_cold_retrieve(&ctx);
    test_batch(&ctx);
    test_ring_eviction(&ctx);
    test_stats(&ctx);

    printf("\n────────────────────────────────────\n");
    printf("  Results: %d pass, %d fail\n", _pass, _fail);

    _free_gb(&gb, 24);
    return _fail;
}

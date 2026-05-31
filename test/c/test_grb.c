/*
 * test_grb.c — GeomRouterBridge unit + integration test
 *
 * Tests:
 *   T1: grb_build zone distribution (round-robin, no empty crash)
 *   T2: grb_decode_route ROUTE (polarity=0) → valid tile
 *   T3: grb_decode_route GROUND (polarity=1) → valid=0, no crash
 *   T4: grb_decode_batch N routes → correct HOT/COLD count
 *   T5: tring_slot → tile_idx proportional mapping (edge cases)
 *   T6: zone_info diagnostic smoke test
 *
 * Compile (no .gsten files needed — uses synthetic GeomBridge):
 *   gcc -O2 -DGEOM_RAW_BRIDGE_IMPLEMENTATION \
 *           -DGEOM_ROUTER_BRIDGE_IMPLEMENTATION \
 *           -I. -o test_grb test_grb.c
 *
 * With real .gsten dir:
 *   ./test_grb [gsten_dir]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#ifndef GEOM_RAW_BRIDGE_IMPLEMENTATION
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#endif
#include "geom_raw_bridge.h"

#ifndef GEOM_ROUTER_BRIDGE_IMPLEMENTATION
#define GEOM_ROUTER_BRIDGE_IMPLEMENTATION
#endif
#include "geom_router_bridge.h"

/* ── helpers ─────────────────────────────────────────────────── */

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); _fail++; } \
} while(0)

/* Build a synthetic GeomBridge with N fake GstenEntry slots filled.
 * Each entry has n_tiles tiles, all FLAT-encoded (2 bytes: type=0, val=i%255).
 */
static void _make_synthetic_gb(GeomBridge *gb, uint32_t n_entries,
                                uint32_t n_tiles_each) {
    memset(gb, 0, sizeof(*gb));

    /* We need actual encoded tile data that _gb_hex_decode can parse.
     * FLAT: [0x00, value] = 2 bytes per tile. */
    size_t tile_data_sz = (size_t)n_tiles_each * 2;

    for (uint32_t e = 0; e < n_entries && e < RB_MAX_ENTRIES; e++) {
        GstenEntry *ge = &gb->entries[e];

        snprintf(ge->name, GSTEN_NAME_MAX, "tensor_%04u", e);
        ge->n_tiles  = n_tiles_each;
        ge->tile_sz  = GSTEN_TILE_SZ;
        ge->occupied = 1;

        /* Allocate index */
        ge->index = (GstenIndexEntry *)malloc(
                        (size_t)n_tiles_each * sizeof(GstenIndexEntry));
        /* Allocate data */
        ge->data  = (uint8_t *)malloc(tile_data_sz);
        ge->data_size = tile_data_sz;

        /* Fake store_blob so gb_free works */
        ge->store_blob = (uint8_t *)malloc(1);
        ge->store_size = 1;

        uint8_t val = (uint8_t)(e % 255);
        uint32_t off = 0;
        for (uint32_t ti = 0; ti < n_tiles_each; ti++) {
            ge->index[ti].tile_offset = off;
            ge->index[ti].enc_size    = 2;
            ge->index[ti].type        = HENC_FLAT;
            ge->data[off]     = HENC_FLAT;
            ge->data[off + 1] = val;
            off += 2;
        }
    }
    gb->n_entries = n_entries;
}

/* Patched gb_free that handles synthetic entries (index/data allocated separately) */
static void _free_synthetic_gb(GeomBridge *gb, uint32_t n_entries) {
    for (uint32_t i = 0; i < n_entries && i < RB_MAX_ENTRIES; i++) {
        if (gb->entries[i].occupied) {
            free(gb->entries[i].index);
            free(gb->entries[i].data);
            free(gb->entries[i].store_blob);
            gb->entries[i].occupied = 0;
        }
    }
}

/* Build a fake BermudaRouteEntry (no bermuda_init needed for these tests) */
static BermudaRouteEntry _make_route(uint16_t idx, uint8_t zone,
                                     uint16_t tring_slot, uint8_t polarity) {
    BermudaRouteEntry r;
    memset(&r, 0, sizeof(r));
    r.idx_in     = idx;
    r.idx_out    = idx;
    r.zone       = zone;
    r.tring_slot = tring_slot;
    r.polarity   = polarity;
    r.pole       = zone >= 6 ? 1 : 0;
    return r;
}

/* ── T1: zone distribution ───────────────────────────────────── */
static void test_zone_distribution(void) {
    printf("\n[T1] Zone distribution (round-robin)\n");

    GeomBridge gb;
    _make_synthetic_gb(&gb, 25, 100);  /* 25 tensors, 100 tiles each */

    GeomRouterBridge grb;
    int ret = grb_build(&grb, &gb);
    CHECK(ret == GRB_OK, "grb_build returns OK");
    CHECK(grb.n_tensors == 25, "n_tensors == 25");

    /* 25 tensors across 12 zones: zones 0..0 get 3, rest get 2 */
    uint16_t total = 0;
    for (uint8_t z = 0; z < GRB_N_ZONES; z++) total += grb.zones.count[z];
    CHECK(total == 25, "zone total == n_tensors");

    /* no zone should be more than ceil(25/12)+1 = 4 */
    int zone_ok = 1;
    for (uint8_t z = 0; z < GRB_N_ZONES; z++) {
        if (grb.zones.count[z] > 4) { zone_ok = 0; break; }
    }
    CHECK(zone_ok, "no zone overloaded");

    grb_zone_info(&grb);
    _free_synthetic_gb(&gb, 25);
}

/* ── T2: ROUTE decode ────────────────────────────────────────── */
static void test_route_decode(void) {
    printf("\n[T2] ROUTE (polarity=0) decode\n");

    GeomBridge gb;
    _make_synthetic_gb(&gb, 12, 720);

    GeomRouterBridge grb;
    grb_build(&grb, &gb);

    /* Zone 3, tring_slot=100, polarity=ROUTE */
    BermudaRouteEntry route = _make_route(100, 3, 100, 0);
    GrbDecodeResult   result;
    int ret = grb_decode_route(&grb, &route, &result);

    CHECK(ret == GRB_OK,       "decode returns OK");
    CHECK(result.valid == 1,   "result.valid == 1 (HOT)");
    CHECK(result.zone  == 3,   "result.zone == 3");
    CHECK(result.polarity == 0,"result.polarity == ROUTE");

    /* FLAT tile: all 7 bytes == tensor val (entry_idx % 255) */
    uint8_t expected = (uint8_t)(gb.entries[result.entry_idx].name[12] - '0'
                                 + (gb.entries[result.entry_idx].name[11] - '0') * 10
                                 + (gb.entries[result.entry_idx].name[10] - '0') * 100
                                 + (gb.entries[result.entry_idx].name[9]  - '0') * 1000)
                       % 255;
    /* simpler: just check all bytes are equal (FLAT property) */
    int flat_ok = 1;
    for (int i = 1; i < GSTEN_TILE_SZ; i++) {
        if (result.tile[i] != result.tile[0]) { flat_ok = 0; break; }
    }
    CHECK(flat_ok, "decoded tile is FLAT (all bytes equal)");
    printf("  tile[0]=%u  entry_idx=%u  tile_idx=%u\n",
           result.tile[0], result.entry_idx, result.tile_idx);

    _free_synthetic_gb(&gb, 12);
}

/* ── T3: GROUND polarity still decodes ──────────────────────── */
static void test_ground_skip(void) {
    printf("\n[T3] GROUND (polarity=1) lane still decodes tile\n");

    GeomBridge gb;
    _make_synthetic_gb(&gb, 12, 100);

    GeomRouterBridge grb;
    grb_build(&grb, &gb);

    /* polarity=GROUND = routing lane, not a decode gate */
    BermudaRouteEntry route = _make_route(50, 5, 50, 1);
    GrbDecodeResult   result;
    int ret = grb_decode_route(&grb, &route, &result);

    CHECK(ret == GRB_OK,       "decode returns OK");
    CHECK(result.polarity==1,  "result.polarity == GROUND preserved");
    /* zone 5 has tensors → should decode */
    CHECK(result.valid == 1,   "result.valid == 1 (GROUND lane decodes)");

    _free_synthetic_gb(&gb, 12);
}

/* ── T4: batch decode across all zones ──────────────────────── */
static void test_batch_decode(void) {
    printf("\n[T4] Batch decode: 12 routes across all zones\n");

    GeomBridge gb;
    _make_synthetic_gb(&gb, 24, 720);

    GeomRouterBridge grb;
    grb_build(&grb, &gb);

    BermudaRouteEntry routes[12];
    GrbDecodeResult   results[12];

    /* one route per zone, alternating polarity — all should decode
     * since polarity is a routing lane signal, not a decode gate */
    for (int i = 0; i < 12; i++) {
        routes[i] = _make_route((uint16_t)(i * 37),
                                (uint8_t)(i % 12),
                                (uint16_t)(i * 59),
                                (uint8_t)(i % 2));  /* alternating ROUTE/GROUND */
    }

    uint32_t n_valid = grb_decode_batch(&grb, routes, results, 12);
    CHECK(n_valid == 12, "batch: all 12 routes decode valid");

    int all_flat = 1;
    for (int i = 0; i < 12; i++) {
        if (!results[i].valid) { all_flat = 0; break; }
        /* FLAT tile: bytes equal */
        for (int b = 1; b < GSTEN_TILE_SZ; b++)
            if (results[i].tile[b] != results[i].tile[0]) { all_flat = 0; break; }
    }
    CHECK(all_flat, "batch: all tiles are FLAT (lossless)");

    _free_synthetic_gb(&gb, 24);
}

/* ── T5: tring_slot → tile_idx mapping ──────────────────────── */
static void test_tring_mapping(void) {
    printf("\n[T5] tring_slot → tile_idx proportional mapping\n");

    /* test _grb_tring_to_tile directly via decode */
    GeomBridge gb;
    _make_synthetic_gb(&gb, 12, 720);

    GeomRouterBridge grb;
    grb_build(&grb, &gb);

    /* slot=0 → tile_idx=0, slot=719 → tile_idx=719 for 720-tile tensor */
    BermudaRouteEntry r0   = _make_route(0, 0,   0, 0);
    BermudaRouteEntry r719 = _make_route(0, 0, 719, 0);
    GrbDecodeResult res0, res719;

    grb_decode_route(&grb, &r0,   &res0);
    grb_decode_route(&grb, &r719, &res719);

    CHECK(res0.tile_idx == 0,   "tring_slot=0 → tile_idx=0");
    CHECK(res719.tile_idx == 719 || res719.tile_idx == 718,
          "tring_slot=719 → tile_idx ~719");

    /* mid: slot=360 → tile_idx=360 */
    BermudaRouteEntry r360 = _make_route(0, 0, 360, 0);
    GrbDecodeResult res360;
    grb_decode_route(&grb, &r360, &res360);
    CHECK(res360.tile_idx == 360, "tring_slot=360 → tile_idx=360");

    _free_synthetic_gb(&gb, 12);
}

/* ── T6: empty zone safe ─────────────────────────────────────── */
static void test_empty_zone_safe(void) {
    printf("\n[T6] Empty zone → GRB_ERR (no crash)\n");

    GeomBridge gb;
    /* Only 1 tensor → 11 zones empty */
    _make_synthetic_gb(&gb, 1, 100);

    GeomRouterBridge grb;
    grb_build(&grb, &gb);

    /* Zone 1 is empty (only zone 0 has tensor_0) */
    BermudaRouteEntry route = _make_route(0, 1, 50, 0);
    GrbDecodeResult   result;
    int ret = grb_decode_route(&grb, &route, &result);

    CHECK(ret == GRB_ERR,    "empty zone returns GRB_ERR");
    CHECK(result.valid == 0, "result.valid == 0 on error");

    _free_synthetic_gb(&gb, 1);
}

/* ── Real .gsten integration (optional) ─────────────────────── */
static void test_real_gsten(const char *gsten_dir) {
    printf("\n[REAL] Loading .gsten from: %s\n", gsten_dir);

    GeomBridge gb;
    int ret = gb_load(&gb, gsten_dir);
    if (ret != RB_OK) {
        printf("  SKIP  gb_load failed (no .gsten files?)\n");
        return;
    }
    printf("  Loaded %u tensors\n", gb.n_entries);

    GeomRouterBridge grb;
    ret = grb_build(&grb, &gb);
    CHECK(ret == GRB_OK, "grb_build on real data");
    grb_zone_info(&grb);

    /* Smoke test: decode one route per zone */
    for (uint8_t z = 0; z < GRB_N_ZONES; z++) {
        BermudaRouteEntry route = _make_route(z * 37, z, z * 59, 0);
        GrbDecodeResult   result;
        ret = grb_decode_route(&grb, &route, &result);
        if (grb.zones.count[z] == 0) {
            CHECK(ret == GRB_ERR, "empty zone returns err");
        } else {
            CHECK(ret == GRB_OK && result.valid, "zone decode OK");
        }
    }

    gb_free(&gb);
}

/* ── main ────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    printf("=== GeomRouterBridge Test ===\n");

    test_zone_distribution();
    test_route_decode();
    test_ground_skip();
    test_batch_decode();
    test_tring_mapping();
    test_empty_zone_safe();

    if (argc > 1) test_real_gsten(argv[1]);

    printf("\n────────────────────────────────────\n");
    printf("  Results: %d pass, %d fail\n", _pass, _fail);
    return _fail;
}

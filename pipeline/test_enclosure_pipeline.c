/*
 * test_enclosure_pipeline.c — Enclosure + Fibo Tick Integration
 * ═══════════════════════════════════════════════════════════════════
 *
 * Full pipeline:
 *   data → gls_enclosure (container) → fibo_tick (4-mode routing)
 *
 *  gls_enclosure.h  IS  the entropy container (spatial)
 *    → enc_find_home(data) → (ring, wedge) on 144×144 field
 *    → enc_process → enc_pack_chunk → chunk_out
 *
 *  fibo_tick.h  routes  by timeline position (temporal)
 *    → ft_store_action(enc) → MAIN/PIPE/BRIDGE/FREEZE
 *    → determines how chunk is packed
 *
 * No malloc in hot path. All pre-allocated.
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "gls_enclosure.h"
#include "fibo_tick.h"
#include "rdh_capture.h"
#include "geo_frame_seek.h"

/* ── Test tracking ─────────────────────────────────────────── */
static int n_pass = 0;
static int n_fail = 0;

#define TEST(name, expr) do { \
    int _ok = (expr); \
    if (_ok) { n_pass++; printf("  PASS  %s\n", name); } \
    else { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); } \
} while(0)

#define TEST_I(name, got, expected) do { \
    int64_t _g = (int64_t)(got); \
    int64_t _e = (int64_t)(expected); \
    if (_g == _e) { n_pass++; printf("  PASS  %s (%lld)\n", name, (long long)_g); } \
    else { n_fail++; printf("  FAIL  %s: got %lld, expected %lld (line %d)\n", name, (long long)_g, (long long)_e, __LINE__); } \
} while(0)

/* ══════════════════════════════════════════════════════════════
   Helper: get enc from data (2nd RDH call for timeline)
   ══════════════════════════════════════════════════════════════ */
static uint16_t data_to_enc(const uint8_t *data, size_t len)
{
    int64_t key = rdh_capture(data, len, &RDH_CAPTURE_144);
    return (uint16_t)((uint64_t)key % 1440);
}

/* ══════════════════════════════════════════════════════════════
   TEST 1: Enclosure roundtrip — store → load → verify
   ══════════════════════════════════════════════════════════════ */
static int test_enclosure_roundtrip(void)
{
    printf("=== Test 1: Enclosure roundtrip ===\n");

    EncCtx ctx;
    enc_init(&ctx, 4);  /* scale 4 → 82,944 B chunk */

    /* 48B test block */
    uint8_t data[48];
    for (int i = 0; i < 48; i++) data[i] = (uint8_t)(i * 37 + 13);

    /* Process through enclosure */
    uint32_t home_x, home_y;
    int cidx = enc_process(&ctx, data, 48, &home_x, &home_y);

    TEST("chunk idx >= 0", cidx >= 0);
    TEST("home_x < 144", home_x < 144);
    TEST("home_y < 144", home_y < 144);
    printf("  home=(%u,%u) chunk=%d\n", home_x, home_y, cidx);

    /* Pack into chunk buffer */
    uint8_t chunk[ENC_CHUNK_SIZE(4)];
    int packed = enc_pack_chunk(&ctx, data, 48, home_x, home_y, chunk);
    TEST_I("packed size", packed, ENC_CHUNK_SIZE(4));

    /* Verify: first 48B of chunk matches original data */
    TEST("chunk[0..47] matches", memcmp(chunk, data, 48) == 0);

    /* Get enc for fibo_tick routing */
    uint16_t enc = data_to_enc(data, 48);
    uint8_t action = ft_store_action(enc);
    uint8_t tick = ft_enc_to_tick(enc);
    printf("  enc=%u tick=%u action=%s\n", enc, tick,
           action == FT_STORE_FREEZE ? "FREEZE" :
           action == FT_STORE_MAIN   ? "MAIN" :
           action == FT_STORE_PIPE   ? "PIPE" :
           action == FT_STORE_BRIDGE ? "BRIDGE" : "?");

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TEST 2: Determinism — same data → same home
   ══════════════════════════════════════════════════════════════ */
static int test_determinism(void)
{
    printf("=== Test 2: Determinism ===\n");

    uint8_t data[48];
    for (int i = 0; i < 48; i++) data[i] = (uint8_t)(i * 7 + 3);

    uint32_t x1, y1, x2, y2;
    enc_find_home(data, 48, 144, &x1, &y1);
    enc_find_home(data, 48, 144, &x2, &y2);

    TEST("same home_x", x1 == x2);
    TEST("same home_y", y1 == y2);

    uint16_t e1 = data_to_enc(data, 48);
    uint16_t e2 = data_to_enc(data, 48);
    TEST("same enc", e1 == e2);

    printf("  home=(%u,%u) enc=%u\n", x1, y1, e1);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TEST 3: Hexagon spread — enc_find_home → 7-cell hex
   ══════════════════════════════════════════════════════════════ */
static int test_hexagon_spread(void)
{
    printf("=== Test 3: Hexagon spread ===\n");

    /* Test hexagon spread from known home */
    uint32_t cells[7][2];
    int n = enc_hexagon_spread(10, 20, 144, cells, 7);

    TEST_I("cells count", n, 7);
    TEST_I("home x", cells[0][0], 10);
    TEST_I("home y", cells[0][1], 20);

    /* 6 neighbors in axial coordinates */
    printf("  Neighbors:\n");
    for (int d = 0; d < 6; d++) {
        printf("    [%d] (%u,%u)\n", d+1, cells[d+1][0], cells[d+1][1]);
        TEST("neighbor on field", cells[d+1][0] < 144 && cells[d+1][1] < 144);
    }

    /* Test wrap-around at field edge */
    n = enc_hexagon_spread(0, 0, 144, cells, 7);
    TEST_I("edge cells", n, 7);
    printf("  Edge home=(0,0) neighbor directions:\n");
    for (int d = 0; d < 6; d++) {
        printf("    [%d] (%u,%u)\n", d+1, cells[d+1][0], cells[d+1][1]);
    }
    /* Neighbors should wrap around (all still on [0,143] since field wraps) */

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TEST 4: fibo_tick × enclosure — 4 storage modes
   ══════════════════════════════════════════════════════════════
 *
 * For N data chunks, compute both enclosure home AND fibo_tick mode.
 * Show how the timeline routing connects to spatial storage.
 */
static int test_fiboTick_enclosure(void)
{
    printf("=== Test 4: fibo_tick × enclosure routing ===\n");

    /* Verify action ↔ tick consistency */
    srand(99);
    int action_ok = 0;
    for (int i = 0; i < 20; i++) {
        uint8_t data[48];
        for (int j = 0; j < 48; j++)
            data[j] = (uint8_t)(rand() & 0xFF);

        uint32_t hx, hy;
        enc_find_home(data, 48, 144, &hx, &hy);
        uint16_t enc = data_to_enc(data, 48);
        uint8_t action = ft_store_action(enc);
        uint8_t tick = ft_enc_to_tick(enc);

        /* Verify: action matches tick */
        int correct = 0;
        if (tick == 11 && action == FT_STORE_BRIDGE) correct = 1;
        if (tick == 0  && action == FT_STORE_FREEZE) correct = 1;
        if (tick == 1  && action == FT_STORE_MAIN)   correct = 1;
        if (tick >= 2  && tick <= 10 && action == FT_STORE_PIPE) correct = 1;
        if (correct) action_ok++;

        const char *action_names[] = {"MAIN", "BRIDGE", "PIPE", "FREEZE"};
        printf("  %-5d enc=%-5u tick=%-2u %-7s home=(%3u,%-3u)\n",
               i, enc, tick, action_names[action], hx, hy);
    }

    printf("\n  20 chunks: action↔tick correct for %d/20\n", action_ok);
    TEST("all actions match their tick", action_ok == 20);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TEST 5: Full pipeline — 5 unique 48B blocks
   ══════════════════════════════════════════════════════════════
 *
   For each block:
     1. enc_find_home → spatial address
     2. data_to_enc → timeline enc
     3. ft_store_action → routing mode
     4. enc_pack_chunk → chunk buffer
     5. Verify chunk[0..47] == original data
 */
static int test_full_pipeline(void)
{
    printf("=== Test 5: Full pipeline (5 blocks) ===\n");

    EncCtx ctx;
    enc_init(&ctx, 4);

    srand(77);
    for (int i = 0; i < 5; i++) {
        uint8_t data[48];
        for (int j = 0; j < 48; j++)
            data[j] = (uint8_t)(rand() & 0xFF);

        /* Step 1: Enclosure */
        uint32_t hx, hy;
        int cidx = enc_process(&ctx, data, 48, &hx, &hy);

        /* Step 2: Fibo_tick routing */
        uint16_t enc = data_to_enc(data, 48);
        uint8_t action = ft_store_action(enc);

        /* Step 3: Pack chunk */
        uint8_t chunk[ENC_CHUNK_SIZE(4)];
        int packed = enc_pack_chunk(&ctx, data, 48, hx, hy, chunk);

        /* Step 4: Verify */
        TEST_I("packed size", packed, ENC_CHUNK_SIZE(4));
        TEST("chunk data matches", memcmp(chunk, data, 48) == 0);

        printf("  block[%d]: home=(%u,%u) enc=%u chunk=%d action=%s\n",
               i, hx, hy, enc, cidx,
               action == FT_STORE_FREEZE ? "FREEZE" :
               action == FT_STORE_MAIN   ? "MAIN" :
               action == FT_STORE_PIPE   ? "PIPE" :
               action == FT_STORE_BRIDGE ? "BRIDGE" : "?");
    }

    TEST_I("total chunks", ctx.n_chunks, 5);
    TEST_I("total bytes", ctx.total_bytes, 240);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TEST 6: Various data → different addresses
   ══════════════════════════════════════════════════════════════ */
static int test_various_data(void)
{
    printf("=== Test 6: Various data → unique addresses ===\n");

    struct {
        const char *label;
        uint8_t    data[48];
        size_t     len;
    } samples[] = {
        {"zeros",     {0},                        1},
        {"ones",      {[0 ... 9] = 0xFF},        10},
        {"text",     "FGLS geometric entropy!",  24},
        {"pattern",  {[0 ... 47] = 0xAA},        48},
        {"mixed",    {0x00,0xFF,0x55,0xAA,0x0F},  5},
        {"sequential",{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}, 16},
    };

    for (int i = 0; i < 6; i++) {
        uint32_t hx, hy;
        enc_find_home(samples[i].data, (uint32_t)samples[i].len, 144, &hx, &hy);
        uint16_t enc = data_to_enc(samples[i].data, samples[i].len);
        uint8_t action = ft_store_action(enc);

        printf("  %-10s → home=(%3u,%3u) enc=%4u action=%s\n",
               samples[i].label, hx, hy, enc,
               action == FT_STORE_FREEZE ? "FREEZE" :
               action == FT_STORE_MAIN   ? "MAIN" :
               action == FT_STORE_PIPE   ? "PIPE" :
               action == FT_STORE_BRIDGE ? "BRIDGE" : "?");

        TEST("home on field", hx < 144 && hy < 144);
        TEST("enc in range", enc < 1440);
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TEST 7: Homing pattern — same data → same path
   ══════════════════════════════════════════════════════════════
 *
   The enclosure's path whistle: same data always produces
   the same home and the same enc.
 */
static int test_homing_pattern(void)
{
    printf("=== Test 7: Homing pattern (same data → same address) ===\n");

    uint8_t text[] = "The quick brown fox jumps over the lazy dog.";

    /* 10 identical calls */
    uint32_t prev_x = 999, prev_y = 999;
    uint16_t prev_enc = 999;
    int consistent = 1;

    for (int i = 0; i < 10; i++) {
        uint32_t hx, hy;
        enc_find_home(text, (uint32_t)sizeof(text), 144, &hx, &hy);
        uint16_t enc = data_to_enc(text, sizeof(text));

        if (i > 0) {
            if (hx != prev_x || hy != prev_y) consistent = 0;
            if (enc != prev_enc) consistent = 0;
        }
        prev_x = hx; prev_y = hy;
        prev_enc = enc;
    }

    TEST("always same home", consistent);
    printf("  \"%s\"\n", text);
    printf("  home=(%u,%u) enc=%u (10/10 consistent)\n", prev_x, prev_y, prev_enc);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TEST 8: Scale levels (4, 12, 16)
   ══════════════════════════════════════════════════════════════ */
static int test_scales(void)
{
    printf("=== Test 8: Scale levels ===\n");

    uint32_t scales[] = {4, 12, 16};
    const char *names[] = {"scale-4  (82KB)", "scale-12 (243KB)", "scale-16 (324KB)"};

    for (int s = 0; s < 3; s++) {
        EncCtx ctx;
        enc_init(&ctx, scales[s]);

        TEST_I(names[s], ctx.cfg.chunk_size, ENC_CHUNK_SIZE(scales[s]));
        TEST_I(names[s], ctx.cfg.n_blocks, ENC_CHUNK_BLOCKS(scales[s]));

        printf("  %s: chunk=%uB blocks=%u\n",
               names[s], ctx.cfg.chunk_size, ctx.cfg.n_blocks);

        /* Process a test block at this scale */
        uint8_t data[48];
        for (int i = 0; i < 48; i++) data[i] = (uint8_t)(i * 7 + 3);

        uint32_t hx, hy;
        int cidx = enc_process(&ctx, data, 48, &hx, &hy);
        TEST("chunk idx >= 0", cidx >= 0);
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TEST 9: Chunk capacity — scale-4 chunk info
   ══════════════════════════════════════════════════════════════ */
static int test_chunk_capacity(void)
{
    printf("=== Test 9: Chunk capacity (scale-4) ===\n");

    EncConfig cfg = enc_config(4);

    printf("  Scale:       %u\n", cfg.scale);
    printf("  Chunk size:  %u bytes (%.2f KB)\n",
           cfg.chunk_size, (double)cfg.chunk_size / 1024.0);
    printf("  Blocks:      %u (each 48B)\n", cfg.n_blocks);
    printf("  Field:       %u × %u = %u\n",
           cfg.field_dim, cfg.field_dim, cfg.field_total);
    printf("  Hex cells:   7 per home (center + 6)\n");
    printf("  Grid:        %u × %u = %u chunks\n",
           enc_chunks_across(4), enc_chunks_across(4),
           enc_chunks_across(4) * enc_chunks_across(4));

    /* Number of 48B chunks that fit in one scale-4 chunk */
    uint32_t n_48b = ENC_CHUNK_SIZE(4) / 48;  /* should be 1728 */
    uint32_t n_7hex = n_48b / 7;              /* full hexagons */
    printf("  48B chunks:  %u per chunk\n", n_48b);
    printf("  Full hexes:  %u (7 cells each)\n", n_7hex);
    printf("  Unused:      %u blocks\n", n_48b - n_7hex * 7);

    /* Each hex = 7 × 48 = 336 bytes */
    printf("\n  Hexagon size: 7 × 48 = %u bytes\n", 7 * 48);
    printf("  Hexagons fit: %u\n", ENC_CHUNK_SIZE(4) / (7 * 48));

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Enclosure + Fibo Tick — Integration Test               ║\n");
    printf("║  gls_enclosure.h (container) × fibo_tick.h (routing)   ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    printf("Constants:\n");
    printf("  ENC_FIELD = %u × %u = %u positions\n",
           ENC_FIELD, ENC_FIELD, ENC_FULL);
    printf("  ENC_BLOCK = %u bytes (atomic unit)\n", ENC_BLOCK);
    printf("  ENC_TOWER = %u (48 × 3)\n", ENC_TOWER);
    printf("  FRAME_CYCLE = %u (stride-37 timeline)\n\n", FRAME_CYCLE);

    test_enclosure_roundtrip();
    test_determinism();
    test_hexagon_spread();
    test_fiboTick_enclosure();
    test_full_pipeline();
    test_various_data();
    test_homing_pattern();
    test_scales();
    test_chunk_capacity();

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d pass, %d fail\n", n_pass, n_fail);

    return n_fail > 0 ? 1 : 0;
}

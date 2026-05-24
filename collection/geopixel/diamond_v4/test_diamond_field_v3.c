/*
 * test_diamond_field_v3.c — test Shell/Slot/Tring pipeline
 * gcc -O2 -I/path/to/twin_core -o test_diamond_field_v3 test_diamond_field_v3.c -lm
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "geo_diamond_field.h"

#define PASS(msg) printf("[PASS] %s\n", msg)
#define FAIL(msg) printf("[FAIL] %s\n", msg)
#define CHECK(cond, msg) do { if(cond) PASS(msg); else { FAIL(msg); fails++; } } while(0)

static int fails = 0;

/* ── T1: shell size math ─────────────────────────────────────────── */
static void test_shell_math(void) {
    CHECK(shell_size(0) == 1,    "shell_size n=0 = 1");
    CHECK(shell_size(1) == 3,    "shell_size n=1 = 3");
    CHECK(shell_size(8) == 17,   "shell_size n=8 = 17");
    CHECK(shell_slots(0) == 1,   "shell_slots n=0 = 1");
    CHECK(shell_slots(1) == 27,  "shell_slots n=1 = 27");
    CHECK(shell_slots(8) == 4913,"shell_slots n=8 = 4913");
}

/* ── T2: global idx encoding ─────────────────────────────────────── */
static void test_global_idx(void) {
    uint32_t g = slot_global(3, 100);
    CHECK(global_level(g) == 3,   "global_level round-trip");
    CHECK(global_local(g) == 100, "global_local round-trip");

    g = slot_global(8, 4912);
    CHECK(global_level(g) == 8,    "global_level n=8");
    CHECK(global_local(g) == 4912, "global_local max slot");
    CHECK(g < INDEX_SIZE,           "gidx within INDEX_SIZE");
}

/* ── T3: shell flag set/clr/get ──────────────────────────────────── */
static void test_shell_flags(void) {
    Shell sh;
    shell_init(&sh, 3);
    CHECK(!shell_get(&sh, 0),   "flag 0 initially clear");
    shell_set(&sh, 0);
    CHECK(shell_get(&sh, 0),    "flag 0 set");
    shell_set(&sh, 342);
    CHECK(shell_get(&sh, 342),  "flag 342 set");
    CHECK(!shell_get(&sh, 341), "flag 341 still clear");
    shell_clr(&sh, 0);
    CHECK(!shell_get(&sh, 0),   "flag 0 cleared");
    CHECK(shell_get(&sh, 342),  "flag 342 still set after clr 0");
}

/* ── T4: fit() — verify threshold behavior ───────────────────────── */
static void test_fit(void) {
    /* all-zero chunk: fold_fibo_intersect = 0, fits at n=0 */
    uint8_t zero[64]; memset(zero, 0, 64);
    CHECK(shell_fit(zero, 0), "zero chunk fits n=0");

    /* all-0xFF chunk: intersect = 0xFFFF... → popcnt=64, fits only at n=8 */
    uint8_t full[64]; memset(full, 0xFF, 64);
    CHECK(!shell_fit(full, 0), "full chunk doesn't fit n=0");
    CHECK(shell_fit(full, 8),  "full chunk fits n=8");

    /* fit_level: zero → level 0, full → somewhere ≤ 8 */
    uint8_t lvl0 = shell_fit_level(zero);
    CHECK(lvl0 == 0, "zero chunk fits at level 0");
    uint8_t lvl_full = shell_fit_level(full);
    CHECK(lvl_full <= 8, "full chunk fits at some level");
}

/* ── T5: tring push/read/release ─────────────────────────────────── */
static void test_tring(void) {
    Tring t;
    tring_init(&t, 1024);

    uint8_t chunk[64];
    for (int i=0;i<64;i++) chunk[i] = (uint8_t)i;

    uint32_t tick = tring_push(&t, chunk);
    CHECK(tick == 0, "first tick = 0");
    CHECK(tick != UINT32_MAX, "push succeeded");

    const uint8_t *rd = tring_read(&t, tick);
    CHECK(rd != NULL, "read returns non-null");
    CHECK(memcmp(rd, chunk, 64) == 0, "read data matches push");

    tring_release(&t, tick);
    CHECK(tring_read(&t, tick) == NULL, "read after release = NULL");

    tring_free(&t);
    PASS("tring_free");
}

/* ── T6: encode → decode roundtrip ──────────────────────────────── */
static void test_encode_decode(void) {
    DiamondField df;
    dfield_init(&df, 4096);

    uint8_t chunk[64];
    for (int i=0;i<64;i++) chunk[i] = (uint8_t)(i * 3 + 7);

    uint8_t enc_level;
    uint32_t gidx = dfield_encode(&df, chunk, &enc_level);
    CHECK(gidx != SLOT_NULL, "encode returns valid gidx");
    CHECK(enc_level <= 8,    "encode level in range");

    const uint8_t *out = dfield_decode(&df, gidx);
    CHECK(out != NULL, "decode returns non-null");
    CHECK(memcmp(out, chunk, 64) == 0, "decode matches original chunk");

    dfield_free(&df);
}

/* ── T7: delete → decode returns NULL ────────────────────────────── */
static void test_delete(void) {
    DiamondField df;
    dfield_init(&df, 4096);

    uint8_t chunk[64]; memset(chunk, 0xAB, 64);
    uint32_t gidx = dfield_encode(&df, chunk, NULL);
    CHECK(gidx != SLOT_NULL, "encode before delete");

    dfield_delete(&df, gidx);
    const uint8_t *out = dfield_decode(&df, gidx);
    CHECK(out == NULL, "decode after delete = NULL (flag cleared)");

    dfield_free(&df);
}

/* ── T8: GC frees unreferenced tick ─────────────────────────────── */
static void test_gc(void) {
    DiamondField df;
    dfield_init(&df, 4096);

    uint8_t chunk[64]; memset(chunk, 0x55, 64);
    uint32_t gidx = dfield_encode(&df, chunk, NULL);

    uint8_t  n   = global_level(gidx);
    uint16_t idx = global_local(gidx);
    uint32_t tick = sidx_get(&df.sidx, gidx);
    CHECK(tick != SLOT_NULL, "tick assigned before GC");
    CHECK(df.tring.nodes[tick] != NULL, "tring node alive before delete");

    dfield_delete(&df, gidx);
    uint32_t freed = dfield_gc(&df);
    CHECK(freed == 1, "GC freed exactly 1 tick");
    CHECK(df.tring.nodes[tick] == NULL, "tring node freed after GC");

    dfield_free(&df);
}

/* ── T9: reshape moves slots to new level ────────────────────────── */
static void test_reshape(void) {
    DiamondField df;
    dfield_init(&df, 4096);

    /* encode chunk that fits at low level */
    uint8_t chunk[64]; memset(chunk, 0, 64);  /* zero → fits n=0 */
    uint8_t lvl;
    uint32_t gidx = dfield_encode(&df, chunk, &lvl);
    CHECK(gidx != SLOT_NULL, "encode for reshape test");

    uint8_t target = (lvl < 8) ? lvl + 1 : lvl;
    uint32_t moved = dfield_reshape(&df, lvl, target);

    if (lvl < 8) {
        CHECK(moved == 1, "reshape moved 1 slot");
        /* old slot must be invisible now */
        CHECK(!shell_get(&df.shell[lvl], global_local(gidx)),
              "old slot cleared after reshape");
    } else {
        PASS("reshape at max level (no-op expected)");
    }

    dfield_free(&df);
}

/* ── T10: multi-chunk encode, no collision ───────────────────────── */
static void test_multi_encode(void) {
    DiamondField df;
    dfield_init(&df, 65536);

    uint32_t gidxs[100];
    uint8_t  chunks[100][64];
    int ok = 1;
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 64; j++) chunks[i][j] = (uint8_t)(i*7 + j);
        gidxs[i] = dfield_encode(&df, chunks[i], NULL);
        if (gidxs[i] == SLOT_NULL) { ok = 0; break; }
    }
    CHECK(ok, "100 chunks encoded without error");

    /* verify all decode correctly */
    int all_match = 1;
    for (int i = 0; i < 100 && ok; i++) {
        const uint8_t *out = dfield_decode(&df, gidxs[i]);
        if (!out || memcmp(out, chunks[i], 64) != 0) { all_match = 0; break; }
    }
    CHECK(all_match, "all 100 chunks decode correctly");

    dfield_free(&df);
}

int main(void) {
    printf("=== geo_diamond_field v3 tests ===\n");
    test_shell_math();
    test_global_idx();
    test_shell_flags();
    test_fit();
    test_tring();
    test_encode_decode();
    test_delete();
    test_gc();
    test_reshape();
    test_multi_encode();
    printf("\n%s — %d failure(s)\n",
           fails == 0 ? "ALL PASS" : "FAILURES FOUND", fails);
    return fails ? 1 : 0;
}

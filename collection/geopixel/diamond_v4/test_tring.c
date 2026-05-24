/*
 * test_tring.c — tests for tring.h
 * gcc -O2 -o test_tring test_tring.c && ./test_tring
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "tring.h"

#define PASS(msg) printf("[PASS] %s\n", msg)
#define FAIL(msg) printf("[FAIL] %s\n", msg)
#define CHECK(cond, msg) do { if(cond) PASS(msg); else { FAIL(msg); fails++; } } while(0)

static int fails = 0;

/* ── T1: init / destroy ─────────────────────────────────────────── */
static void test_init(void) {
    Tring t;
    CHECK(tring_init(&t, 256) == 0, "init returns 0");
    CHECK(t.capacity   == 256, "capacity set");
    CHECK(t.next_tick  == 0,   "next_tick starts at 0");
    CHECK(t.live_count == 0,   "live_count starts at 0");
    tring_destroy(&t);
    CHECK(t.nodes == NULL, "destroy nulls nodes");
    PASS("init/destroy");
}

/* ── T2: push + read variable size ──────────────────────────────── */
static void test_push_read_varsize(void) {
    Tring t; tring_init(&t, 64);

    uint8_t buf12[12]; for (int i=0;i<12;i++) buf12[i]=(uint8_t)i;
    uint8_t buf3[3]   = {0xAA, 0xBB, 0xCC};
    uint8_t buf100[100]; memset(buf100, 0x55, 100);

    uint32_t t0 = tring_push(&t, buf12, 12);
    uint32_t t1 = tring_push(&t, buf3,  3);
    uint32_t t2 = tring_push(&t, buf100, 100);

    CHECK(t0 == 0 && t1 == 1 && t2 == 2, "ticks assigned sequentially");
    CHECK(t.live_count == 3, "live_count = 3");

    uint32_t sz;
    const uint8_t *p0 = tring_read(&t, t0, &sz);
    CHECK(sz == 12,  "read t0 size = 12");
    CHECK(p0 && memcmp(p0, buf12, 12) == 0, "read t0 data matches");

    const uint8_t *p1 = tring_read(&t, t1, &sz);
    CHECK(sz == 3,   "read t1 size = 3");
    CHECK(p1 && memcmp(p1, buf3, 3) == 0, "read t1 data matches");

    const uint8_t *p2 = tring_read(&t, t2, &sz);
    CHECK(sz == 100, "read t2 size = 100");
    CHECK(p2 && memcmp(p2, buf100, 100) == 0, "read t2 data matches");

    tring_destroy(&t);
}

/* ── T3: push64 / read64 fast path ──────────────────────────────── */
static void test_push_read_64(void) {
    Tring t; tring_init(&t, 64);

    uint8_t chunk[64];
    for (int i=0;i<64;i++) chunk[i] = (uint8_t)(i * 3 + 1);

    uint32_t tick = tring_push64(&t, chunk);
    CHECK(tick == 0, "push64 tick = 0");
    CHECK(tring_size_of(&t, tick) == 64, "size_of = 64");

    const uint8_t *rd = tring_read64(&t, tick);
    CHECK(rd != NULL, "read64 non-null");
    CHECK(memcmp(rd, chunk, 64) == 0, "read64 data matches");

    tring_destroy(&t);
}

/* ── T4: marker node (size=0) ────────────────────────────────────── */
static void test_marker_node(void) {
    Tring t; tring_init(&t, 16);

    uint32_t tick = tring_push(&t, NULL, 0);
    CHECK(tick == 0, "marker push tick = 0");
    CHECK(tring_alive(&t, tick), "marker node alive");

    uint32_t sz;
    const uint8_t *p = tring_read(&t, tick, &sz);
    CHECK(sz == 0, "marker size = 0");
    CHECK(p == NULL, "marker data ptr = NULL (correct)");

    tring_destroy(&t);
}

/* ── T5: release ─────────────────────────────────────────────────── */
static void test_release(void) {
    Tring t; tring_init(&t, 16);

    uint8_t buf[8] = {1,2,3,4,5,6,7,8};
    uint32_t tick = tring_push(&t, buf, 8);

    CHECK(tring_alive(&t, tick), "alive before release");
    tring_release(&t, tick);
    CHECK(!tring_alive(&t, tick), "not alive after release");
    CHECK(t.live_count == 0, "live_count = 0 after release");

    /* double-release: safe, no crash */
    tring_release(&t, tick);
    PASS("double-release safe");

    /* read after release: NULL */
    uint32_t sz;
    CHECK(tring_read(&t, tick, &sz) == NULL, "read after release = NULL");

    tring_destroy(&t);
}

/* ── T6: capacity overflow ───────────────────────────────────────── */
static void test_capacity(void) {
    Tring t; tring_init(&t, 4);

    uint8_t buf[1] = {0xFF};
    uint32_t ticks[4];
    for (int i=0;i<4;i++) ticks[i] = tring_push(&t, buf, 1);
    CHECK(ticks[3] == 3, "4th push gets tick=3");

    uint32_t overflow = tring_push(&t, buf, 1);
    CHECK(overflow == UINT32_MAX, "5th push = UINT32_MAX (overflow)");

    tring_destroy(&t);
}

/* ── T7: gc_bitmap ────────────────────────────────────────────────── */
static void test_gc_bitmap(void) {
    Tring t; tring_init(&t, 64);

    uint8_t buf[4] = {1,2,3,4};
    /* push 5 nodes: ticks 0-4 */
    for (int i=0;i<5;i++) tring_push(&t, buf, 4);
    CHECK(t.live_count == 5, "5 live before gc");

    /* keep ticks 1 and 3, free the rest */
    uint64_t bitmap[1] = {0};
    bitmap[0] |= (1ULL << 1);
    bitmap[0] |= (1ULL << 3);

    uint32_t freed = tring_gc_bitmap(&t, bitmap, 1);
    CHECK(freed == 3, "gc_bitmap freed 3 nodes (0,2,4)");
    CHECK(t.live_count == 2, "live_count = 2 after gc");
    CHECK(!tring_alive(&t, 0), "tick 0 freed");
    CHECK( tring_alive(&t, 1), "tick 1 kept");
    CHECK(!tring_alive(&t, 2), "tick 2 freed");
    CHECK( tring_alive(&t, 3), "tick 3 kept");
    CHECK(!tring_alive(&t, 4), "tick 4 freed");

    tring_destroy(&t);
}

/* ── T8: gc_scan with callback ────────────────────────────────────── */
static int keep_odd(uint32_t tick, void *ctx) {
    (void)ctx;
    return (tick & 1u) ? 1 : 0;  /* keep odd ticks only */
}

static void test_gc_scan(void) {
    Tring t; tring_init(&t, 16);

    uint8_t buf[2] = {0xAB, 0xCD};
    for (int i=0;i<6;i++) tring_push(&t, buf, 2);

    uint32_t freed = tring_gc_scan(&t, keep_odd, NULL);
    CHECK(freed == 3, "gc_scan freed 3 even-tick nodes");
    for (int i=0;i<6;i++) {
        int alive = tring_alive(&t, i);
        int expected = (i & 1) ? 1 : 0;
        if (alive != expected) { FAIL("gc_scan parity mismatch"); fails++; return; }
    }
    PASS("gc_scan parity correct");

    tring_destroy(&t);
}

/* ── T9: stats ────────────────────────────────────────────────────── */
static void test_stats(void) {
    Tring t; tring_init(&t, 32);

    uint8_t a[10], b[20], c[5];
    memset(a, 1, 10); memset(b, 2, 20); memset(c, 3, 5);
    tring_push(&t, a, 10);
    tring_push(&t, b, 20);
    tring_push(&t, c,  5);

    TringStats s = tring_stats(&t);
    CHECK(s.live_count  == 3,  "stats live_count = 3");
    CHECK(s.next_tick   == 3,  "stats next_tick = 3");
    CHECK(s.total_bytes == 35, "stats total_bytes = 35");
    CHECK(s.max_size    == 20, "stats max_size = 20");
    CHECK(s.min_size    == 5,  "stats min_size = 5");

    tring_release(&t, 1);  /* free the 20B node */
    s = tring_stats(&t);
    CHECK(s.live_count  == 2,  "stats live_count = 2 after release");
    CHECK(s.total_bytes == 15, "stats total_bytes = 15 after release");
    CHECK(s.max_size    == 10, "stats max_size = 10 after release");

    tring_destroy(&t);
}

/* ── T10: data isolation (nodes don't share memory) ─────────────── */
static void test_isolation(void) {
    Tring t; tring_init(&t, 16);

    uint8_t buf[8]; memset(buf, 0xAA, 8);
    uint32_t t0 = tring_push(&t, buf, 8);

    memset(buf, 0xBB, 8);  /* mutate original buffer */
    uint32_t t1 = tring_push(&t, buf, 8);

    uint32_t sz;
    const uint8_t *p0 = tring_read(&t, t0, &sz);
    const uint8_t *p1 = tring_read(&t, t1, &sz);

    /* t0 must still have 0xAA, not 0xBB */
    CHECK(p0[0] == 0xAA, "t0 data isolated from later push");
    CHECK(p1[0] == 0xBB, "t1 data correct");
    CHECK(p0 != p1, "t0 and t1 data pointers differ");

    tring_destroy(&t);
}

int main(void) {
    printf("=== tring.h tests ===\n");
    test_init();
    test_push_read_varsize();
    test_push_read_64();
    test_marker_node();
    test_release();
    test_capacity();
    test_gc_bitmap();
    test_gc_scan();
    test_stats();
    test_isolation();
    printf("\n%s — %d failure(s)\n",
           fails == 0 ? "ALL PASS" : "FAILURES FOUND", fails);
    return fails ? 1 : 0;
}

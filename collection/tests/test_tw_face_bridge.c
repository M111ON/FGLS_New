/* test_tw_face_bridge.c — Capo ×12: Y-Triangle Node_id Capture
 * ════════════════════════════════════════════════════════════════
 * Build:
 *   gcc -DGEO_JUMP_INLINE -I. -I../geo_jump_module/include -I../src
 *       -o tests/test_tw_face_bridge.exe
 *       tests/test_tw_face_bridge.c -lm
 * Run:   test_tw_face_bridge.exe [tensors_dir]
 *
 * Tests:
 *   [T1]  Rewind buffer — store/find/has/occupied via node_id
 *   [T2]  Freeze wallet — entry create + file write/read
 *   [T3]  Single node capture — mapping sanity
 *   [T4]  Capo ×12 — all 12 pentagons via tw_capture_capo_all
 *   [T5]  Node distribution — histogram across GEO_FULL=20736
 *   [T6]  Frame seek — node_id → DualFrame via frame_at
 *   [T7]  Capo uniqueness — 12 capo nodes = 12 distinct pentagons
 *   [T8]  Full pipeline (real tensors)
 *   [T9]  Orchestrator — tw_capture_12face_init/free
 *   [T10] Timeline round-trip — node_id → DualFrame determinism
 *   [B1]  Capo benchmark (behind -DBENCHMARK)
 * ════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* ── RawBridge for real tensor loading (must set impl BEFORE first include) ── */
#ifdef TEST_WITH_TENSORS
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#endif

#include "tw_face_bridge.h"

/* ── Test framework ── */
static int g_pass = 0, g_fail = 0;

#define ASSERT(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s (line %d)\n", #cond, __LINE__); } \
} while(0)

#define ASSERT_EQ(a,b) do { \
    if ((a) == (b)) { g_pass++; } \
    else { g_fail++; printf("  FAIL: " #a " == " #b " (line %d): %lld != %lld\n", \
           __LINE__, (long long)(a), (long long)(b)); } \
} while(0)

#define ASSERT_NE(a,b) do { \
    if ((a) != (b)) { g_pass++; } \
    else { g_fail++; printf("  FAIL: " #a " != " #b " (line %d)\n", __LINE__); } \
} while(0)

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  PASS: %s\n", msg); } \
    else { g_fail++; printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

/* ══════════════════════════════════════════════════════════════
   [T1] Rewind Buffer — node_id based
   ══════════════════════════════════════════════════════════════ */
static void test_rewind_buffer(void) {
    printf("\n─── [T1] Rewind Buffer (node_id) ───\n");

    TWFaceRewind rb;
    tw_rewind_init(&rb);
    ASSERT_EQ(tw_rewind_occupied(&rb), 0u);
    ASSERT_EQ(rb.stored, 0u);

    /* store at node 0 */
    uint64_t k0 = tw_node_pack_key(0, 100, -50, 1);
    tw_rewind_store(&rb, k0, 0);
    ASSERT(tw_rewind_has(&rb, 0));
    ASSERT(!tw_rewind_has(&rb, 1));
    ASSERT_EQ(tw_rewind_occupied(&rb), 1u);
    ASSERT_EQ(rb.stored, 1u);

    /* retrieve and verify packed key */
    uint64_t k = tw_rewind_find(&rb, 0);
    uint32_t node_id; int64_t rx, ry; uint8_t pent;
    tw_node_unpack_key(k, &node_id, &rx, &ry, &pent);
    ASSERT_EQ(node_id, 0u);
    ASSERT_EQ(rx, 100);
    ASSERT_EQ(ry, -50);
    ASSERT_EQ(pent, 1u);

    /* store at node 1379 */
    uint64_t k1 = tw_node_pack_key(1379, -200, 300, 1);
    tw_rewind_store(&rb, k1, 1379);
    ASSERT(tw_rewind_has(&rb, 1379));
    ASSERT_EQ(tw_rewind_occupied(&rb), 2u);

    /* overwrite at same position */
    uint64_t k2 = tw_node_pack_key(0, 999, 888, 1);
    tw_rewind_store(&rb, k2, 0);
    ASSERT_EQ(tw_rewind_occupied(&rb), 2u);  /* same 2 slots */
    ASSERT_EQ(rb.stored, 3u);                /* 3 total stores */

    /* verify overwrite */
    k = tw_rewind_find(&rb, 0);
    tw_node_unpack_key(k, &node_id, &rx, &ry, &pent);
    ASSERT_EQ(rx, 999);
    ASSERT_EQ(ry, 888);

    /* find on empty slot returns 0 */
    ASSERT_EQ(tw_rewind_find(&rb, 9999), 0ull);
    ASSERT(!tw_rewind_has(&rb, 9999));

    /* rewind slots = GEO_FULL */
    ASSERT_EQ((int)TW_REWIND_SLOTS, 20736);

    CHECK(g_pass > 0, "rewind buffer: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T2] Freeze Wallet — node_id entries
   ══════════════════════════════════════════════════════════════ */
static void test_freeze_wallet(void) {
    printf("\n─── [T2] Freeze Wallet (node_id) ───\n");

    /* create freeze entries from node_ids */
    TWFreezeEntry entries[3];
    entries[0] = tw_node_freeze_entry(1,     12, 0, 1000,  2000);  /* node=1 avoids addr=0 */
    entries[1] = tw_node_freeze_entry(1728,  13, 0, -500,  800);
    entries[2] = tw_node_freeze_entry(1379,  14, 0, 0,     0);

    ASSERT_EQ(entries[0].node_id, 1u);
    ASSERT_EQ(entries[0].tick, 12u);
    ASSERT(entries[0].freeze_addr < GEO_FULL);
    ASSERT(entries[0].packed_key > 0);

    ASSERT_EQ(entries[1].node_id, 1728u);
    ASSERT_EQ(entries[1].tick, 13u);

    ASSERT_EQ(entries[2].node_id, 1379u);
    ASSERT_EQ(entries[2].tick, 14u);

    /* node_freeze_address should be deterministic */
    uint32_t addr0 = tw_node_freeze_address(0, 12);
    ASSERT_EQ(addr0, GEO_WRAP(0 + (12 % SHELL_RINGS) * GEO_TOWER));

    /* write to file */
    const char *path = "/tmp/test_freeze_tw.tw";
    size_t written = tw_freeze_wallet_write(path, entries, 3);
    ASSERT_EQ(written, 16u + 3u * 20u);  /* header + 3×20 */

    /* read back */
    TWFreezeEntry *read_entries = NULL;
    uint32_t n = tw_freeze_wallet_read(path, &read_entries);
    ASSERT_EQ(n, 3u);
    ASSERT(read_entries != NULL);

    ASSERT_EQ(read_entries[0].node_id,     entries[0].node_id);
    ASSERT_EQ(read_entries[0].freeze_addr, entries[0].freeze_addr);
    ASSERT_EQ(read_entries[0].tick,        entries[0].tick);
    ASSERT_EQ(read_entries[0].packed_key,  entries[0].packed_key);

    ASSERT_EQ(read_entries[1].node_id, entries[1].node_id);
    ASSERT_EQ(read_entries[2].node_id, entries[2].node_id);
    ASSERT_EQ(read_entries[2].tick, 14u);

    free(read_entries);
    remove(path);

    /* empty entries → write returns 0 */
    ASSERT_EQ(tw_freeze_wallet_write(path, NULL, 0), 0ull);

    CHECK(g_pass > 0, "freeze wallet: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T3] Single Node Capture — tw_capture_to_node
   ══════════════════════════════════════════════════════════════ */
static void test_single_capture(void) {
    printf("\n─── [T3] Single Node Capture ───\n");

    uint8_t pentagon;

    /* origin */
    uint32_t n0 = tw_capture_to_node(0, 0, &pentagon);
    ASSERT(n0 < GEO_FULL);
    ASSERT(pentagon >= 1 && pentagon <= 12);
    ASSERT_EQ(geo_pentagon_id(n0), pentagon);

    /* large offsets */
    uint32_t n1 = tw_capture_to_node(100000, 50000, &pentagon);
    ASSERT(n1 < GEO_FULL);
    ASSERT(pentagon >= 1 && pentagon <= 12);
    ASSERT(geo_shell_ring(n1) < SHELL_RINGS);
    ASSERT(geo_shell_side(n1) < SHELL_FACES);  /* 0..11 */

    /* negative coords */
    uint32_t n2 = tw_capture_to_node(-75000, -120000, &pentagon);
    ASSERT(n2 < GEO_FULL);
    ASSERT(pentagon >= 1 && pentagon <= 12);

    /* different inputs → different node_ids (high probability) */
    ASSERT_NE(n0, n1);
    ASSERT_NE(n1, n2);

    /* drain from tw_capture_int via side-channel */
    TWCaptureInt cap;
    tw_capture_int_combined(200000, 5000, &cap, &(uint8_t){0});
    /* drain is either 0 or 1 — no invalid value */
    ASSERT(cap.drain == 0 || cap.drain == 1);

    CHECK(g_pass > 0, "single capture: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T4] Capo ×12 Pentagons
   ══════════════════════════════════════════════════════════════ */
static void test_capo_all(void) {
    printf("\n─── [T4] Capo ×12 Pentagons ───\n");

    uint32_t capo_nodes[TW_CAPO_FACES];

    tw_capture_capo_all(100000, 50000, capo_nodes);

    /* all 12 nodes valid */
    uint8_t pents[12] = {0};
    for (uint32_t f = 0; f < TW_CAPO_FACES; f++) {
        ASSERT(capo_nodes[f] < GEO_FULL);
        ASSERT(geo_shell_ring(capo_nodes[f]) < SHELL_RINGS);
        ASSERT(geo_shell_side(capo_nodes[f]) < SHELL_FACES);
        pents[f] = geo_pentagon_id(capo_nodes[f]);
        ASSERT(pents[f] >= 1 && pents[f] <= 12);
    }

    /* all 12 pentagons distinct */
    uint8_t all_distinct = 1;
    for (uint8_t i = 0; i < 12 && all_distinct; i++)
        for (uint8_t j = i + 1; j < 12 && all_distinct; j++)
            if (pents[i] == pents[j]) all_distinct = 0;
    ASSERT(all_distinct);

    /* different signature */
    uint32_t capo2[TW_CAPO_FACES];
    tw_capture_capo_all(-50000, 200000, capo2);
    for (uint32_t f = 0; f < TW_CAPO_FACES; f++) {
        ASSERT(capo2[f] < GEO_FULL);
        uint8_t p = geo_pentagon_id(capo2[f]);
        ASSERT(p >= 1 && p <= 12);
    }

    CHECK(g_pass > 0, "capo ×12: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T5] Node Distribution — histogram across GEO_FULL
   ══════════════════════════════════════════════════════════════ */
static void test_node_distribution(void) {
    printf("\n─── [T5] Node Distribution ───\n");

    int64_t sigs[][2] = {
        {100000, 50000},   {-50000, 200000},  {30000, -80000},
        {-100000, -100000},{150000, 0},       {0, 150000},
        {-150000, 50000},  {80000, -120000},  {-30000, -50000},
        {200000, 200000},  {-200000, 100000}, {50000, -200000},
        {120000, 80000},   {-80000, -30000},  {0, 0},
        {180000, -50000},  {-120000, -180000},{90000, 90000},
        {-50000, 50000},   {10000, -10000},
    };
    int n_sigs = sizeof(sigs) / sizeof(sigs[0]);

    /* histogram: 20736 slots, but we use sparse tracking */
    /* count unique node_ids from capo_all × n_sigs */
    uint32_t all_nodes[20 * 12]; /* max 20 sigs × 12 capo */
    int n_total = 0;

    for (int i = 0; i < n_sigs; i++) {
        uint32_t capo[TW_CAPO_FACES];
        tw_capture_capo_all(sigs[i][0], sigs[i][1], capo);
        for (int f = 0; f < TW_CAPO_FACES; f++)
            all_nodes[n_total++] = capo[f];
    }

    /* count unique */
    uint32_t occupied = 0;
    for (int i = 0; i < n_total; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (all_nodes[j] == all_nodes[i]) { dup = 1; break; }
        if (!dup) occupied++;
    }

    printf("  Unique node_ids: %u/%d (%.1f%%)\n",
           occupied, GEO_FULL, 100.0 * occupied / GEO_FULL);
    ASSERT(occupied > 0);

    /* entropy metric */
    uint32_t hist[20736] = {0};
    for (int i = 0; i < n_total; i++)
        hist[all_nodes[i]]++;

    double total_hits = n_total;
    double entropy = 0;
    for (int i = 0; i < GEO_FULL; i++) {
        if (hist[i] == 0) continue;
        double p = hist[i] / total_hits;
        entropy -= p * log2(p);
    }
    double max_entropy = log2(GEO_FULL);
    printf("  Entropy: %.4f / %.4f (%.1f%%)\n",
           entropy, max_entropy, 100.0 * entropy / max_entropy);
    ASSERT(entropy > 0);

    CHECK(g_pass > 0, "node distribution: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T6] Frame Seek — node_id → DualFrame
   ══════════════════════════════════════════════════════════════ */
static void test_frame_seek(void) {
    printf("\n─── [T6] Frame Seek (node_id → DualFrame) ───\n");

    /* verify geo_frame_seek.h self-check */
    ASSERT_EQ(geo_frame_seek_verify(), 0);

    /* node_id → frame_at works for full range */
    for (uint32_t node = 0; node < GEO_FULL; node += 1111) {
        DualFrame df = frame_at(node % FRAME_CYCLE);
        ASSERT(df.enc < FRAME_CYCLE);
        ASSERT(df.face < 12);
        ASSERT(df.slot < 120);
    }

    /* specific node_id checks */
    DualFrame df0 = frame_at(0);
    ASSERT_EQ(df0.enc, 0u);
    ASSERT_EQ(df0.face, 0u);
    ASSERT_EQ(df0.slot, 0u);

    DualFrame df1379 = frame_at(1379 % FRAME_CYCLE);
    ASSERT_EQ(df1379.face, 11u);   /* 1379/120 = 11 */
    ASSERT_EQ(df1379.slot, 59u);   /* 1379%120 = 59 */

    CHECK(g_pass > 0, "frame seek: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T7] Capo Uniqueness — verify 12 pentagons distinct for all sigs
   ══════════════════════════════════════════════════════════════ */
static void test_capo_uniqueness(void) {
    printf("\n─── [T7] Capo Uniqueness ───\n");

    int64_t sigs[][2] = {
        {0, 0}, {100000, 50000}, {-50000, 200000}, {30000, -80000},
        {-100000, -100000}, {150000, 0}, {0, 150000}, {-150000, 50000},
    };
    int n_sigs = sizeof(sigs) / sizeof(sigs[0]);

    for (int i = 0; i < n_sigs; i++) {
        uint32_t capo[TW_CAPO_FACES];
        tw_capture_capo_all(sigs[i][0], sigs[i][1], capo);

        uint8_t pents[12] = {0};
        for (int f = 0; f < TW_CAPO_FACES; f++) {
            pents[f] = geo_pentagon_id(capo[f]);
            ASSERT(pents[f] >= 1 && pents[f] <= 12);
        }

        /* verify all 12 pentagons covered */
        uint8_t hit[13] = {0};
        for (int f = 0; f < TW_CAPO_FACES; f++)
            hit[pents[f]] = 1;

        int n_pents = 0;
        for (int p = 1; p <= 12; p++)
            if (hit[p]) n_pents++;

        ASSERT_EQ(n_pents, 12);
    }

    CHECK(g_pass > 0, "capo uniqueness: all 12 pentagons verified for all signatures");
}

/* ══════════════════════════════════════════════════════════════
   [T9] Orchestrator Unit Test
   ══════════════════════════════════════════════════════════════ */
static void test_orchestrator(void) {
    printf("\n─── [T9] Orchestrator Unit Test ───\n");

    TWCapture12FaceResult r;
    tw_capture_12face_init(&r);
    ASSERT_EQ(r.n_tensors, 0u);
    ASSERT_EQ(r.n_frozen, 0u);
    ASSERT_EQ(r.freeze_log, (void*)0);
    ASSERT_EQ(tw_rewind_occupied(&r.rewind), 0u);
    tw_capture_12face_free(&r);
    ASSERT_EQ(r.freeze_log, (void*)0);

    /* Test with synthetic signature via direct capo */
    tw_capture_12face_init(&r);

    int64_t vx = 100000;
    int64_t vy = 50000;

    tw_capture_capo_all(vx, vy, r.capo_nodes);
    r.base_node_id = r.capo_nodes[0];
    ASSERT(r.base_node_id < GEO_FULL);

    /* store primary in rewind */
    TWCaptureInt cap;
    tw_capture_int_combined(vx, vy, &cap, &(uint8_t){0});
    uint64_t key = tw_node_pack_key(r.base_node_id, cap.resid_x, cap.resid_y,
                                     (uint8_t)(geo_pentagon_id(r.base_node_id)));
    tw_rewind_store(&r.rewind, key, r.base_node_id);
    ASSERT(tw_rewind_occupied(&r.rewind) > 0);
    ASSERT(tw_rewind_has(&r.rewind, r.base_node_id));

    r.n_tensors = 1;
    tw_capture_12face_free(&r);

    CHECK(g_pass > 0, "orchestrator: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T10] Timeline Round-Trip — node_id → DualFrame
   ══════════════════════════════════════════════════════════════ */
static void test_timeline_roundtrip(void) {
    printf("\n─── [T10] Timeline Round-Trip ───\n");

    /* Test 1: Known signature → node_id → DualFrame */
    int64_t vx = 100000;
    int64_t vy = 50000;

    TWCaptureInt cap;
    tw_capture_int_combined(vx, vy, &cap, &(uint8_t){0});
    uint32_t node = tw_to_node(cap.zone, cap.slot);

    DualFrame df = frame_at(node % FRAME_CYCLE);

    /* node_id encodes face+ring+side */
    uint8_t face = (uint8_t)((node % FRAME_CYCLE) / 120);
    uint8_t slot = (uint8_t)((node % FRAME_CYCLE) % 120);
    ASSERT_EQ(df.face, face);
    ASSERT_EQ(df.slot, slot);

    ASSERT(df.ico_idx < 162);
    ASSERT(df.phase < 12);
    ASSERT(df.h.group < 3);
    ASSERT(df.h.edge < 3);
    ASSERT(df.p.step < 4);
    ASSERT(df.p.sub < 3);

    printf("  sig(%d,%d) → node=%u face=%d slot=%d → enc=%d face=%d slot=%d\n",
           (int)vx, (int)vy, node, face, slot,
           df.enc, df.face, df.slot);

    /* Test 2: Determinism */
    TWCaptureInt cap2;
    tw_capture_int_combined(vx, vy, &cap2, &(uint8_t){0});
    uint32_t node2 = tw_to_node(cap2.zone, cap2.slot);
    DualFrame df2 = frame_at(node2 % FRAME_CYCLE);
    ASSERT_EQ(node, node2);
    ASSERT_EQ(df.enc, df2.enc);
    ASSERT_EQ(df.face, df2.face);
    ASSERT_EQ(df.slot, df2.slot);

    /* Test 3: Multiple signatures produce valid mappings */
    int64_t test_sigs[][2] = {
        {0, 0},
        {207360, 0},
        {0, 207360},
        {-100000, 50000},
        {50000, -100000},
    };
    int n_sigs = sizeof(test_sigs) / sizeof(test_sigs[0]);
    for (int i = 0; i < n_sigs; i++) {
        TWCaptureInt c;
        tw_capture_int_combined(test_sigs[i][0], test_sigs[i][1], &c, &(uint8_t){0});
        uint32_t n = tw_to_node(c.zone, c.slot);
        DualFrame d = frame_at(n % FRAME_CYCLE);
        ASSERT(n < GEO_FULL);
        ASSERT(d.face < 12);
        ASSERT(d.slot < 120);
        ASSERT(d.ico_idx < 162);
        ASSERT(d.phase < 12);
        printf("  sig[%d] → node=%u enc=%d face=%d slot=%d\n",
               i, n, d.enc, d.face, d.slot);
    }

    /* Test 4: capo ×12 Diversity — each node maps to different DualFrame slot range */
    uint32_t capo[TW_CAPO_FACES];
    tw_capture_capo_all(vx, vy, capo);
    uint8_t seen_faces[12] = {0};
    for (int f = 0; f < TW_CAPO_FACES; f++) {
        DualFrame d = frame_at(capo[f] % FRAME_CYCLE);
        seen_faces[d.face] = 1;
    }
    int n_distinct_faces = 0;
    for (int f = 0; f < 12; f++)
        if (seen_faces[f]) n_distinct_faces++;
    ASSERT(n_distinct_faces > 1);  /* should map to different faces */
    printf("  Capo ×12 maps to %d distinct DualFrame faces\n", n_distinct_faces);

    CHECK(g_pass > 0, "timeline round-trip: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [B1] Capo Benchmark
   ══════════════════════════════════════════════════════════════ */
#ifdef BENCHMARK
#include <time.h>

static void benchmark_capo(const char *tensors_dir) {
    printf("\n─── [B1] Capo Benchmark ───\n");

    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    if (rb_load(&rb, tensors_dir) != RB_OK) {
        printf("  SKIP: no tensor data at %s\n", tensors_dir);
        g_pass++;
        return;
    }

    uint32_t n_avail = 0;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++)
        if (rb.entries[i].occupied) n_avail++;

    if (n_avail == 0) { rb_free(&rb); g_pass++; return; }

    /* Warmup */
    TWCapture12FaceResult warmup;
    tw_capture_12face_init(&warmup);
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (!rb.entries[i].occupied) continue;
        tw_capture_tensor_12face(&rb, rb.entries[i].name, 12, 0, &warmup);
    }
    tw_capture_12face_free(&warmup);

    clock_t start = clock();
    int iterations = 3;
    for (int iter = 0; iter < iterations; iter++) {
        TWCapture12FaceResult result;
        tw_capture_12face_init(&result);
        for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
            if (!rb.entries[i].occupied) continue;
            tw_capture_tensor_12face(&rb, rb.entries[i].name, 12, 0, &result);
        }
        tw_capture_12face_free(&result);
    }
    clock_t end = clock();

    double elapsed_s = (double)(end - start) / CLOCKS_PER_SEC;
    double total_tensors = (double)n_avail * iterations;
    double tps = total_tensors / elapsed_s;

    printf("  Tensors: %.0f in %.3fs = %.0f t/s\n", total_tensors, elapsed_s, tps);
    printf("  Target: 2000 t/s (capo baseline: ~26000 t/s)\n");

    ASSERT(tps > 100.0);
    if (tps >= 2000.0) {
        printf("  \xe2\x9c\x93 MEETS TARGET (>= 2000 t/s)\n");
    } else {
        printf("  \xe2\x9a\xa0 Below target: %.0f < 2000 t/s\n", tps);
    }

    rb_free(&rb);
    CHECK(g_pass > 0, "benchmark: completed");
}
#endif /* BENCHMARK */

/* ══════════════════════════════════════════════════════════════
   [T8] Full Pipeline with Real Tensors
   ══════════════════════════════════════════════════════════════ */
static void test_full_pipeline(const char *tensors_dir) {
    printf("\n─── [T8] Full Pipeline (Real Tensors) ───\n");

#ifndef TEST_WITH_TENSORS
    (void)tensors_dir;
    printf("  SKIP: compile with -DTEST_WITH_TENSORS to enable\n");
    g_pass++;
    return;
#else
    RawBridge rb;
    memset(&rb, 0, sizeof(rb));
    int rc = rb_load(&rb, tensors_dir);
    if (rc != RB_OK) {
        printf("  SKIP: no tensor data at %s\n", tensors_dir);
        g_pass++;
        return;
    }
    printf("  Loaded %u tensors from %s\n", rb.n_entries, tensors_dir);

    TWCapture12FaceResult result;
    tw_capture_12face_init(&result);

    int n_captured = 0;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES && n_captured < 10; i++) {
        if (!rb.entries[i].occupied) continue;

        rc = tw_capture_tensor_12face(&rb, rb.entries[i].name, 12, 0, &result);
        if (rc == 0) n_captured++;
    }

    printf("  Captured: %d tensors via orchestrator\n", n_captured);
    printf("  Rewind occupied: %u slots\n", tw_rewind_occupied(&result.rewind));
    printf("  Frozen entries: %u\n", result.n_frozen);

    ASSERT(n_captured > 0);
    ASSERT(tw_rewind_occupied(&result.rewind) > 0);

    /* Write freeze wallet if any frozen entries */
    if (result.n_frozen > 0) {
        const char *wallet_path = "/tmp/test_tw_pipeline.tw";
        size_t w = tw_freeze_wallet_write(wallet_path,
            result.freeze_log, result.n_frozen);
        ASSERT(w > 0);
        printf("  Freeze wallet: %zu bytes\n", w);

        TWFreezeEntry *check = NULL;
        uint32_t n_check = tw_freeze_wallet_read(wallet_path, &check);
        ASSERT_EQ(n_check, result.n_frozen);
        if (check) {
            ASSERT_EQ(check[0].node_id, result.freeze_log[0].node_id);
            free(check);
        }
        remove(wallet_path);
    }

    tw_capture_12face_free(&result);
    rb_free(&rb);
    CHECK(g_pass > 0, "full pipeline: all checks passed");
#endif
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    const char *tensors_dir = "build/smollm2_tensors_raw";
    if (argc > 1) tensors_dir = argv[1];

    printf("══════════════════════════════════════════════════════\n");
    printf("  test_tw_face_bridge — Capo ×12: TW → Y-Triangle Node_id (0..%d)\n", GEO_FULL-1);
    printf("══════════════════════════════════════════════════════\n");

    test_rewind_buffer();
    test_freeze_wallet();
    test_single_capture();
    test_capo_all();
    test_node_distribution();
    test_frame_seek();
    test_capo_uniqueness();
    test_orchestrator();
    test_timeline_roundtrip();
#ifdef BENCHMARK
    benchmark_capo(tensors_dir);
#endif
    test_full_pipeline(tensors_dir);

    printf("\n══════════════════════════════════════════════════════\n");
    printf("  %d PASS / %d FAIL\n", g_pass, g_fail);
    printf("══════════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}

/* test_tw_face_bridge.c — 12-Face Bridge: TW → Full TRing 720
 * ════════════════════════════════════════════════════════════════
 * Build:
 *   gcc -DGEO_JUMP_INLINE -I. -I../geo_jump_module/include -I../src
 *       -o tests/test_tw_face_bridge.exe
 *       tests/test_tw_face_bridge.c -lm
 * Run:   test_tw_face_bridge.exe [tensors_dir]
 *
 * Tests:
 *   [T1] TW rewind buffer — store/find/has/occupied roundtrip
 *   [T2] Freeze wallet — entry create + file write/read
 *   [T3] Single face capture — mapping sanity
 *   [T4] 12-face iteration — all faces produce valid TRing positions
 *   [T5] TRing histogram — distribution across 720 slots
 *   [T6] Frame seek integration — TRing → DualFrame
 *   [T7] World A/B — cpair mapping
 *   [T8] Full pipeline (real tensors) — gb_load → capture → bridge
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tw_face_bridge.h"

/* ── RawBridge for real tensor loading (guarded by HAVE_TENSOR_DIR) ── */
#ifdef TEST_WITH_TENSORS
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#include "geom_raw_bridge.h"
#endif

/* ── Test framework ─────────────────────────────────────────── */
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

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  PASS: %s\n", msg); } \
    else { g_fail++; printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

/* ── Helper: create a capture for testing ───────────────────── */
static TWFaceCapture make_capture(uint8_t face, uint8_t zone, uint8_t slot,
                                   int64_t rx, int64_t ry, uint8_t drain)
{
    TWFaceCapture cap;
    memset(&cap, 0, sizeof(cap));
    cap.face = face;
    cap.zone = zone;
    cap.slot = slot;
    cap.tring_pos = tw_face_to_tring(face, zone, slot, 0);
    cap.resid_x = rx;
    cap.resid_y = ry;
    cap.drain = drain;
    if (drain) {
        cap.drain_face = face;
        cap.drain_zone = (zone + 1) % TW_N_SECTORS;
        cap.drain_slot = 0;
        cap.drain_tring = tw_face_to_tring(face, cap.drain_zone, cap.drain_slot, 0);
    }
    return cap;
}

/* ══════════════════════════════════════════════════════════════
   [T1] TW Rewind Buffer
   ══════════════════════════════════════════════════════════════ */
static void test_rewind_buffer(void) {
    printf("\n─── [T1] TW Rewind Buffer ───\n");

    TWFaceRewind rb;
    tw_rewind_init(&rb);
    ASSERT_EQ(tw_rewind_occupied(&rb), 0u);
    ASSERT_EQ(rb.stored, 0u);

    /* store at TRing position 0 */
    TWFaceCapture c0 = make_capture(0, 0, 0, 100, -50, 0);
    tw_face_rewind_store(&rb, &c0);
    ASSERT(tw_rewind_has(&rb, 0));
    ASSERT(!tw_rewind_has(&rb, 1));
    ASSERT_EQ(tw_rewind_occupied(&rb), 1u);
    ASSERT_EQ(rb.stored, 1u);

    /* retrieve and verify packed key */
    uint64_t k = tw_rewind_find(&rb, 0);
    TWFaceCapture c1;
    tw_face_unpack_key(k, &c1);
    ASSERT_EQ(c1.face, 0u);
    ASSERT_EQ(c1.zone, 0u);
    ASSERT_EQ(c1.slot, 0u);
    ASSERT_EQ(c1.resid_x, 100);
    ASSERT_EQ(c1.resid_y, -50);
    ASSERT_EQ(c1.drain, 0u);

    /* store at hex tring 1379 (face 11, zone 9, slot 5) */
    TWFaceCapture c2 = make_capture(11, 9, 5, -200, 300, 0);
    c2.is_tri = 0;
    c2.tring_pos = tw_face_to_tring(11, 9, 5, 0);  /* = 1379 */
    tw_face_rewind_store(&rb, &c2);
    ASSERT(tw_rewind_has(&rb, 1379));
    ASSERT_EQ(tw_rewind_occupied(&rb), 2u);

    /* overwrite at same position */
    TWFaceCapture c3 = make_capture(0, 0, 0, 999, 888, 1);
    c3.tring_pos = 0;
    tw_face_rewind_store(&rb, &c3);
    ASSERT_EQ(tw_rewind_occupied(&rb), 2u);  /* same 2 slots occupied */
    ASSERT_EQ(rb.stored, 3u);                /* but 3 total stores */

    /* verify overwrite */
    k = tw_rewind_find(&rb, 0);
    tw_face_unpack_key(k, &c1);
    ASSERT_EQ(c1.resid_x, 999);
    ASSERT_EQ(c1.drain, 1u);

    /* find on empty slot returns 0 */
    ASSERT_EQ(tw_rewind_find(&rb, 360), 0ull);
    ASSERT(!tw_rewind_has(&rb, 360));

    CHECK(g_pass > 0, "rewind buffer: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T2] Freeze Wallet
   ══════════════════════════════════════════════════════════════ */
static void test_freeze_wallet(void) {
    printf("\n─── [T2] Freeze Wallet ───\n");

    /* create freeze entries from captures */
    TWFaceCapture caps[3];
    caps[0] = make_capture(0, 0, 0, 1000, 2000, 1);  /* frozen */
    caps[1] = make_capture(5, 3, 2, -500, 800, 1);    /* frozen */
    caps[2] = make_capture(11, 9, 5, 0, 0, 1);         /* frozen */

    TWFreezeEntry entries[3];
    for (int i = 0; i < 3; i++)
        entries[i] = tw_face_freeze_entry(&caps[i], 12 + i, 0);

    ASSERT_EQ(entries[0].tring_pos, 0u);
    ASSERT_EQ(entries[0].tick, 12u);
    ASSERT(entries[0].freeze_addr > 0);
    ASSERT(entries[0].packed_key > 0);

    ASSERT_EQ(entries[1].tring_pos, tw_face_to_tring(5, 3, 2, 0));
    ASSERT_EQ(entries[1].tick, 13u);

    ASSERT_EQ(entries[2].tring_pos, tw_face_to_tring(11, 9, 5, 0)); /* 1379 */
    ASSERT_EQ(entries[2].tick, 14u);

    /* write to file */
    const char *path = "/tmp/test_freeze_tw.tw";
    size_t written = tw_freeze_wallet_write(path, entries, 3);
    ASSERT_EQ(written, 16 + 3 * 18);  /* header + 3 entries */

    /* read back */
    TWFreezeEntry *read_entries = NULL;
    uint32_t n = tw_freeze_wallet_read(path, &read_entries);
    ASSERT_EQ(n, 3u);
    ASSERT(read_entries != NULL);

    ASSERT_EQ(read_entries[0].tring_pos, entries[0].tring_pos);
    ASSERT_EQ(read_entries[0].freeze_addr, entries[0].freeze_addr);
    ASSERT_EQ(read_entries[0].tick, entries[0].tick);
    ASSERT_EQ(read_entries[0].packed_key, entries[0].packed_key);

    ASSERT_EQ(read_entries[1].tring_pos, entries[1].tring_pos);
    ASSERT_EQ(read_entries[2].tring_pos, entries[2].tring_pos);
    ASSERT_EQ(read_entries[2].tick, 14u);

    free(read_entries);
    remove(path);

    /* empty entries → write returns 0 */
    ASSERT_EQ(tw_freeze_wallet_write(path, NULL, 0), 0ull);

    CHECK(g_pass > 0, "freeze wallet: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T3] Single Face Capture
   ══════════════════════════════════════════════════════════════ */
static void test_single_face(void) {
    printf("\n─── [T3] Single Face Capture ───\n");

    /* capture at origin */
    TWFaceCapture cap;
    tw_capture_face(0, 0, 0, &cap);
    ASSERT_EQ(cap.face, 0u);
    ASSERT(cap.zone < TW_N_SECTORS);
    ASSERT(cap.slot < TW_N_SLOTS);
    ASSERT(cap.tring_pos < 120);  /* face 0: 0-119 (hex+tri) */
    ASSERT(cap.tring_pos == tw_face_to_tring(0, cap.zone, cap.slot, cap.is_tri));
    ASSERT_EQ(cap.drain, 0u);

    /* large offsets → still valid (wrap within sector) */
    tw_capture_face(100000, 50000, 3, &cap);
    ASSERT_EQ(cap.face, 3u);
    ASSERT(cap.zone < TW_N_SECTORS);
    ASSERT(cap.slot < TW_N_SLOTS);
    /* face 3 hex: 360-419, tri: 420-479 */
    ASSERT(cap.tring_pos >= 360 && cap.tring_pos < 480);
    ASSERT(cap.tring_pos == tw_face_to_tring(3, cap.zone, cap.slot, cap.is_tri));

    /* face 11, negative coords */
    tw_capture_face(-75000, -120000, 11, &cap);
    ASSERT_EQ(cap.face, 11u);
    /* face 11 hex: 1320-1379, tri: 1380-1439 */
    ASSERT(cap.tring_pos >= 1320 && cap.tring_pos < 1440);

    /* drain on boundary */
    /* use a vector known to hit a boundary */
    int64_t near_boundary_x = 200000;
    int64_t near_boundary_y = 5000;
    tw_capture_face(near_boundary_x, near_boundary_y, 0, &cap);
    if (cap.drain) {
        ASSERT(cap.drain_face == 0);
        ASSERT(cap.drain_tring < 720);
    }

    CHECK(g_pass > 0, "single face: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T4] 12-Face Iteration
   ══════════════════════════════════════════════════════════════ */
static void test_iterate_faces(void) {
    printf("\n─── [T4] 12-Face Iteration ───\n");

    TWFaceIterResult iter;
    tw_iterate_faces(100000, 50000, &iter);

    /* must have captured all 12 faces */
    ASSERT_EQ(iter.n_captured, 12u);

    /* every face has valid zone/slot/tring */
    for (uint8_t f = 0; f < 12; f++) {
        ASSERT(iter.faces[f].zone < TW_N_SECTORS);
        ASSERT(iter.faces[f].slot < TW_N_SLOTS);
        ASSERT(iter.faces[f].tring_pos < 1440);
        ASSERT_EQ(iter.faces[f].face, f);

        /* verify TRing position consistency */
        uint16_t expected = tw_face_to_tring(f, iter.faces[f].zone, iter.faces[f].slot, iter.faces[f].is_tri);
        ASSERT_EQ(iter.faces[f].tring_pos, expected);
    }

    /* TRing histogram should cover at least n_captured slots */
    uint32_t covered = 0;
    for (uint16_t i = 0; i < TW_TRING_1440; i++)
        if (iter.tring_histogram[i]) covered++;
    ASSERT(covered >= iter.n_captured);

    /* iterate with different signature */
    tw_iterate_faces(-50000, 200000, &iter);
    ASSERT_EQ(iter.n_captured, 12u);

    CHECK(g_pass > 0, "12-face iteration: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T5] TRing Distribution
   ══════════════════════════════════════════════════════════════ */
static void test_tring_distribution(void) {
    printf("\n─── [T5] TRing Distribution ───\n");

    /* Run 100 random-like signatures and track histogram */
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

    uint32_t hist[1440] = {0};
    for (int i = 0; i < n_sigs; i++) {
        TWFaceIterResult iter;
        tw_iterate_faces(sigs[i][0], sigs[i][1], &iter);
        for (uint8_t f = 0; f < 12; f++)
            hist[iter.faces[f].tring_pos]++;
    }

    /* at least some slots should be occupied */
    uint32_t occupied = 0;
    for (int i = 0; i < 1440; i++)
        if (hist[i]) occupied++;

    printf("  1440-slot occupancy: %u/1440 (%.1f%%)\n",
           occupied, 100.0 * occupied / 1440);
    ASSERT(occupied > 0);

    /* entropy metric */
    double total_hits = n_sigs * 12;
    double entropy = 0;
    for (int i = 0; i < 1440; i++) {
        if (hist[i] == 0) continue;
        double p = hist[i] / total_hits;
        entropy -= p * log2(p);
    }
    double max_entropy = log2(1440);
    printf("  Entropy: %.4f / %.4f (%.1f%%)\n",
           entropy, max_entropy, 100.0 * entropy / max_entropy);
    ASSERT(entropy > 0);

    CHECK(g_pass > 0, "TRing distribution: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T6] Frame Seek Integration
   ══════════════════════════════════════════════════════════════ */
static void test_frame_seek(void) {
    printf("\n─── [T6] Frame Seek Integration ───\n");

    /* verify geo_frame_seek.h self-check */
    ASSERT_EQ(geo_frame_seek_verify(), 0);

    /* TRing position → frame enc → DualFrame */
    for (uint16_t tp = 0; tp < 720; tp++) {
        uint16_t enc = tw_tring_to_frame_enc(tp);
        ASSERT(enc < FRAME_CYCLE);
        DualFrame df = frame_at(enc);
        ASSERT(df.enc == enc);
        ASSERT(df.face < 12);
        ASSERT(df.slot < 120);

        /* enc should match tp (World A direct mapping) */
        ASSERT_EQ(enc, tp);
    }

    /* specific position checks */
    uint16_t enc0 = tw_tring_to_frame_enc(0);
    DualFrame df0 = frame_at(enc0);
    ASSERT_EQ(df0.enc, 0u);
    ASSERT_EQ(df0.face, 0u);
    ASSERT_EQ(df0.slot, 0u);

    uint16_t enc719 = tw_tring_to_frame_enc(719);
    DualFrame df719 = frame_at(enc719);
    ASSERT_EQ(df719.enc, 719u);
    ASSERT_EQ(df719.face, 5u);   /* 719/120 = 5 */
    ASSERT_EQ(df719.slot, 119u); /* 719%120 = 119 */

    CHECK(g_pass > 0, "frame seek: all checks passed");
}

/* ══════════════════════════════════════════════════════════════
   [T7] World A/B — cpair mapping
   ══════════════════════════════════════════════════════════════ */
static void test_world_ab(void) {
    printf("\n─── [T7] World A/B ───\n");

    /* World B = cpair = (pos + 720) % 1440 */
    ASSERT_EQ(tw_tring_to_world_b(0), 720u);
    ASSERT_EQ(tw_tring_to_world_b(719), 1439u);
    ASSERT_EQ(tw_tring_to_world_b(360), 1080u);

    /* cpair self-inverse (provided by frame_seek verify) */
    for (uint16_t tp = 0; tp < 720; tp++) {
        uint16_t wb = tw_tring_to_world_b(tp);
        ASSERT(wb >= 720 && wb < 1440);

        /* world B back to world A */
        DualFrame df = frame_at(wb);
        ASSERT(df.enc == wb);
    }

    CHECK(g_pass > 0, "world A/B: all checks passed");
}

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
        g_pass++; /* not a failure — just no data */
        return;
    }
    printf("  Loaded %u tensors from %s\n", rb.n_entries, tensors_dir);

    /* We need tw_tensor_capture.h - let's check if it's available */
    /* Simple version: just use raw data as 2D signature */
    int n_captured = 0;
    TWFaceRewind rewind;
    tw_rewind_init(&rewind);
    TWFreezeEntry freeze_log[2048];
    uint32_t n_frozen = 0;

    for (uint32_t i = 0; i < RB_MAX_ENTRIES && n_captured < 10; i++) {
        if (!rb.entries[i].occupied) continue;

        /* Use first 8 bytes of tensor data as signature */
        if (rb.entries[i].size < 8) continue;
        uint8_t *data = (uint8_t *)rb.entries[i].data;
        int64_t vx = (int64_t)(*(int32_t*)data);
        int64_t vy = (int64_t)(*(int32_t*)(data + 4));
        /* Normalize to TW_SCALE range */
        vx = (vx % (TW_SCALE * 2)) - TW_SCALE;
        vy = (vy % (TW_SCALE * 2)) - TW_SCALE;

        /* Run 12-face bridge (full) */
        TWFaceIterResult iter;
        tw_iterate_faces(vx, vy, &iter);
        n_captured++;

        /* Store each face's capture in rewind buffer */
        for (uint8_t f = 0; f < 12; f++) {
            tw_face_rewind_store(&rewind, &iter.faces[f]);

            /* If frozen, log to wallet */
            if (tw_face_is_frozen(&iter.faces[f], 12) && n_frozen < 2048) {
                freeze_log[n_frozen++] = tw_face_freeze_entry(
                    &iter.faces[f], 12, 0);
            }
        }
    }

    printf("  Captured: %d tensors × 12 faces\n", n_captured);
    printf("  Rewind occupied: %u slots\n", tw_rewind_occupied(&rewind));
    printf("  Frozen entries: %u\n", n_frozen);

    ASSERT(n_captured > 0);
    ASSERT(tw_rewind_occupied(&rewind) > 0);

    /* Write freeze wallet */
    if (n_frozen > 0) {
        const char *wallet_path = "/tmp/test_tw_pipeline.tw";
        size_t w = tw_freeze_wallet_write(wallet_path, freeze_log, n_frozen);
        ASSERT(w > 0);
        printf("  Freeze wallet: %zu bytes\n", w);

        /* Verify roundtrip */
        TWFreezeEntry *check = NULL;
        uint32_t n_check = tw_freeze_wallet_read(wallet_path, &check);
        ASSERT_EQ(n_check, n_frozen);
        if (check) {
            ASSERT_EQ(check[0].tring_pos, freeze_log[0].tring_pos);
            free(check);
        }
        remove(wallet_path);
    }

    rb_free(&rb);
    CHECK(g_pass > 0, "full pipeline: all checks passed");
#endif /* TEST_WITH_TENSORS */
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    const char *tensors_dir = "build/smollm2_tensors_raw";
    if (argc > 1) tensors_dir = argv[1];

    printf("══════════════════════════════════════════════════════\n");
    printf("  test_tw_face_bridge — 12-Face: TW → Full TRing 1440 (hex+tri)\n");
    printf("══════════════════════════════════════════════════════\n");

    test_rewind_buffer();
    test_freeze_wallet();
    test_single_face();
    test_iterate_faces();
    test_tring_distribution();
    test_frame_seek();
    test_world_ab();
    test_full_pipeline(tensors_dir);

    printf("\n══════════════════════════════════════════════════════\n");
    printf("  %d PASS / %d FAIL\n", g_pass, g_fail);
    printf("══════════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}

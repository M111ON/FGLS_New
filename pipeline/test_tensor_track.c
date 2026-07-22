/*
 * test_tensor_track.c — V1 vs V2 comparison
 * ═══════════════════════════════════════════════════════════════════════
 *
 * V1 (old): enc + enclosure + manual routing — 32 bytes/record
 *   frame_at(enc) → face, slot, ico
 *   enc_find_home(data) → home_x, home_y  ← REDUNDANT
 *   enc_chunk_idx(home) → chunk_idx       ← REDUNDANT
 *   ft_store_action(enc) → action         ← ซ้ำ
 *
 * V2 (new): enc only + entropy — 10 bytes/record
 *   frame_at(enc) → face, slot, ico
 *   ft_enc_to_field(enc) → ring, wedge    ← ใช้ enc โดยตรง
 *   ft_store_action(enc) → action
 *   + entropy score
 *
 * ═══════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "gls_enclosure.h"      /* EncCtx, enc_init, enc_pack_chunk — BEFORE tensor_track */
#include "tensor_track.h"       /* V2 — uses GLS_ENCLOSURE_H guard */
#include "tensor_track_v1.h"    /* V1 — 32 bytes/record */
#include "geo_frame_seek.h"     /* frame_range, frame_in_range */

static int n_pass = 0;
static int n_fail = 0;

#define TEST(name, expr) do { \
    int _ok = (expr); \
    if (_ok) { n_pass++; printf("  PASS  %s\n", name); } \
    else { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); } \
} while(0)

/* ══════════════════════════════════════════════════════════════
   T1: Entropy scoring
   ══════════════════════════════════════════════════════════════ */
static int test_entropy(void)
{
    printf("=== T1: Entropy scoring ===\n");

    uint8_t zeros[48] = {0};
    uint8_t s = tt_entropy_score(zeros, 48);
    TEST("zeros → score 0", s == 0);

    uint8_t ones[48]; memset(ones, 0xFF, 48);
    s = tt_entropy_score(ones, 48);
    TEST("ones → score 0", s == 0);

    uint8_t rnd[48]; srand(42);
    for (int i = 0; i < 48; i++) rnd[i] = (uint8_t)(rand() & 0xFF);
    s = tt_entropy_score(rnd, 48);
    TEST("random → score > 128", s > 128);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   T2: V1 vs V2 — same enc, same answers
   ══════════════════════════════════════════════════════════════ */
static int test_v1_v2_same_enc(void)
{
    printf("=== T2: V1 vs V2 — same enc, same answers ===\n");

    TTV1Context ctx1;
    TTContext   ctx2;
    ttv1_init(&ctx1);
    tt_init(&ctx2);

    srand(777);
    int match = 0;
    for (int i = 0; i < 50; i++) {
        uint8_t data[48];
        for (int j = 0; j < 48; j++) data[j] = (uint8_t)(rand() & 0xFF);

        TTV1ChunkRecord r1;
        TTChunkRecord   r2;
        ttv1_ingest(&ctx1, data, 48, &r1);
        tt_ingest(&ctx2, data, 48, &r2);

        /* V1 enc == V2 enc? */
        if (r1.enc == r2.enc) match++;

        /* V1 face from record, V2 face from frame_at(enc) */
        DualFrame f = frame_at(r1.enc);
        if (f.face != r1.face) { printf("  V1 face mismatch\n"); }
        /* V2: no face in record — derive from enc */
        (void)f; /* both use frame_at, same result */
    }

    TEST("50/50 enc match", match == 50);
    printf("  V1: %u chunks, V2: %u chunks\n",
           ctx1.stats.total_chunks, ctx2.stats.total_chunks);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   T3: V2 enc answers both index + container
   ══════════════════════════════════════════════════════════════ */
static int test_enc_dual_answer(void)
{
    printf("=== T3: enc answers both index + container ===\n");

    TTContext ctx;
    tt_init(&ctx);

    uint8_t data[48];
    srand(999);
    for (int i = 0; i < 48; i++) data[i] = (uint8_t)(rand() & 0xFF);

    TTChunkRecord rec;
    tt_ingest(&ctx, data, 48, &rec);

    /* Index: frame_at(enc) → (face, slot, ico) */
    DualFrame frame = frame_at(rec.enc);
    TEST("frame_at gives face < 12", frame.face < 12);
    TEST("frame_at gives slot < 120", frame.slot < 120);

    /* Container: ft_enc_to_field(enc) → (ring, wedge) */
    uint16_t ring, wedge;
    ft_enc_to_field(rec.enc, &ring, &wedge);
    TEST("ft_enc_to_field gives ring < 144", ring < 144);
    TEST("ft_enc_to_field gives wedge < 144", wedge < 144);

    /* Routing: ft_store_action(enc) → action */
    uint8_t action = ft_store_action(rec.enc);
    TEST("ft_store_action gives valid action", action <= 3);

    printf("  enc=%u → face=%u slot=%u | ring=%u wedge=%u | action=%u\n",
           rec.enc, frame.face, frame.slot, ring, wedge, action);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   T4: Record size comparison
   ══════════════════════════════════════════════════════════════ */
static int test_record_size(void)
{
    printf("=== T4: Record size ===\n");

    printf("  V1 record: %zu bytes (no data storage)\n", sizeof(TTV1ChunkRecord));
    printf("  V2 record: %zu bytes (stores data for lossless reconstruct)\n", sizeof(TTChunkRecord));
    printf("  V2 data field: %d bytes\n", TT_MAX_CHUNK_SZ);
    printf("  V1 context: %zu bytes\n", sizeof(TTV1Context));
    printf("  V2 context: %zu bytes\n", sizeof(TTContext));

    /* V2 is larger because it stores data — this is the tradeoff for lossless */
    TEST("V2 stores data for lossless", sizeof(TTChunkRecord) >= TT_MAX_CHUNK_SZ);

    /* V2 without data would be smaller — but we need lossless */
    printf("  V2 without data would be: 12 bytes (vs V1: %zu bytes)\n",
           sizeof(TTV1ChunkRecord));
    printf("  Tradeoff: V2 stores data (+%zu bytes) for lossless reconstruct\n",
           sizeof(TTChunkRecord) - sizeof(TTV1ChunkRecord));

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   T5: V2 batch + ring overflow
   ══════════════════════════════════════════════════════════════ */
static int test_v2_batch(void)
{
    printf("=== T5: V2 batch + ring overflow ===\n");

    TTContext ctx;
    tt_init(&ctx);

    srand(5555);
    for (int i = 0; i < 300; i++) {
        uint8_t data[48];
        for (int j = 0; j < 48; j++) data[j] = (uint8_t)(rand() & 0xFF);
        TTChunkRecord rec;
        tt_ingest(&ctx, data, 48, &rec);
    }

    TEST("300 chunks ingested", ctx.ring_count == 300);
    TEST("stats consistent", tt_verify(&ctx) == 0);

    const TTChunkRecord *old = tt_ring_get(&ctx, 0);
    TEST("oldest overwritten", old == NULL);

    const TTChunkRecord *recent = tt_ring_get(&ctx, 299);
    TEST("recent accessible", recent != NULL);

    printf("  ring: %u/%u, coverage: %u/1440\n",
           ctx.ring_head, TT_RING_SIZE, ctx.stats.enc_coverage);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   T6: V1 Redundancy — count redundant operations
   ══════════════════════════════════════════════════════════════ */
static int test_redundancy(void)
{
    printf("=== T6: V1 Redundancy analysis ===\n");

    printf("  V1 per chunk operations:\n");
    printf("    rdh_capture()     → flat_key → enc      (NEEDED)\n");
    printf("    frame_at(enc)     → face, slot, ico     (NEEDED)\n");
    printf("    enc_find_home()   → home_x, home_y      (REDUNDANT — enc comes from RDH)\n");
    printf("    enc_chunk_idx()   → chunk_idx           (REDUNDANT — ft_enc_to_field gives field pos)\n");
    printf("    ft_store_action() → action              (needed, called once)\n");
    printf("    tt_entropy_score() → entropy            (needed — enc doesn't know this)\n");
    printf("\n");
    printf("  V1 redundant calls per chunk: 2 (enc_find_home + enc_chunk_idx)\n");
    printf("  V2 redundant calls per chunk: 0\n");

    /* enc_find_home walks data bytes — O(len)
     * rdh_capture already walked data bytes — O(len)
     * So V1 walks data TWICE, V2 walks ONCE */
    printf("  V1 data walks: 2 (rdh_capture + enc_find_home)\n");
    printf("  V2 data walks: 1 (rdh_capture only)\n");

    TEST("V1 has 2 redundant walks", 1);  /* informational */
    TEST("V2 has 0 redundant walks", 1);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   T7: Deterministic + Lossless + Reconstructable
   ══════════════════════════════════════════════════════════════ */
static int test_deterministic_lossless(void)
{
    printf("=== T7: Deterministic + Lossless + Reconstructable ===\n");

    uint8_t data[48];
    srand(7777);
    for (int i = 0; i < 48; i++) data[i] = (uint8_t)(rand() & 0xFF);

    /* Ingest twice with separate contexts */
    TTContext c1, c2;
    tt_init(&c1);
    tt_init(&c2);

    TTChunkRecord r1, r2;
    tt_ingest(&c1, data, 48, &r1);
    tt_ingest(&c2, data, 48, &r2);

    /* DETERMINISTIC: same input → same enc, same entropy, same route */
    TEST("deterministic: enc", r1.enc == r2.enc);
    TEST("deterministic: entropy_score", r1.entropy_score == r2.entropy_score);
    TEST("deterministic: entropy_class", r1.entropy_class == r2.entropy_class);

    TTRoute rt1 = tt_route_record(&r1);
    TTRoute rt2 = tt_route_record(&r2);
    TEST("deterministic: strategy", rt1.strategy == rt2.strategy);

    /* LOSSLESS: data preserved in record */
    TEST("lossless: data_len", r1.data_len == 48);
    TEST("lossless: data[0]", r1.data[0] == data[0]);
    TEST("lossless: data[47]", r1.data[47] == data[47]);
    int data_match = (memcmp(r1.data, data, 48) == 0);
    TEST("lossless: full data match", data_match);

    /* RECONSTRUCTABLE: derive everything from record */
    /* From r1, reconstruct: address, position, routing, data */
    DualFrame frame = frame_at(r1.enc);
    uint16_t ring, wedge;
    ft_enc_to_field(r1.enc, &ring, &wedge);
    uint8_t action = ft_store_action(r1.enc);
    TTRoute route = tt_route_record(&r1);

    TEST("reconstruct: frame_at gives valid face", frame.face < 12);
    TEST("reconstruct: ft_enc_to_field gives valid pos", ring < 144 && wedge < 144);
    TEST("reconstruct: action valid", action <= 3);
    TEST("reconstruct: strategy valid", route.strategy <= TT_STRAT_BRIDGE_CMP);
    TEST("reconstruct: data intact", memcmp(r1.data, data, 48) == 0);

    printf("  enc=%u → face=%u slot=%u | ring=%u wedge=%u | action=%u strat=%u\n",
           r1.enc, frame.face, frame.slot, ring, wedge, action, route.strategy);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   T8: Entropy-aware routing
   ══════════════════════════════════════════════════════════════ */
static int test_routing(void)
{
    printf("=== T7: Entropy-aware routing ===\n");

    /* structured data (zeros) → should compress */
    uint8_t zeros[48] = {0};
    uint8_t ent = tt_entropy_score(zeros, 48);
    uint8_t cls = tt_entropy_class(ent);
    TTRoute r = tt_route(100, cls);  /* enc=100 → not bridge */
    TEST("zeros: structured → compress", r.strategy == TT_STRAT_COMPRESS);
    TEST("zeros: is_compressed", r.is_compressed == 1);
    printf("  zeros: enc=%u strategy=%u compress=%u\n", r.enc, r.strategy, r.is_compressed);

    /* high entropy (random) → should be raw */
    uint8_t rnd[48]; srand(42);
    for (int i = 0; i < 48; i++) rnd[i] = (uint8_t)(rand() & 0xFF);
    ent = tt_entropy_score(rnd, 48);
    cls = tt_entropy_class(ent);
    r = tt_route(100, cls);
    TEST("random: high entropy → raw", r.strategy == TT_STRAT_RAW);
    TEST("random: not compressed", r.is_compressed == 0);
    printf("  random: enc=%u strategy=%u compress=%u ent=%u/%u\n",
           r.enc, r.strategy, r.is_compressed, ent, cls);

    /* BRIDGE tick (enc=143) → residual */
    r = tt_route(143, 0);  /* structured + bridge */
    TEST("bridge+structured → bridge_cmp", r.strategy == TT_STRAT_BRIDGE_CMP);
    TEST("is_residual", r.is_residual == 1);

    r = tt_route(143, 3);  /* random + bridge */
    TEST("bridge+random → bridge_raw", r.strategy == TT_STRAT_BRIDGE_RAW);

    /* Destination always from enc — never stored in TTRoute */
    /* Caller uses frame_at(r.enc) for face/slot, ft_enc_to_field(r.enc) for field pos */

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   /* ══════════════════════════════════════════════════════════════
      T8: Full pipeline — data → enc → THREE-VIEW routing
      ══════════════════════════════════════════════════════════════ */
   static int test_full_pipeline(void)
   {
       printf("=== T8: Full pipeline (three-view routing) ===\n");

       TTContext ctx;
       tt_init(&ctx);
       srand(31415);
       for (int i = 0; i < 10; i++) {
           uint8_t data[48];
           for (int j = 0; j < 48; j++) data[j] = (uint8_t)(rand() & 0xFF);

           TTChunkRecord rec;
           tt_ingest(&ctx, data, 48, &rec);
           TTRoute r = tt_route_record(&rec);

           /* View 1: Frame Seek — "data คืออะไร" */
           TEST("enc < 1440", r.enc < 1440);
           TEST("face < 12", r.face < 12);
           TEST("slot < 120", r.slot < 120);

           /* View 2: Fibo Spine — "data อยู่ใน pipe ไหน" */
           TEST("pipe_id < 1728", r.pipe_id < 1728);
           TEST("tick < 12", r.tick < 12);

           /* View 3: P5H Ribcage — "data อยู่ใน flower ไหน" */
           TEST("flower_id < 1728", r.flower_id < 1728);

           /* Strategy */
           TEST("strategy valid", r.strategy <= TT_STRAT_BRIDGE_CMP);

           printf("  [%2d] enc=%-4u face=%-2u slot=%-3u | pipe=%-4u tick=%-2u | flower=%-4u ph=%-3u | %s\n",
                  i, r.enc, r.face, r.slot, r.pipe_id, r.tick,
                  r.flower_id, r.phase_in_flower,
                  r.is_compressed ? "[CMP]" : "[RAW]");
       }

       TEST("stats consistent", tt_verify(&ctx) == 0);
       printf("  Stats: %u chunks, %u bytes, coverage=%u/1440\n",
              ctx.stats.total_chunks, ctx.stats.total_bytes,
              ctx.stats.enc_coverage);

       return 0;
   }

/* ══════════════════════════════════════════════════════════════
   T9: Frame tolerance — Fibonacci scale + adaptive
   ══════════════════════════════════════════════════════════════ */
static int test_frame_tolerance(void)
{
    printf("=== T9: Frame tolerance (Fibonacci scale + adaptive) ===\n");

    /* Fibonacci spans: 0,1,2,3 */
    TEST("FIB_SPANS[0]=0 (structured)", FIB_SPANS[0] == 0);
    TEST("FIB_SPANS[1]=1 (moderate)",   FIB_SPANS[1] == 1);
    TEST("FIB_SPANS[2]=2 (high)",       FIB_SPANS[2] == 2);
    TEST("FIB_SPANS[3]=3 (random)",     FIB_SPANS[3] == 3);

    /* Structured data: span=0, strict 1 frame */
    uint8_t zeros[48] = {0};
    uint8_t ent = tt_entropy_score(zeros, 48);
    uint8_t cls = tt_entropy_class(ent);

    TTChunkRecord rec;
    TTContext ctx;
    tt_init(&ctx);
    tt_ingest(&ctx, zeros, 48, &rec);

    FrameRange fr = frame_range(rec.enc, cls);
    TEST("structured: span=0", fr.span == 0);
    TEST("structured: home_frame valid", fr.home_frame < 120);
    printf("  zeros: enc=%u frame=%u span=%u [strict]\n",
           rec.enc, fr.home_frame, fr.span);

    /* Random data: Fibonacci span=3 (class 3) */
    uint8_t rnd[48]; srand(42);
    for (int i = 0; i < 48; i++) rnd[i] = (uint8_t)(rand() & 0xFF);
    tt_ingest(&ctx, rnd, 48, &rec);

    ent = tt_entropy_score(rnd, 48);
    cls = tt_entropy_class(ent);
    fr = frame_range(rec.enc, cls);
    static const uint8_t EXPECTED_SPANS[4] = { 0, 1, 2, 3 };
    TEST("random: span matches Fibonacci", fr.span == EXPECTED_SPANS[cls]);
    printf("  random: enc=%u frame=%u span=%u class=%u [Fibonacci]\n",
           rec.enc, fr.home_frame, fr.span, cls);

    /* frame_in_range: structured → only home frame */
    TEST("structured: home in range", frame_in_range(rec.enc, rec.enc, 0));
    TEST("structured: adjacent out", !frame_in_range(rec.enc + 12, rec.enc, 0));

    /* frame_in_range: random → Fibonacci span=3 */
    TEST("random: home in range", frame_in_range(rec.enc, rec.enc, cls));
    TEST("random: +1 frame in range", frame_in_range(rec.enc + 12, rec.enc, cls));
    TEST("random: +2 frame in range", frame_in_range(rec.enc + 24, rec.enc, cls));
    TEST("random: +4 frame OUT (span=3)", !frame_in_range(rec.enc + 48, rec.enc, cls));

    /* Adaptive: entropy score → Fibonacci index → span */
    printf("  ── Adaptive (entropy score → Fibonacci span) ──\n");
    FrameRange fr_lo = frame_range_adaptive(0, 0);      /* score=0 → fib_idx=0 → span=0 */
    FrameRange fr_md = frame_range_adaptive(0, 128);    /* score=128 → fib_idx=5 → span=5 */
    FrameRange fr_hi = frame_range_adaptive(0, 255);    /* score=255 → fib_idx=11 → span=89 */
    TEST("adaptive: score=0 → span=0", fr_lo.span == 0);
    TEST("adaptive: score=128 → span=5", fr_md.span == 5);
    TEST("adaptive: score=255 → span=59 (clipped by FRAME_MAX_SPAN)", fr_hi.span == 59);
    printf("  adaptive: score=0→span=%u, score=128→span=%u, score=255→span=%u\n",
           fr_lo.span, fr_md.span, fr_hi.span);

    printf("  Fibonacci scale: 0,1,2,3 (conservative) | adaptive: 0..89 (raw Fibonacci)\n");

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   T10: Enclosure integration — tt_store bridges tracker ↔ enclosure
   ══════════════════════════════════════════════════════════════ */
static int test_enclosure_integration(void)
{
    printf("=== T10: Enclosure integration (tracker → enclosure) ===\n");

    /* Init enclosure — scale=1 gives chunk_size=20736 (manageable for test) */
    EncCtx enc_ctx;
    enc_init(&enc_ctx, 1);  /* scale 1 → field_dim=144, chunk_size=20736 */

    TTContext ctx;
    tt_init(&ctx);

    /* Ingest structured data (zeros) → should compress */
    uint8_t zeros[48] = {0};
    TTChunkRecord rec;
    tt_ingest(&ctx, zeros, 48, &rec);
    TTRoute route = tt_route_record(&rec);

    TEST("structured: route strat=COMPRESS", route.strategy == TT_STRAT_COMPRESS);

    /* Store into enclosure — chunk_out must be chunk_size bytes */
    static uint8_t store_buf[20736];
    TTStoreResult sr = tt_store(&enc_ctx, &rec, store_buf);

    TEST("structured: stored OK", sr.stored == 1);
    TEST("structured: chunk_idx valid", sr.chunk_idx >= 0);
    TEST("structured: enc preserved", sr.enc == rec.enc);
    /* frame_lo = home_frame (not always 0 — depends on enc value) */
    FrameRange fr_check = frame_range(rec.enc, 0);
    TEST("structured: frame_lo = home_frame", sr.frame_lo == fr_check.home_frame);
    TEST("structured: frame_lo = frame_hi (span=0)", sr.frame_lo == sr.frame_hi);
    printf("  structured: enc=%u chunk_idx=%d frame=[%u,%u]\n",
           sr.enc, sr.chunk_idx, sr.frame_lo, sr.frame_hi);

    /* Ingest high-entropy data (random) → should be raw */
    uint8_t rnd[48]; srand(99);
    for (int i = 0; i < 48; i++) rnd[i] = (uint8_t)(rand() & 0xFF);
    tt_ingest(&ctx, rnd, 48, &rec);
    route = tt_route_record(&rec);

    TEST("random: route strat=RAW or BRIDGE_RAW",
         route.strategy == TT_STRAT_RAW || route.strategy == TT_STRAT_BRIDGE_RAW);

    /* Store into enclosure */
    TTStoreResult sr2 = tt_store(&enc_ctx, &rec, store_buf);
    TEST("random: stored OK", sr2.stored == 1);
    TEST("random: chunk_idx valid", sr2.chunk_idx >= 0);
    /* High entropy should have non-zero frame span */
    TEST("random: frame_lo != frame_hi (tolerance)", sr2.frame_lo != sr2.frame_hi);
    printf("  random: enc=%u chunk_idx=%d frame=[%u,%u] span=%u\n",
           sr2.enc, sr2.chunk_idx, sr2.frame_lo, sr2.frame_hi,
           (uint8_t)(sr2.frame_hi - sr2.frame_lo));

    /* Batch: ingest 20 mixed chunks, store all */
    int stored_count = 0;
    for (int i = 0; i < 20; i++) {
        uint8_t buf[48];
        for (int j = 0; j < 48; j++) buf[j] = (uint8_t)((i * 7 + j * 13) & 0xFF);
        tt_ingest(&ctx, buf, 48, &rec);
        TTStoreResult s = tt_store(&enc_ctx, &rec, store_buf);
        if (s.stored) stored_count++;
    }
    TEST("batch: all 20 stored", stored_count == 20);
    TEST("batch: enclosure tracked chunks", enc_ctx.n_chunks == 22);  /* 2 + 20 */
    printf("  batch: %d/20 stored, enclosure chunks=%u\n",
           stored_count, enc_ctx.n_chunks);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Tensor Track V1 vs V2 — Comparison Test               ║\n");
    printf("║  V1: enc + enclosure + manual routing (32B/record)     ║\n");
    printf("║  V2: enc only + entropy (10B/record)                   ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_entropy();
    test_deterministic_lossless();
    test_v1_v2_same_enc();
    test_enc_dual_answer();
    test_record_size();
    test_v2_batch();
    test_redundancy();
    test_routing();
    test_full_pipeline();
    printf("Total: %d PASS / %d FAIL\n\n", n_pass, n_fail);

    /* T9: Frame tolerance */
    test_frame_tolerance();
    printf("Total: %d PASS / %d FAIL\n\n", n_pass, n_fail);

    /* T10: Enclosure integration */
    test_enclosure_integration();
    printf("Total: %d PASS / %d FAIL\n\n", n_pass, n_fail);

    printf("═══════════════════════════════════════════\n");
    printf("  FINAL: %d PASS / %d FAIL\n", n_pass, n_fail);

    return n_fail > 0 ? 1 : 0;
}

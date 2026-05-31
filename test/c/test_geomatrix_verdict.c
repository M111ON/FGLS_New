/*
 * test_geomatrix_verdict.c — Bridge→Verdict→Route/Ground Wire Test
 * =================================================================
 * ทดสอบ handshake: gbt_capture → GeoPacket → geomatrix_batch_verdict
 *
 * Tests:
 *   T6: gbt_capture → GeoPacket mapping ถูกต้อง (sig/hpos/idx/phase)
 *   T7: structured batch (18 pkts) → verdict=true  → ROUTE path
 *   T8: corrupted batch (sig mismatch) → verdict=false → GROUND path
 *
 * Logic:
 *   GBBlueprint (from gbt_capture window) → GeoPacket[] via blueprint fields:
 *     sig   = geo_compute_sig64(bundle, phase)   ← must match exactly
 *     hpos  = tring_end % GEO_SLOTS              ← Hilbert position
 *     idx   = tring_start % GEO_SLOTS            ← bit index
 *     phase = circuit_fired & 0x3                ← PROBE/MAIN/MIRROR/CANCEL
 *   Same-block rule: (hpos>=288) == (idx>=288) → Hilbert consistency
 *
 * Compile:
 *   gcc -O2 -o test_geomatrix_verdict test_geomatrix_verdict.c \
 *       -I/tmp/core/core/core -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "geo_tring_goldberg_wire.h"

/* ── Config ──────────────────────────────────────────────────────── */
static const GeoSeed TEST_SEED = { 0xDEADBEEF12345678ULL,
                                   0xCAFEBABE87654321ULL };

/* Structured bundle — all words non-zero, deterministic */
static const uint64_t STRUCT_BUNDLE[GEO_BUNDLE_WORDS] = {
    0x1111111111111111ULL, 0x2222222222222222ULL,
    0x3333333333333333ULL, 0x4444444444444444ULL,
    0x5555555555555555ULL, 0x6666666666666666ULL,
    0x7777777777777777ULL, 0x8888888888888888ULL,
};

/* ── Blueprint → GeoPacket conversion (the missing wire) ─────────── */
/*
 * Blueprint เก็บ spatial+temporal fingerprint ของ 1 flush window (144 ops)
 * GeoPacket ต้องการ: sig, hpos, idx, phase
 *
 * Mapping:
 *   phase = circuit_fired & 0x3        (which bipolar pair fired → phase slot)
 *   sig   = geo_compute_sig64(bundle, phase)
 *   hpos  = tring_end   % GEO_SLOTS    (end position → Hilbert space)
 *   idx   = tring_start % GEO_SLOTS    (start position → bit index)
 *
 * Same-block constraint (Hilbert consistency):
 *   both hpos and idx must be in same half [0..287] or [288..575]
 *   → enforce by clamping idx into same block as hpos
 */
static inline GeoPacket blueprint_to_geopkt(const GBBlueprint *bp,
                                             const uint64_t    *bundle,
                                             uint8_t            path_id)
{
    GeoPacket pkt;
    memset(&pkt, 0, sizeof(pkt));

    /* phase rotates per path to fill all 18 GEOMATRIX_PATHS */
    pkt.phase = (uint8_t)((bp->circuit_fired + path_id) & 0x3u);
    pkt.sig   = geo_compute_sig64(bundle, pkt.phase);

    /* Hilbert positions from temporal window */
    uint16_t h_end   = (uint16_t)(bp->tring_end   % GEO_SLOTS);
    uint16_t h_start = (uint16_t)(bp->tring_start % GEO_SLOTS);

    pkt.hpos = h_end;

    /* Enforce same-block: idx must be in same half as hpos */
    bool hpos_hi = (h_end >= GEO_BLOCK_BOUNDARY);
    bool idx_hi  = (h_start >= GEO_BLOCK_BOUNDARY);
    if (hpos_hi != idx_hi)
        /* flip idx into same block */
        pkt.idx = hpos_hi
                  ? (uint16_t)(h_start + GEO_BLOCK_BOUNDARY)
                  : (uint16_t)(h_start % GEO_BLOCK_BOUNDARY);
    else
        pkt.idx = h_start;

    pkt.bit = (uint8_t)(bp->stamp_hash & 0x1u);
    return pkt;
}

/* Build a full 18-packet batch from one blueprint */
static void build_batch(GeoPacket batch[GEOMATRIX_PATHS],
                        const GBBlueprint *bp,
                        const uint64_t    *bundle)
{
    for (int i = 0; i < GEOMATRIX_PATHS; i++)
        batch[i] = blueprint_to_geopkt(bp, bundle, (uint8_t)i);
}

/* ── Helpers ─────────────────────────────────────────────────────── */
static void make_structured(uint8_t *buf, uint32_t sz) {
    uint32_t a=1, b=2;
    for (uint32_t i=0; i<sz; i++) {
        buf[i] = (uint8_t)((a+b) & 0xFF);
        uint32_t t = a+b; a=b; b=t;
    }
}

/* Run enough writes through TGWCtx to trigger at least 1 blueprint flush */
static GBBlueprint capture_blueprint(const uint64_t *bundle) {
    static uint8_t data[4096 * 2];
    make_structured(data, sizeof(data));

    TGWCtx ctx;
    tgw_init(&ctx, TEST_SEED, bundle);

    uint16_t n = tgw_stream_file(&ctx, data, sizeof(data));
    (void)n;

    /* Force flush if not yet triggered (should be after 144+ ops) */
    GBBlueprint bp = gbt_tracker_extract(&ctx.gp.tracker);
    return bp;
}

/* ═══════════════════════════════════════════════════════════════════
 * T6: GeoPacket mapping — sig/hpos/idx/phase all valid
 * ═══════════════════════════════════════════════════════════════════ */
static int test_t6_geopkt_mapping(void) {
    printf("[T6] blueprint → GeoPacket field mapping\n");

    GBBlueprint bp = capture_blueprint(STRUCT_BUNDLE);

    printf("  blueprint: tring=[%u..%u] span=%u circuit=0x%02X stamp=0x%08X\n",
           bp.tring_start, bp.tring_end, bp.tring_span,
           bp.circuit_fired, bp.stamp_hash);

    int ok = 1;
    for (int i = 0; i < GEOMATRIX_PATHS; i++) {
        GeoPacket pkt = blueprint_to_geopkt(&bp, STRUCT_BUNDLE, (uint8_t)i);

        /* sig must match bundle+phase */
        uint64_t expected = geo_compute_sig64(STRUCT_BUNDLE, pkt.phase);
        if (pkt.sig != expected) {
            printf("  FAIL path %d: sig mismatch\n", i);
            ok = 0; break;
        }

        /* Hilbert same-block constraint */
        bool hb = (pkt.hpos >= GEO_BLOCK_BOUNDARY);
        bool ib = (pkt.idx  >= GEO_BLOCK_BOUNDARY);
        if (hb != ib) {
            printf("  FAIL path %d: Hilbert block mismatch hpos=%u idx=%u\n",
                   i, pkt.hpos, pkt.idx);
            ok = 0; break;
        }

        /* range */
        if (pkt.hpos >= GEO_SLOTS || pkt.idx >= GEO_SLOTS || pkt.phase > 3) {
            printf("  FAIL path %d: out of range hpos=%u idx=%u phase=%u\n",
                   i, pkt.hpos, pkt.idx, pkt.phase);
            ok = 0; break;
        }
    }

    if (ok) printf("  PASS: all %d paths produce valid GeoPackets\n",
                   GEOMATRIX_PATHS);
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * T7: structured batch → verdict=true → ROUTE
 * ═══════════════════════════════════════════════════════════════════ */
static int test_t7_route(void) {
    printf("[T7] structured batch → verdict=true (ROUTE)\n");

    GBBlueprint bp = capture_blueprint(STRUCT_BUNDLE);
    GeoPacket batch[GEOMATRIX_PATHS];
    build_batch(batch, &bp, STRUCT_BUNDLE);

    GeomatrixStatsV3 stats = {0};
    bool verdict = geomatrix_batch_verdict(batch, STRUCT_BUNDLE, &stats);

    /* Count how many paths passed validation */
    int32_t score = 0;
    for (int i = 0; i < GEOMATRIX_PATHS; i++) {
        GeomatrixStatsV3 tmp = {0};
        if (geomatrix_packet_validate(&batch[i], STRUCT_BUNDLE, &tmp))
            score += 8;
    }

    printf("  score=%d  window=[%d..%d]  verdict=%s\n",
           score, GEO_WINDOW_LO, GEO_WINDOW_HI,
           verdict ? "true→ROUTE" : "false→GROUND");
    printf("  stats: total=%llu sig_miss=%llu hilbert_viol=%llu stable=%llu\n",
           (unsigned long long)stats.total_packets,
           (unsigned long long)stats.sig_mismatches,
           (unsigned long long)stats.hilbert_violations,
           (unsigned long long)stats.stable_batches);

    if (verdict)
        printf("  PASS: structured batch → ROUTE ✓\n");
    else
        printf("  FAIL: expected ROUTE (score=%d outside [%d..%d])\n",
               score, GEO_WINDOW_LO, GEO_WINDOW_HI);

    return (int)verdict;
}

/* ═══════════════════════════════════════════════════════════════════
 * T8: corrupted batch → verdict=false → GROUND
 * ═══════════════════════════════════════════════════════════════════ */
static int test_t8_ground(void) {
    printf("[T8] corrupted batch → verdict=false (GROUND)\n");

    GBBlueprint bp = capture_blueprint(STRUCT_BUNDLE);
    GeoPacket batch[GEOMATRIX_PATHS];
    build_batch(batch, &bp, STRUCT_BUNDLE);

    /* Corrupt: flip sig bits on all packets → sig mismatch */
    for (int i = 0; i < GEOMATRIX_PATHS; i++)
        batch[i].sig ^= 0xDEAD000000000000ULL;

    GeomatrixStatsV3 stats = {0};
    bool verdict = geomatrix_batch_verdict(batch, STRUCT_BUNDLE, &stats);

    printf("  verdict=%s  sig_mismatches=%llu\n",
           verdict ? "true→ROUTE" : "false→GROUND",
           (unsigned long long)stats.sig_mismatches);

    if (!verdict)
        printf("  PASS: corrupted batch → GROUND ✓\n");
    else
        printf("  FAIL: expected GROUND but got ROUTE\n");

    return (int)(!verdict);
}

/* ═══════════════════════════════════════════════════════════════════
 * T8b: Hilbert violation → GROUND (same-block rule)
 * ═══════════════════════════════════════════════════════════════════ */
static int test_t8b_hilbert_violation(void) {
    printf("[T8b] Hilbert block violation → GROUND\n");

    GBBlueprint bp = capture_blueprint(STRUCT_BUNDLE);
    GeoPacket batch[GEOMATRIX_PATHS];
    build_batch(batch, &bp, STRUCT_BUNDLE);

    /* Force cross-block violation: put hpos in hi, idx in lo */
    for (int i = 0; i < GEOMATRIX_PATHS; i++) {
        batch[i].hpos = GEO_BLOCK_BOUNDARY + 1;  /* hi block */
        batch[i].idx  = GEO_BLOCK_BOUNDARY - 1;  /* lo block */
        /* keep sig valid — this isolates Hilbert layer */
    }

    GeomatrixStatsV3 stats = {0};
    bool verdict = geomatrix_batch_verdict(batch, STRUCT_BUNDLE, &stats);

    printf("  verdict=%s  hilbert_violations=%llu\n",
           verdict ? "true→ROUTE" : "false→GROUND",
           (unsigned long long)stats.hilbert_violations);

    if (!verdict)
        printf("  PASS: Hilbert violation → GROUND ✓\n");
    else
        printf("  FAIL: expected GROUND from Hilbert violation\n");

    return (int)(!verdict);
}

/* ═══════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  Geomatrix Verdict Wire Test  T6/T7/T8       ║\n");
    printf("║  GBBlueprint → GeoPacket → ROUTE/GROUND      ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    int r[4];
    r[0] = test_t6_geopkt_mapping();  printf("\n");
    r[1] = test_t7_route();           printf("\n");
    r[2] = test_t8_ground();          printf("\n");
    r[3] = test_t8b_hilbert_violation(); printf("\n");

    int passed = r[0]+r[1]+r[2]+r[3];
    printf("══════════════════════════════════════════════\n");
    printf("RESULT: %d/4 passed\n", passed);
    printf("STATUS: %s\n", passed==4
        ? "✅ Verdict Layer VERIFIED — bridge→route/ground wired"
        : "⚠️  check failures above");

    /* Wiring map summary */
    printf("\n── Wiring Status ──────────────────────────────\n");
    printf("gbt_capture → GeoPacket mapping  %s\n", r[0]?"✅":"❌");
    printf("structured  → verdict=true  ROUTE %s\n", r[1]?"✅":"❌");
    printf("corrupt sig → verdict=false GROUND%s\n", r[2]?"✅":"❌");
    printf("Hilbert viol→ verdict=false GROUND%s\n", r[3]?"✅":"❌");
    printf("───────────────────────────────────────────────\n");

    return (passed==4) ? 0 : 1;
}

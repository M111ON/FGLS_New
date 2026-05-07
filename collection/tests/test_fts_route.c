/*
 * test_fts_route.c — T10: ROUTE → FtsTwinStore Wire Test
 * =======================================================
 * ทดสอบ full path หลัง verdict=true:
 *
 *   GBBlueprint → GeoPacket → verdict=ROUTE
 *       → DodecaEntry (from bp fields)
 *       → fts_write()  → FrustumSlot64 in GiantArray
 *       → fts_accessible() / fts_stats()
 *       → fts_delete()  → structural silence
 *       → fts_serialize() → 4896B CubeFileStore
 *
 * Tests:
 *   T10a: trit address derivation — (addr^value)%27 maps to valid coset/face/level
 *   T10b: fts_write from DodecaEntry → FrustumSlot64 checksum consistent
 *   T10c: fts_accessible before/after delete → 0 / -1
 *   T10d: overflow on deleted coset → overflow_count increments
 *   T10e: fts_serialize → 4896B buffer non-zero
 *   T10f: full ROUTE pipeline — blueprint → verdict → DodecaEntry → fts_write
 *
 * Compile:
 *   gcc -O2 -o test_fts_route test_fts_route.c \
 *       -I/tmp/core/core/core \
 *       -I/tmp/core/core/core/twin_core \
 *       -I/tmp/core/core/geo_headers \
 *       -I/tmp/core/core/pogls_engine \
 *       -I/tmp/core/core/pogls_engine/core \
 *       -I/tmp/core/core/pogls_engine/twin_core \
 *       -I/tmp/core/core/pogls_engine/KV_patched \
 *       -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "geo_tring_goldberg_wire.h"
#include "../pogls_engine/fgls_twin_store.h"

/* ── Config ──────────────────────────────────────────────────────── */
static const GeoSeed TEST_SEED = { 0xDEADBEEF12345678ULL,
                                   0xCAFEBABE87654321ULL };

static const uint64_t STRUCT_BUNDLE[GEO_BUNDLE_WORDS] = {
    0x1111111111111111ULL, 0x2222222222222222ULL,
    0x3333333333333333ULL, 0x4444444444444444ULL,
    0x5555555555555555ULL, 0x6666666666666666ULL,
    0x7777777777777777ULL, 0x8888888888888888ULL,
};

/* ── Blueprint → DodecaEntry (the ROUTE wire) ────────────────────
 * After verdict=true, blueprint fields map to DodecaEntry:
 *   merkle_root = stamp_hash (64-bit extended)
 *   sha256_hi   = spatial_xor >> 0  (reuse as integrity proof)
 *   sha256_lo   = window_id
 *   offset      = circuit_fired & 0xFF
 *   hop_count   = tring_span
 *   segment     = top_gaps[0]
 */
static inline DodecaEntry blueprint_to_dodeca(const GBBlueprint *bp) {
    DodecaEntry e;
    memset(&e, 0, sizeof(e));
    e.merkle_root = (uint64_t)bp->stamp_hash
                  | ((uint64_t)bp->stamp_hash << 32);
    e.sha256_hi   = (uint64_t)bp->spatial_xor;
    e.sha256_lo   = bp->window_id;
    e.offset      = bp->circuit_fired;
    e.hop_count   = bp->tring_span;
    e.segment     = bp->top_gaps[0];
    return e;
}

/* ── Run 150 writes → capture real blueprint via TGWResult ───────── */
static GBBlueprint get_real_blueprint(void) {
    TGWCtx ctx;
    tgw_init(&ctx, TEST_SEED, STRUCT_BUNDLE);
    GBBlueprint bp;
    memset(&bp, 0, sizeof(bp));
    for (uint32_t i = 0; i < 150u; i++) {
        TGWResult r = tgw_write(&ctx,
                                (uint64_t)(0x1000u + i * 7u),
                                (uint64_t)(0xABCDu ^ i), 0);
        if (r.gpr.blueprint_ready)
            bp = r.gpr.bp;
    }
    return bp;
}

/* ═══════════════════════════════════════════════════════════════════
 * T10a: trit address derivation — all 27 trits map correctly
 * ═══════════════════════════════════════════════════════════════════ */
static int test_t10a_trit_addr(void) {
    printf("[T10a] trit address derivation — 27-trit space coverage\n");

    int seen_trit[27] = {0};
    int ok = 1;

    /* Sample 270 (addr,value) pairs — should hit all 27 trits */
    for (uint32_t i = 0; i < 270u; i++) {
        uint64_t addr  = (uint64_t)(i * 13u + 1u);
        uint64_t value = (uint64_t)(i * 7u);
        FtsTritAddr ta = fts_trit_addr(addr, value);

        /* range checks */
        if (ta.trit  > 26 || ta.coset > 8 || ta.face > 5 || ta.level > 3) {
            printf("  FAIL i=%u: trit=%u coset=%u face=%u level=%u\n",
                   i, ta.trit, ta.coset, ta.face, ta.level);
            ok = 0; break;
        }

        /* derivation consistency */
        if (ta.coset != ta.trit / 3u || ta.face != ta.trit % 6u
            || ta.level != ta.trit % 4u) {
            printf("  FAIL i=%u: derivation inconsistent\n", i);
            ok = 0; break;
        }

        seen_trit[ta.trit] = 1;
    }

    int covered = 0;
    for (int i = 0; i < 27; i++) covered += seen_trit[i];
    printf("  trit coverage: %d/27\n", covered);

    if (ok && covered == 27)
        printf("  PASS: all 27 trits reachable, derivation correct\n");
    else if (ok)
        printf("  WARN: only %d/27 trits covered (need more samples)\n", covered);
    else
        return 0;

    return ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * T10b: fts_write → FrustumSlot64 checksum consistent
 * ═══════════════════════════════════════════════════════════════════ */
static int test_t10b_write_checksum(void) {
    printf("[T10b] fts_write → FrustumSlot64 checksum integrity\n");

    FtsTwinStore store;
    fts_init(&store, 0xFEEDFACECAFEBABEULL);

    GBBlueprint bp = get_real_blueprint();
    DodecaEntry e  = blueprint_to_dodeca(&bp);

    /* write at 9 different (addr,value) pairs — one per coset */
    int ok = 1;
    for (uint32_t i = 0; i < 9u; i++) {
        uint64_t addr  = (uint64_t)(i * 3u);    /* trit = (0,3,6,...) */
        uint64_t value = 0ULL;                   /* addr^value = addr  */

        int r = fts_write(&store, addr, value, &e);
        if (r != 0) {
            printf("  FAIL i=%u: fts_write returned %d\n", i, r);
            ok = 0; break;
        }

        /* verify checksum on the written slot */
        FtsTritAddr ta   = fts_trit_addr(addr, value);
        GiantCube   *gc  = &store.ga.cubes[ta.coset];
        FrustumSlot64 *slot = &gc->faces[ta.face];

        uint32_t expected_chk = 0u;
        for (int j = 0; j < 4; j++)
            expected_chk ^= (uint32_t)(slot->core[j] ^ (slot->core[j] >> 32));

        if (slot->checksum != expected_chk) {
            printf("  FAIL i=%u: checksum 0x%08X != expected 0x%08X\n",
                   i, slot->checksum, expected_chk);
            ok = 0; break;
        }

        /* verify field mapping */
        if (slot->coset != ta.coset || slot->frustum_id != ta.face) {
            printf("  FAIL i=%u: slot coset=%u face=%u != ta coset=%u face=%u\n",
                   i, slot->coset, slot->frustum_id, ta.coset, ta.face);
            ok = 0; break;
        }
    }

    FtsTwinStats st = fts_stats(&store);
    printf("  writes=%u  active_cosets=%u\n", st.writes, st.active_cosets);

    if (ok) printf("  PASS: all FrustumSlot64 checksums consistent\n");
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * T10c: fts_accessible before/after delete
 * ═══════════════════════════════════════════════════════════════════ */
static int test_t10c_accessible(void) {
    printf("[T10c] fts_accessible before/after delete\n");

    FtsTwinStore store;
    fts_init(&store, 0x1234567890ABCDEFULL);

    GBBlueprint bp = get_real_blueprint();
    DodecaEntry e  = blueprint_to_dodeca(&bp);

    uint64_t addr = 0x42ULL, value = 0x0ULL;  /* trit = 0x42%27 */
    fts_write(&store, addr, value, &e);

    int before = fts_accessible(&store, addr, value);
    fts_delete(&store, addr, value);
    int after  = fts_accessible(&store, addr, value);

    printf("  accessible before=%d after=%d (expect 0 / -1)\n", before, after);

    int ok = (before == 0) && (after == -1);
    if (ok) printf("  PASS: coset silenced correctly\n");
    else    printf("  FAIL\n");
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * T10d: overflow on deleted coset
 * ═══════════════════════════════════════════════════════════════════ */
static int test_t10d_overflow(void) {
    printf("[T10d] write to deleted coset → overflow_count\n");

    FtsTwinStore store;
    fts_init(&store, 0xABCDEF0123456789ULL);

    GBBlueprint bp = get_real_blueprint();
    DodecaEntry e  = blueprint_to_dodeca(&bp);

    uint64_t addr = 5ULL, value = 0ULL;   /* trit=5, coset=1, face=5 */
    FtsTritAddr ta = fts_trit_addr(addr, value);

    /* delete coset first */
    fts_delete_coset(&store, ta.coset);

    /* attempt write → should overflow */
    int r = fts_write(&store, addr, value, &e);
    FtsTwinStats st = fts_stats(&store);

    printf("  fts_write returned %d  overflow_count=%u\n", r, st.overflows);

    int ok = (r == -1) && (st.overflows == 1u);
    if (ok) printf("  PASS: overflow correctly detected\n");
    else    printf("  FAIL\n");
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * T10e: fts_serialize → 4896B buffer, header non-zero
 * ═══════════════════════════════════════════════════════════════════ */
static int test_t10e_serialize(void) {
    printf("[T10e] fts_serialize → 4896B CubeFileStore\n");

    FtsTwinStore store;
    fts_init(&store, 0xDEADC0DEBEEF1234ULL);

    GBBlueprint bp = get_real_blueprint();
    DodecaEntry e  = blueprint_to_dodeca(&bp);

    /* write a few entries */
    for (uint32_t i = 0; i < 6u; i++)
        fts_write(&store, (uint64_t)(i * 5u), (uint64_t)i, &e);

    fts_serialize(&store);

    static uint8_t buf[GCFS_TOTAL_BYTES];
    fts_write_buf(&store, buf);

    /* check size constant */
    if (GCFS_TOTAL_BYTES != 4896u) {
        printf("  FAIL: GCFS_TOTAL_BYTES=%u != 4896\n", GCFS_TOTAL_BYTES);
        return 0;
    }

    /* at least some bytes must be non-zero */
    int nonzero = 0;
    for (int i = 0; i < GCFS_TOTAL_BYTES; i++)
        if (buf[i]) { nonzero++; }

    printf("  buffer size=%u  non-zero bytes=%d\n", GCFS_TOTAL_BYTES, nonzero);

    int ok = (nonzero > 0);
    if (ok) printf("  PASS: 4896B buffer serialized\n");
    else    printf("  FAIL: buffer all zeros\n");
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * T10f: full ROUTE pipeline end-to-end
 *   GBBlueprint → verdict=ROUTE → DodecaEntry → fts_write → verify
 * ═══════════════════════════════════════════════════════════════════ */
static int test_t10f_full_route(void) {
    printf("[T10f] full ROUTE pipeline — verdict → FtsTwinStore\n");

    /* 1. Get real blueprint via TGW */
    TGWCtx tgw;
    tgw_init(&tgw, TEST_SEED, STRUCT_BUNDLE);

    GBBlueprint bp;
    memset(&bp, 0, sizeof(bp));
    int routed = 0;

    for (uint32_t i = 0; i < 150u && !routed; i++) {
        TGWResult r = tgw_write(&tgw,
                                (uint64_t)(0x2000u + i * 11u),
                                (uint64_t)(0xBEEFu ^ i), 0);
        if (!r.gpr.blueprint_ready) continue;
        bp = r.gpr.bp;

        /* 2. Build GeoPacket batch and run verdict */
        GeoPacket batch[GEOMATRIX_PATHS];
        for (int p = 0; p < GEOMATRIX_PATHS; p++) {
            batch[p].phase = (uint8_t)((bp.circuit_fired + p) & 0x3u);
            batch[p].sig   = geo_compute_sig64(STRUCT_BUNDLE, batch[p].phase);
            uint16_t h_end   = (uint16_t)(bp.tring_end   % GEO_SLOTS);
            uint16_t h_start = (uint16_t)(bp.tring_start % GEO_SLOTS);
            batch[p].hpos  = h_end;
            bool hb = (h_end   >= GEO_BLOCK_BOUNDARY);
            bool ib = (h_start >= GEO_BLOCK_BOUNDARY);
            batch[p].idx = (hb != ib)
                ? (hb ? (uint16_t)(h_start + GEO_BLOCK_BOUNDARY)
                      : (uint16_t)(h_start % GEO_BLOCK_BOUNDARY))
                : h_start;
            batch[p].bit = (uint8_t)(bp.stamp_hash & 1u);
        }

        GeomatrixStatsV3 stats = {0};
        bool verdict = geomatrix_batch_verdict(batch, STRUCT_BUNDLE, &stats);
        if (!verdict) {
            printf("  FAIL: verdict=GROUND (unexpected)\n");
            return 0;
        }

        /* 3. Convert blueprint → DodecaEntry → fts_write */
        FtsTwinStore store;
        fts_init(&store, 0xF00DBABE12345678ULL);

        DodecaEntry e = blueprint_to_dodeca(&bp);
        uint64_t route_addr  = (uint64_t)bp.stamp_hash;
        uint64_t route_value = (uint64_t)bp.spatial_xor;

        int wr = fts_write(&store, route_addr, route_value, &e);
        if (wr != 0) {
            printf("  FAIL: fts_write returned %d\n", wr);
            return 0;
        }

        /* 4. Verify accessible and stat */
        int acc = fts_accessible(&store, route_addr, route_value);
        FtsTwinStats st = fts_stats(&store);

        printf("  blueprint: events=%u window=%llu stamp=0x%08X\n",
               bp.event_count, (unsigned long long)bp.window_id, bp.stamp_hash);
        printf("  verdict=ROUTE  fts_write=ok  accessible=%d  writes=%u\n",
               acc, st.writes);

        if (acc == 0 && st.writes == 1u)
            routed = 1;
    }

    if (routed)
        printf("  PASS: full ROUTE pipeline wired end-to-end ✓\n");
    else
        printf("  FAIL: pipeline did not complete\n");

    return routed;
}

/* ═══════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  FtsTwinStore Route Wire Test  T10a–T10f     ║\n");
    printf("║  verdict=ROUTE → DodecaEntry → GiantArray    ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    int r[6];
    r[0] = test_t10a_trit_addr();    printf("\n");
    r[1] = test_t10b_write_checksum(); printf("\n");
    r[2] = test_t10c_accessible();   printf("\n");
    r[3] = test_t10d_overflow();     printf("\n");
    r[4] = test_t10e_serialize();    printf("\n");
    r[5] = test_t10f_full_route();   printf("\n");

    int passed = r[0]+r[1]+r[2]+r[3]+r[4]+r[5];
    printf("══════════════════════════════════════════════\n");
    printf("RESULT: %d/6 passed\n", passed);
    printf("STATUS: %s\n", passed==6
        ? "✅ ROUTE→FtsTwinStore VERIFIED"
        : "⚠️  check failures above");

    printf("\n── Wiring Status ──────────────────────────────\n");
    printf("trit derivation (addr^val)%%27     %s\n", r[0]?"✅":"❌");
    printf("fts_write → FrustumSlot checksum  %s\n", r[1]?"✅":"❌");
    printf("fts_accessible before/after delete %s\n", r[2]?"✅":"❌");
    printf("deleted coset → overflow_count    %s\n", r[3]?"✅":"❌");
    printf("fts_serialize → 4896B buffer      %s\n", r[4]?"✅":"❌");
    printf("full ROUTE pipeline end-to-end    %s\n", r[5]?"✅":"❌");
    printf("───────────────────────────────────────────────\n");

    return (passed==6) ? 0 : 1;
}

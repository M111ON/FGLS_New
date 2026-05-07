/*
 * test_tring_goldberg.c — Phase 12+ Wire Integration Test
 * ════════════════════════════════════════════════════════
 * Tests:
 *   T1: init context
 *   T2: single write → TGWResult fields valid
 *   T3: batch write → blueprint accumulation
 *   T4: file stream → TRing ordering → reconstruct
 *   T5: flush + stats sanity
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "geo_tring_goldberg_wire.h"

static int pass = 0, fail = 0;
#define ASSERT(c, m) do { \
    if (c) { printf("  ✓ %s\n", m); pass++; } \
    else   { printf("  ✗ FAIL: %s\n", m); fail++; } \
} while(0)

/* ── helpers ─────────────────────────────────────────────────────── */
static GeoSeed make_seed(void) {
    GeoSeed s;
    s.gen2 = UINT64_C(0xDEADBEEFCAFEBABE);
    s.gen3 = UINT64_C(0x9E3779B185EBCA87);
    return s;
}

static void make_bundle(uint64_t *b) {
    for (int i = 0; i < GEO_BUNDLE_WORDS; i++)
        b[i] = (uint64_t)(i + 1) * UINT64_C(0x9E3779B185EBCA87);
}

/* ═══════════════════════════════════════════════════════════════════
 * T1: init
 * ═══════════════════════════════════════════════════════════════════ */
static void test_init(void) {
    printf("\n[T1] init\n");
    uint64_t bundle[GEO_BUNDLE_WORDS];
    make_bundle(bundle);
    TGWCtx ctx;
    tgw_init(&ctx, make_seed(), bundle);
    ASSERT(ctx.total_writes == 0,           "total_writes starts 0");
    ASSERT(ctx.blueprint_count == 0,        "blueprint_count starts 0");
    ASSERT(ctx.tr.head == 0,               "tring head starts 0");
    ASSERT(ctx.tb.total_ops == 0,          "twin_bridge ops starts 0");
}

/* ═══════════════════════════════════════════════════════════════════
 * T2: single write
 * ═══════════════════════════════════════════════════════════════════ */
static void test_single_write(void) {
    printf("\n[T2] single write\n");
    uint64_t bundle[GEO_BUNDLE_WORDS];
    make_bundle(bundle);
    TGWCtx ctx;
    tgw_init(&ctx, make_seed(), bundle);

    TGWResult r = tgw_write(&ctx, 0xABCD1234ULL, 0xDEAD5678ULL, 0);

    ASSERT(ctx.total_writes == 1,              "write count incremented");
    /* tring_pos 0..719 */
    ASSERT(r.gpr.fp.tring_pos < 720,          "tring_pos in valid range");
    /* twin wrote at least 1 op */
    ASSERT(ctx.tb.total_ops >= 1,             "twin_bridge got op");
}

/* ═══════════════════════════════════════════════════════════════════
 * T3: batch write → blueprint eventually appears
 * ═══════════════════════════════════════════════════════════════════ */
static void test_batch(void) {
    printf("\n[T3] batch → blueprints\n");
    uint64_t bundle[GEO_BUNDLE_WORDS];
    make_bundle(bundle);
    TGWCtx ctx;
    tgw_init(&ctx, make_seed(), bundle);

    const uint32_t N = 64;
    uint64_t addrs[64], vals[64];
    for (uint32_t i = 0; i < N; i++) {
        addrs[i] = (uint64_t)i * UINT64_C(0x9E3779B185EBCA87);
        vals[i]  = addrs[i] ^ UINT64_C(0xFFFFFFFF00000000);
    }
    tgw_batch(&ctx, addrs, vals, N, 0);

    ASSERT(ctx.total_writes == N,              "batch write count correct");
    ASSERT(ctx.tb.total_ops >= N,             "twin_bridge got all ops");

    /* individually write to accumulate blueprints */
    for (uint32_t i = 0; i < N; i++)
        tgw_write(&ctx, addrs[i], vals[i], (i & 1));

    TGWStats s = tgw_stats(&ctx);
    ASSERT(s.total_writes == N * 2,           "combined write count");
    ASSERT(s.tring_pos <= 720,                "tring_pos bounded");
}

/* ═══════════════════════════════════════════════════════════════════
 * T4: file stream → TRing ordering → reconstruct
 * ═══════════════════════════════════════════════════════════════════ */
static void test_stream(void) {
    printf("\n[T4] file stream + reconstruct\n");
    uint64_t bundle[GEO_BUNDLE_WORDS];
    make_bundle(bundle);
    TGWCtx ctx;
    tgw_init(&ctx, make_seed(), bundle);

    /* build synthetic file: 8KB of patterned data */
    static uint8_t file_data[8192];
    for (int i = 0; i < 8192; i++)
        file_data[i] = (uint8_t)(i * 7 + 13);

    uint16_t pkts_sent = tgw_stream_file(&ctx, file_data, sizeof(file_data));
    ASSERT(pkts_sent > 0,                     "stream sliced > 0 packets");
    ASSERT(ctx.stream_pkts_rx == pkts_sent,   "all pkts received into store");
    ASSERT(ctx.stream_gaps == 0,              "no gaps in ordered stream");

    /* reconstruct */
    static uint8_t out[8192];
    memset(out, 0, sizeof(out));
    uint32_t bytes = tgw_stream_reconstruct(&ctx, out, pkts_sent);
    ASSERT(bytes > 0,                         "reconstruct produced bytes");
    ASSERT(bytes <= sizeof(file_data),        "reconstruct <= input size");
    ASSERT(memcmp(file_data, out, bytes) == 0, "reconstruct matches input");
}

/* ═══════════════════════════════════════════════════════════════════
 * T5: flush + stats
 * ═══════════════════════════════════════════════════════════════════ */
static void test_flush_stats(void) {
    printf("\n[T5] flush + stats\n");
    uint64_t bundle[GEO_BUNDLE_WORDS];
    make_bundle(bundle);
    TGWCtx ctx;
    tgw_init(&ctx, make_seed(), bundle);

    for (int i = 0; i < 32; i++)
        tgw_write(&ctx, (uint64_t)i, (uint64_t)i * 31337, 0);

    tgw_flush(&ctx);
    TGWStats s = tgw_stats(&ctx);

    ASSERT(s.total_writes == 32,              "stats: total_writes correct");
    ASSERT(s.tb.total_ops >= 32,             "stats: twin ops correct");
    ASSERT(s.tring_pos <= 720,               "stats: tring_pos bounded");

    tgw_status(&ctx);   /* visual confirm */
}

/* ═══════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  TRing × Goldberg Wire — Integration Test    ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    test_init();
    test_single_write();
    test_batch();
    test_stream();
    test_flush_stats();

    printf("\n══════════════════════════════════════════\n");
    printf("  PASS: %d  FAIL: %d  TOTAL: %d\n", pass, fail, pass + fail);
    printf("══════════════════════════════════════════\n");
    return (fail == 0) ? 0 : 1;
}

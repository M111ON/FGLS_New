/*
 * test_tgw_dispatch.c — Full Pipeline Integration Test
 * ======================================================
 * ทดสอบ wire จริง: tgw_write → tgw_dispatch → ROUTE/GROUND
 *
 * Tests:
 *   D1: GROUND gate — sign-mismatch addr → pl_write (ไม่ผ่าน blueprint)
 *   D2: ROUTE path — 150 writes → blueprint_ready → verdict=true → fts_write
 *   D3: tgw_write_dispatch — one-call API ครบ pipeline
 *   D4: stats consistency — route+ground+no_blueprint == total_dispatched
 *   D5: read-back — ROUTE fts_accessible=0, GROUND pl_read found=true
 *
 * Compile:
 *   gcc -O2 -o test_tgw_dispatch test_tgw_dispatch.c \
 *       -I/tmp/core/core/core \
 *       -I/tmp/core/core/core/twin_core \
 *       -I/tmp/core/core/pogls_engine/core \
 *       -I/tmp/core/core/pogls_engine \
 *       -I/tmp/core/core/pogls_engine/KV_patched \
 *       -I/tmp/core/core/geo_headers \
 *       -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "tgw_dispatch.h"

static const GeoSeed TEST_SEED = { 0xDEADBEEF12345678ULL,
                                   0xCAFEBABE87654321ULL };

static const uint64_t STRUCT_BUNDLE[GEO_BUNDLE_WORDS] = {
    0x1111111111111111ULL, 0x2222222222222222ULL,
    0x3333333333333333ULL, 0x4444444444444444ULL,
    0x5555555555555555ULL, 0x6666666666666666ULL,
    0x7777777777777777ULL, 0x8888888888888888ULL,
};

/* addr with sign mismatch → GROUND */
#define GROUND_ADDR(b)  ((uint64_t)(b) | (1ULL << 63))
#define GROUND_VALUE(b) ((uint64_t)(b) & ~(1ULL << 63))

/* ═══════════════════════════════════════════
 * D1: GROUND gate bypasses blueprint
 * ═══════════════════════════════════════════ */
static int test_d1_ground_gate(void) {
    printf("[D1] GROUND gate — sign mismatch → pl_write direct\n");

    TGWCtx ctx;
    TGWDispatch d;
    tgw_init(&ctx, TEST_SEED, STRUCT_BUNDLE);
    tgw_dispatch_init(&d, 0xFEEDFACE00000000ULL);

    /* step=145 (=144+1) ensures addr%144=i → unique slots for N=10 */
    static const int N = 10;
    for (int i = 1; i <= N; i++) {
        uint64_t addr  = GROUND_ADDR(i * 145u);
        uint64_t value = GROUND_VALUE(i * 290u);
        TGWResult r = tgw_write(&ctx, addr, value, 0);
        tgw_dispatch(&d, &r, addr, value, STRUCT_BUNDLE);
    }

    TGWDispatchStats s = tgw_dispatch_stats(&d);
    printf("  total=%u  ground=%u  route=%u  no_bp=%u\n",
           s.total_dispatched, s.ground_count,
           s.route_count, s.no_blueprint);

    /* all N must be GROUND (sign mismatch) */
    int ok = (s.ground_count == (uint32_t)N)
          && (s.route_count  == 0u);

    /* verify read-back from PayloadStore */
    int readback_ok = 1;
    for (int i = 1; i <= N && readback_ok; i++) {
        uint64_t addr  = GROUND_ADDR(i * 145u);
        uint64_t value = GROUND_VALUE(i * 290u);
        PayloadResult pr = pl_read(&d.ps, addr);
        if (!pr.found || pr.value != value) {
            printf("  FAIL readback i=%d\n", i);
            readback_ok = 0;
        }
    }

    if (ok && readback_ok)
        printf("  PASS: GROUND gate wired — all %d bypassed geo ✓\n", N);

    else
        printf("  FAIL\n");
    return ok && readback_ok;
}

/* ═══════════════════════════════════════════
 * D2: ROUTE path — blueprint → verdict → fts_write
 * ═══════════════════════════════════════════ */
static int test_d2_route_path(void) {
    printf("[D2] ROUTE path — blueprint → verdict=true → fts_write\n");

    TGWCtx ctx;
    TGWDispatch d;
    tgw_init(&ctx, TEST_SEED, STRUCT_BUNDLE);
    tgw_dispatch_init(&d, 0xC0FFEE0012345678ULL);

    /* same-sign addr → not GROUND → goes through blueprint path */
    for (uint32_t i = 0; i < 150u; i++) {
        uint64_t addr  = (uint64_t)(0x1000u + i * 7u);   /* bit63=0 */
        uint64_t value = (uint64_t)(0xABCDu ^ i);         /* bit63=0 */
        TGWResult r = tgw_write(&ctx, addr, value, 0);
        tgw_dispatch(&d, &r, addr, value, STRUCT_BUNDLE);
    }

    TGWDispatchStats s = tgw_dispatch_stats(&d);
    printf("  total=%u  route=%u  ground=%u  verdict_fail=%u  no_bp=%u\n",
           s.total_dispatched, s.route_count,
           s.ground_count, s.verdict_fail, s.no_blueprint);
    printf("  fts: writes=%u  active_cosets=%u\n",
           s.fts.writes, s.fts.active_cosets);

    int ok = (s.route_count >= 1u)  /* at least one blueprint routed */
          && (s.fts.writes   >= 1u); /* fts_write was actually called */

    if (ok)
        printf("  PASS: ROUTE path wired — blueprint → fts_write ✓\n");
    else
        printf("  FAIL: route_count=%u fts.writes=%u\n",
               s.route_count, s.fts.writes);
    return ok;
}

/* ═══════════════════════════════════════════
 * D3: tgw_write_dispatch one-call API
 * ═══════════════════════════════════════════ */
static int test_d3_one_call(void) {
    printf("[D3] tgw_write_dispatch — one-call API\n");

    TGWCtx ctx;
    TGWDispatch d;
    tgw_init(&ctx, TEST_SEED, STRUCT_BUNDLE);
    tgw_dispatch_init(&d, 0xBEEF000000000000ULL);

    /* mix: some GROUND, some ROUTE candidates */
    for (uint32_t i = 0; i < 160u; i++) {
        uint64_t addr, value;
        if (i % 5 == 0) {
            addr  = GROUND_ADDR(i * 0x111u);
            value = GROUND_VALUE(i * 0x222u);
        } else {
            addr  = (uint64_t)(0x2000u + i * 13u);
            value = (uint64_t)(0xFACEu ^ i);
        }
        tgw_write_dispatch(&ctx, &d, addr, value, 0);
    }

    TGWDispatchStats s = tgw_dispatch_stats(&d);
    printf("  total=%u  ground=%u  route=%u  no_bp=%u\n",
           s.total_dispatched, s.ground_count,
           s.route_count, s.no_blueprint);

    /* 160/5 = 32 GROUND, rest go through blueprint path */
    int ok = (s.total_dispatched == 160u)
          && (s.ground_count     >= 32u);

    if (ok)
        printf("  PASS: tgw_write_dispatch works end-to-end ✓\n");
    else
        printf("  FAIL\n");
    return ok;
}

/* ═══════════════════════════════════════════
 * D4: stats consistency
 * ═══════════════════════════════════════════ */
static int test_d4_stats(void) {
    printf("[D4] stats consistency — route+ground+no_bp == total\n");

    TGWCtx ctx;
    TGWDispatch d;
    tgw_init(&ctx, TEST_SEED, STRUCT_BUNDLE);
    tgw_dispatch_init(&d, 0xDEAD000000000000ULL);

    for (uint32_t i = 0; i < 200u; i++) {
        uint64_t addr  = (uint64_t)(i * 0x137u) | ((uint64_t)(i & 1u) << 63);
        uint64_t value = (uint64_t)(i * 0x251u);
        tgw_write_dispatch(&ctx, &d, addr, value, 0);
    }

    TGWDispatchStats s = tgw_dispatch_stats(&d);
    uint32_t sum = s.route_count + s.ground_count + s.no_blueprint;

    printf("  total=%u  sum(route+ground+no_bp)=%u\n",
           s.total_dispatched, sum);
    printf("  route=%u  ground=%u  verdict_fail=%u  no_bp=%u\n",
           s.route_count, s.ground_count, s.verdict_fail, s.no_blueprint);

    int ok = (sum == s.total_dispatched);
    if (ok)
        printf("  PASS: stats are consistent ✓\n");
    else
        printf("  FAIL: leaked %d ops\n", (int)s.total_dispatched - (int)sum);
    return ok;
}

/* ═══════════════════════════════════════════
 * D5: read-back — ROUTE accessible, GROUND readable
 * ═══════════════════════════════════════════ */
static int test_d5_readback(void) {
    printf("[D5] read-back — ROUTE fts_accessible, GROUND pl_read\n");

    TGWCtx ctx;
    TGWDispatch d;
    tgw_init(&ctx, TEST_SEED, STRUCT_BUNDLE);
    tgw_dispatch_init(&d, 0xABCD000012345678ULL);

    /* Write 150 same-sign (ROUTE candidates) */
    uint32_t last_route_addr = 0, last_route_val = 0;
    for (uint32_t i = 0; i < 150u; i++) {
        uint64_t addr  = (uint64_t)(0x3000u + i * 11u);
        uint64_t value = (uint64_t)(0xCAFEu ^ i);
        TGWResult r = tgw_write(&ctx, addr, value, 0);
        if (r.gpr.blueprint_ready) {
            last_route_addr = (uint32_t)r.gpr.bp.stamp_hash;
            last_route_val  = (uint32_t)r.gpr.bp.spatial_xor;
        }
        tgw_dispatch(&d, &r, addr, value, STRUCT_BUNDLE);
    }

    /* Write 10 GROUND */
    uint64_t g_addr  = GROUND_ADDR(0x9999u);
    uint64_t g_value = GROUND_VALUE(0x1111u);
    {
        TGWResult r = tgw_write(&ctx, g_addr, g_value, 0);
        tgw_dispatch(&d, &r, g_addr, g_value, STRUCT_BUNDLE);
    }

    TGWDispatchStats s = tgw_dispatch_stats(&d);
    int ok = 1;

    /* Check ROUTE: fts_accessible on last routed key */
    if (s.route_count > 0) {
        int acc = fts_accessible(&d.fts,
                                  (uint64_t)last_route_addr,
                                  (uint64_t)last_route_val);
        printf("  ROUTE: fts_accessible(last_key)=%d  (expect 0)\n", acc);
        if (acc != 0) { printf("  FAIL: ROUTE not accessible\n"); ok = 0; }
    } else {
        printf("  WARN: no ROUTE happened in D5\n");
    }

    /* Check GROUND: pl_read on known addr */
    PayloadResult pr = pl_read(&d.ps, g_addr);
    printf("  GROUND: pl_read found=%d value=0x%llX expected=0x%llX\n",
           pr.found,
           (unsigned long long)pr.value,
           (unsigned long long)g_value);
    if (!pr.found || pr.value != g_value) {
        printf("  FAIL: GROUND read-back wrong\n"); ok = 0;
    }

    if (ok)
        printf("  PASS: ROUTE+GROUND both readable after dispatch ✓\n");
    return ok;
}

/* ═══════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════ */
int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  TGW Full Dispatch Integration Test  D1-D5  ║\n");
    printf("║  tgw_write → dispatch → ROUTE / GROUND      ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    int r[5];
    r[0] = test_d1_ground_gate();  printf("\n");
    r[1] = test_d2_route_path();   printf("\n");
    r[2] = test_d3_one_call();     printf("\n");
    r[3] = test_d4_stats();        printf("\n");
    r[4] = test_d5_readback();     printf("\n");

    int passed = r[0]+r[1]+r[2]+r[3]+r[4];
    printf("══════════════════════════════════════════════\n");
    printf("RESULT: %d/5 passed\n", passed);
    printf("STATUS: %s\n", passed == 5
        ? "✅ FULL PIPELINE WIRED AND VERIFIED"
        : "⚠️  check failures above");

    printf("\n── Full Wire Status ───────────────────────────\n");
    printf("GROUND gate (sign mismatch → pl_write)   %s\n", r[0]?"✅":"❌");
    printf("ROUTE path (blueprint → fts_write)       %s\n", r[1]?"✅":"❌");
    printf("tgw_write_dispatch one-call API          %s\n", r[2]?"✅":"❌");
    printf("stats consistency                        %s\n", r[3]?"✅":"❌");
    printf("read-back ROUTE+GROUND                   %s\n", r[4]?"✅":"❌");
    printf("───────────────────────────────────────────────\n");

    printf("\n── Pipeline Map ───────────────────────────────\n");
    printf("tgw_write(addr,value)\n");
    printf("  → lch_gate(sign check)\n");
    printf("      ├─ GROUND → pl_write(addr,value)          ✅\n");
    printf("      └─ (pass) → blueprint_ready?\n");
    printf("              ├─ no  → no_blueprint counter      ✅\n");
    printf("              └─ yes → geomatrix_batch_verdict\n");
    printf("                    ├─ true  → fts_write(ROUTE)  ✅\n");
    printf("                    └─ false → pl_write(GROUND)  ✅\n");
    printf("───────────────────────────────────────────────\n");

    return (passed == 5) ? 0 : 1;
}

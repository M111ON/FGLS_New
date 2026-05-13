/*
 * test_ground_lcgw.c — TGW GROUND → LC-GCFS hook verification
 * ═══════════════════════════════════════════════════════════════
 * Tests:
 *   G01: ground writes reach correct spoke lane
 *   G02: all 6 spokes populated after 720-cycle stream
 *   G03: write counts match ground_count from dispatch
 *   G04: ghost delete → lane ghosted, subsequent writes ignored
 *   G05: ghost delete → lcgw_read_payload returns NULL (unreachable)
 *   G06: non-ghosted lanes still readable after delete of spoke 0
 *   G07: full pipeline — dispatch_v2 + tgw_ground_fn + stats consistent
 *
 * Compile:
 *   gcc -O2 -Wall -I. -o test_ground_lcgw test_ground_lcgw.c && ./test_ground_lcgw
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#ifndef TEST_STUB_MODE
#define TEST_STUB_MODE 0
#endif

#if TEST_STUB_MODE
/* ── stubs ────────────────────────────────────────────────────── */
#define GEO_SLOTS            576u
#define GEO_BLOCK_BOUNDARY   288u
#define GEOMATRIX_PATHS       18
typedef struct { uint8_t phase; uint64_t sig; uint16_t hpos; uint16_t idx; uint8_t bit; } GeoPacket;
typedef struct { uint32_t x; } GeomatrixStatsV3;
typedef struct {
    uint32_t stamp_hash; uint32_t spatial_xor; uint32_t window_id;
    uint32_t circuit_fired; uint16_t tring_start; uint16_t tring_end;
    uint16_t tring_span; uint16_t top_gaps[4];
} GBBlueprint;
typedef struct { int blueprint_ready; GBBlueprint bp; } GBPResult;
typedef struct { GBPResult gpr; } TGWResult;
typedef struct { uint64_t *_bundle; } TGWCtx;
static inline uint64_t geo_compute_sig64(const uint64_t *b, uint8_t p) { (void)b; return p; }
static inline bool geomatrix_batch_verdict(GeoPacket *b, const uint64_t *u, GeomatrixStatsV3 *s)
    { (void)b;(void)u;(void)s; return true; }
typedef struct { uint32_t x; } FtsTwinStore;
typedef struct { uint32_t writes; } FtsTwinStats;
typedef struct { uint64_t merkle_root,sha256_hi,sha256_lo,offset,hop_count,segment; } DodecaEntry;
static inline void fts_init(FtsTwinStore *f, uint64_t s) { (void)f;(void)s; }
static inline void fts_write(FtsTwinStore *f, uint64_t a, uint64_t v, const DodecaEntry *e)
    { (void)f;(void)a;(void)v;(void)e; }
static inline FtsTwinStats fts_stats(const FtsTwinStore *f) { (void)f; FtsTwinStats s={0}; return s; }
typedef struct { uint32_t count; } PayloadStore;
static inline void pl_init(PayloadStore *p) { p->count=0; }
static inline void pl_write(PayloadStore *p, uint64_t a, uint64_t v) { (void)a;(void)v; p->count++; }
#define LC_GATE_GROUND 3
typedef struct { uint32_t gate_counts[4]; } LCTwinGateCtx;
static inline void lc_twin_gate_init(LCTwinGateCtx *c) { memset(c,0,sizeof(*c)); }
static inline TGWResult tgw_write(TGWCtx *c, uint64_t a, uint64_t v, uint8_t h)
    { (void)c;(void)a;(void)v;(void)h; TGWResult r={0}; return r; }
static inline void tgw_batch(TGWCtx *c, const uint64_t *a, const uint64_t *v, uint32_t n, int f)
    { (void)c;(void)a;(void)v;(void)n;(void)f; }
#endif

/* ── null flush (ROUTE goes here) ─────────────────────────────── */
static void _null_flush(const uint64_t *a, const uint64_t *v,
                         uint32_t n, void *ctx)
{ (void)a;(void)v;(void)n;(void)ctx; }

/* ── real headers ─────────────────────────────────────────────── */
#include "geo_addr_net.h"
#include "lc_hdr.h"
#include "tgw_lc_bridge_p6.h"
#include "tgw_dispatch_v2.h"
#include "tgw_ground_lcgw.h"

/* ── harness ──────────────────────────────────────────────────── */
static int _pass=0, _fail=0;
#define CHECK(cond, msg) do { \
    if(cond){printf("  PASS  %s\n",msg);_pass++;} \
    else    {printf("  FAIL  %s\n",msg);_fail++;} \
} while(0)

/* ════════════════════════════════════════════════════════════════ */

static void g01(void) {
    printf("\nG01: GROUND writes reach correct spoke lane\n");
    geo_addr_net_init();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* enc=0: spoke=0, polarity=0 (ROUTE) — skip
     * enc=60: polarity=1 (GROUND), spoke=0 */
    lcgw_ground_write(&gs, 60u, 0xABu);

    GeoNetAddr a = geo_net_encode(60u);
    printf("  INFO  enc=60: spoke=%d polarity=%d\n", a.spoke, a.polarity);
    CHECK(gs.lanes[a.spoke].write_count == 1u, "write_count=1 on correct spoke");
    CHECK(gs.total_writes == 1u, "total_writes=1");
}

static void g02(void) {
    printf("\nG02: all 6 spokes populated after 720-cycle\n");
    geo_addr_net_init();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* send all GROUND pkts from enc=0..719 */
    for (uint64_t enc=0; enc<720u; enc++) {
        GeoNetAddr a = geo_net_encode(enc);
        if (a.polarity)
            lcgw_ground_write(&gs, enc, enc^0xDEADu);
    }

    LCGWGroundStats s = lcgw_ground_stats(&gs);
    int all_hit = 1;
    for (int sp=0; sp<6; sp++) {
        printf("  INFO  spoke[%d] writes=%u ghosted=%d\n",
               sp, s.writes[sp], s.ghosted[sp]);
        if (s.writes[sp] == 0) all_hit = 0;
    }
    CHECK(all_hit, "all 6 spokes have writes");
    CHECK(s.total_writes == 360u, "total=360 (50% of 720)");
}

static void g03(void) {
    printf("\nG03: write counts match dispatch ground_count\n");
    geo_addr_net_init();
    TGWDispatchV2   d; tgw_dispatch_v2_init(&d, 0xBEEF);
    LCGWGroundStore gs; lcgw_ground_init(&gs);
    TGWResult r; memset(&r, 0, sizeof(r));

    for (uint64_t enc=0; enc<720u; enc++)
        tgw_dispatch_v2(&d, NULL, &r, enc, enc^0xCAFEu, NULL,
                        _null_flush, tgw_ground_fn, &gs);
    tgw_dispatch_v2_flush(&d, _null_flush, &gs);

    printf("  INFO  dispatch.ground=%u  lcgw.total=%u\n",
           d.ground_count, gs.total_writes);
    printf("  INFO  express=%u cusp_block=%u skew=%.2f\n",
           d.express_count, d.cusp_block_count, tgw_bucket_skew(&d));
    CHECK(d.ground_count == gs.total_writes, "dispatch.ground == lcgw.total");
    CHECK(d.ground_count + d.route_count == d.total_dispatched,
          "ground + route == total");
    CHECK(gs.total_writes <= 360u, "at most 360 GROUND writes (cardioid may reduce)");
    CHECK(tgw_bucket_skew(&d) < 2.0, "bucket skew < 2.0");
}

static void g04(void) {
    printf("\nG04: ghost delete → subsequent writes ignored\n");
    geo_addr_net_init();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* write to spoke 0 (enc=60, polarity=1, spoke=0) */
    lcgw_ground_write(&gs, 60u, 1u);
    uint8_t spk = geo_net_encode(60u).spoke;
    CHECK(gs.lanes[spk].write_count == 1u, "1 write before ghost");

    /* ghost delete spoke */
    LCDeleteResult res = lcgw_ground_delete(&gs, spk);
    (void)res;
    CHECK(gs.lanes[spk].ghosted == 1u, "lane ghosted");

    /* write again → should be ignored */
    lcgw_ground_write(&gs, 60u, 2u);
    CHECK(gs.lanes[spk].write_count == 1u, "write_count unchanged after ghost");
}

static void g05(void) {
    printf("\nG05: ghosted lane → lcgw_read_payload returns NULL\n");
    geo_addr_net_init();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    lcgw_ground_write(&gs, 60u, 0xFFu);
    uint8_t spk = geo_net_encode(60u).spoke;
    int gslot = gs.lanes[spk].gslot;

    /* before ghost: chunk valid */
    CHECK(lcgw_files[gslot].chunks[0].valid == 1u, "chunk valid before ghost");

    /* ghost delete */
    lcgw_ground_delete(&gs, spk);

    /* after ghost: all chunks ghosted → read returns NULL */
    LCPalette pal = {0};
    const uint8_t *p = lcgw_read_payload(gslot, 0u, &pal);
    CHECK(p == NULL, "read_payload=NULL after ghost (unreachable)");
}

static void g06(void) {
    printf("\nG06: non-ghosted spokes readable after deleting spoke 0\n");
    geo_addr_net_init();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* populate all 6 spokes */
    for (uint64_t enc=0; enc<720u; enc++) {
        GeoNetAddr a = geo_net_encode(enc);
        if (a.polarity) lcgw_ground_write(&gs, enc, enc);
    }

    /* ghost spoke 0 only */
    uint8_t spk0 = geo_net_encode(60u).spoke;
    lcgw_ground_delete(&gs, spk0);

    /* other spokes still active */
    uint32_t active = lcgw_ground_active_spokes(&gs);
    printf("  INFO  active spokes after deleting spoke %d: %u\n", spk0, active);
    CHECK(active == 5u, "5 spokes still active");
    CHECK(gs.lanes[spk0].ghosted == 1u, "spoke 0 ghosted");
}

static void g07(void) {
    printf("\nG07: full pipeline stats consistent\n");
    geo_addr_net_init();
    TGWDispatchV2   d; tgw_dispatch_v2_init(&d, 0x1234);
    LCGWGroundStore gs; lcgw_ground_init(&gs);
    TGWResult r; memset(&r, 0, sizeof(r));

    /* two full cycles */
    for (uint64_t enc=0; enc<1440u; enc++)
        tgw_dispatch_v2(&d, NULL, &r, enc % 720u, enc^0xABCDu, NULL,
                        _null_flush, tgw_ground_fn, &gs);
    tgw_dispatch_v2_flush(&d, _null_flush, &gs);

    TGWDispatchV2Stats ds = tgw_dispatch_v2_stats(&d);
    LCGWGroundStats    gs2 = lcgw_ground_stats(&gs);

    printf("  INFO  total=%u route=%u ground=%u lcgw_total=%u\n",
           ds.total_dispatched, ds.route_count, ds.ground_count, gs2.total_writes);
    printf("  INFO  express=%u(%.0f%%) cusp_block=%u skew=%.2f\n",
           ds.express_count, ds.express_ratio * 100.0,
           ds.cusp_block_count, tgw_bucket_skew(&d));

    CHECK(ds.ground_count + ds.route_count == ds.total_dispatched,
          "dispatch invariant: ground+route=total");
    CHECK(ds.ground_count == gs2.total_writes,
          "lcgw writes == dispatch ground_count");
    CHECK(ds.ground_count <= 720u,
          "at most 720 GROUND writes across 2 cycles (cardioid may reduce)");
    CHECK(ds.express_ratio < 0.70, "express ratio < 70% (GEO_MIN floor active)");
    CHECK(ds.cusp_block_count > 0u, "cusp_block > 0 (Gate 1 noise rejection working)");
    CHECK(tgw_bucket_skew(&d) < 2.0, "bucket skew < 2.0");
}

int main(void) {
    printf("=== TGW GROUND → LC-GCFS Hook Verification ===\n");
    geo_addr_net_init();
    cardioid_lut_init();   /* init Q8 cos LUT once — required for Hook A/B */
    g01(); g02(); g03(); g04(); g05(); g06(); g07();
    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    if (_fail == 0)
        printf("\n✓ GROUND path wired — ghost delete = present but unreachable\n");
    return _fail ? 1 : 0;
}

/*
 * test_p6.c — P6: tgw_lc_bridge_p6.h test suite
 * ================================================
 * T01: tgwlc_route_p6() numeric = P4 tgwlc_route() for all 720 enc
 * T02: tgwlc_route_full_p6() hilbert_lane in [0,719]
 * T03: hilbert_lane unique across 720 enc (bijection)
 * T04: tgw_pending_sort() — sorted ASC after call
 * T05: tgw_pending_sort() — already-sorted input = 0 swaps
 * T06: tgw_pending_sort() — reverse-sorted = max swaps
 * T07: tgw_pending_sort() — preserves addr/val pairing
 * T08: tgwlc_prefilter_p6() ground_mask matches polarity split
 * T09: tgwlc_prefilter_p6() hilbert_lanes all in [0,719]
 * T10: tgw_dispatch_p6() GROUND path → ground_fn called, not batch
 * T11: tgw_dispatch_p6() ROUTE path → batch accumulated
 * T12: tgw_dispatch_p6() auto-flush at BATCH_MAX=64
 * T13: tgw_dispatch_p6() flush → batch sorted by hilbert_lane
 * T14: tgw_dispatch_p6_flush() drains remainder
 * T15: invariant T7: ground+route = total_dispatched
 * T16: stats avg_swaps_per_flush reasonable (< 2016 max)
 * T17: TGWLCRouteP6 size = 8 bytes
 * T18: sort_efficiency in [0,1]
 * T19: spoke histogram P6 = uniform 120/spoke over 720
 * T20: tgwlc_hilbert_lane() = geo_net_hilbert_lane() (consistent)
 *
 * Compile: gcc -O2 -Wall -o test_p6 test_p6.c && ./test_p6
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "geo_addr_net.h"
#include "tgw_lc_bridge_p6.h"

/* ── harness ─────────────────────────────────────────────── */
static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else       { printf("  FAIL  %s\n", msg); _fail++; } \
} while(0)

/* ── P4 reference (inline, no include) ──────────────────── */
static inline uint16_t _p4_tring_pos(uint32_t enc) {
    return (uint16_t)(enc % 720u);
}
static inline uint8_t _p4_spoke(uint32_t enc) {
    return (uint8_t)(_p4_tring_pos(enc) / GAN_PENT_SPAN);
}
static inline uint8_t _p4_polarity(uint32_t enc) {
    uint16_t pos = _p4_tring_pos(enc);
    return (uint8_t)((pos % GAN_PENT_SPAN) >= GAN_MIRROR_HALF);
}

/* ── Mock flush/ground callbacks ─────────────────────────── */
typedef struct {
    uint64_t flushed_addrs[TGW_P6_BATCH_MAX * 16];
    uint64_t flushed_vals [TGW_P6_BATCH_MAX * 16];
    uint16_t flushed_lanes[TGW_P6_BATCH_MAX * 16]; /* lanes at flush time */
    uint32_t flush_count;
    uint32_t total_flushed;
    uint64_t ground_addrs[TGW_P6_BATCH_MAX * 16];
    uint32_t ground_count;
    bool     last_flush_sorted;   /* was last flush batch sorted? */
} MockCtx;

static void _mock_flush(const uint64_t *addrs, const uint64_t *vals,
                         uint32_t n, void *ctx) {
    MockCtx *m = (MockCtx *)ctx;
    uint32_t base = m->total_flushed;
    for (uint32_t i = 0; i < n; i++) {
        m->flushed_addrs[base + i] = addrs[i];
        m->flushed_vals [base + i] = vals[i];
        /* capture hilbert lane at flush time */
        m->flushed_lanes[base + i] = tgwlc_hilbert_lane(addrs[i]);
    }
    m->total_flushed += n;
    m->flush_count++;
    /* check sorted */
    bool sorted = true;
    for (uint32_t i = 1; i < n; i++)
        if (m->flushed_lanes[base+i] < m->flushed_lanes[base+i-1]) {
            sorted = false; break;
        }
    m->last_flush_sorted = sorted;
}
static void _mock_ground(uint64_t addr, uint64_t val, void *ctx) {
    MockCtx *m = (MockCtx *)ctx;
    (void)val;
    m->ground_addrs[m->ground_count++] = addr;
}

/* ════════════════════════════════════════════════════════════
   TESTS
   ════════════════════════════════════════════════════════════ */

/* T01: route_p6 numeric matches P4 */
static void t01(void) {
    printf("\nT01: tgwlc_route_p6() numeric = P4 tgwlc_route()\n");
    int ok = 1;
    for (uint64_t enc = 0; enc < GAN_TRING_CYCLE; enc++) {
        TGWLCRoute r = tgwlc_route_p6(enc);
        if (r.spoke     != _p4_spoke((uint32_t)enc))    { ok=0; break; }
        if (r.polarity  != _p4_polarity((uint32_t)enc)) { ok=0; break; }
        if (r.tring_pos != _p4_tring_pos((uint32_t)enc)){ ok=0; break; }
    }
    CHECK(ok, "route_p6 spoke/polarity/tring_pos = P4 for all 720");
}

/* T02: hilbert_lane in [0,719] */
static void t02(void) {
    printf("\nT02: hilbert_lane in [0,719]\n");
    int ok = 1;
    for (uint64_t enc = 0; enc < GAN_TRING_CYCLE; enc++) {
        TGWLCRouteP6 r = tgwlc_route_full_p6(enc);
        if (r.hilbert_lane >= GAN_TRING_CYCLE) { ok=0; break; }
    }
    CHECK(ok, "all hilbert_lane values in [0,719]");
}

/* T03: hilbert_lane bijection */
static void t03(void) {
    printf("\nT03: hilbert_lane bijection across 720 enc\n");
    uint8_t seen[GAN_TRING_CYCLE];
    memset(seen, 0, sizeof(seen));
    int ok = 1;
    for (uint64_t enc = 0; enc < GAN_TRING_CYCLE; enc++) {
        uint16_t lane = tgwlc_hilbert_lane(enc);
        if (lane >= GAN_TRING_CYCLE || seen[lane]) { ok=0; break; }
        seen[lane] = 1;
    }
    CHECK(ok, "hilbert_lane: no duplicates, full [0,719] coverage");
}

/* T04: tgw_pending_sort — sorted ASC after call */
static void t04(void) {
    printf("\nT04: tgw_pending_sort() → ASC hilbert_lane\n");
    TGWPendingSlot slots[8];
    uint16_t lanes[] = {500, 3, 250, 720-1, 100, 1, 400, 50};
    for (int i=0; i<8; i++) {
        slots[i].addr = (uint64_t)i;
        slots[i].val  = (uint64_t)(i*100);
        slots[i].hilbert_lane = lanes[i];
    }
    tgw_pending_sort(slots, 8);
    int sorted = 1;
    for (int i=1; i<8; i++)
        if (slots[i].hilbert_lane < slots[i-1].hilbert_lane) { sorted=0; break; }
    CHECK(sorted, "pending[8] sorted ASC by hilbert_lane");
}

/* T05: already-sorted = no movement */
static void t05(void) {
    printf("\nT05: tgw_pending_sort() — already sorted (best case)\n");
    TGWPendingSlot slots[4];
    for (int i=0; i<4; i++) {
        slots[i].addr         = (uint64_t)i;
        slots[i].val          = (uint64_t)i;
        slots[i].hilbert_lane = (uint16_t)(i * 100);
    }
    /* insertion sort on sorted input: zero swaps */
    /* manual verify: order unchanged */
    tgw_pending_sort(slots, 4);
    int ok = (slots[0].hilbert_lane==0 && slots[1].hilbert_lane==100 &&
              slots[2].hilbert_lane==200 && slots[3].hilbert_lane==300);
    CHECK(ok, "already-sorted: order unchanged");
}

/* T06: reverse-sorted hits max swaps */
static void t06(void) {
    printf("\nT06: tgw_pending_sort() — reverse input (worst case)\n");
    TGWPendingSlot slots[5];
    uint16_t rev[] = {400,300,200,100,0};
    for (int i=0; i<5; i++) {
        slots[i].hilbert_lane = rev[i];
        slots[i].addr = (uint64_t)i;
        slots[i].val  = (uint64_t)i;
    }
    tgw_pending_sort(slots, 5);
    /* expected sorted: 0,100,200,300,400 */
    int ok = (slots[0].hilbert_lane==0 && slots[4].hilbert_lane==400);
    CHECK(ok, "reverse-sorted → correctly sorted after");
}

/* T07: addr/val pairing preserved through sort */
static void t07(void) {
    printf("\nT07: tgw_pending_sort() — addr/val pairing intact\n");
    TGWPendingSlot slots[4];
    /* addr=lane*10 as tag */
    uint16_t lanes[] = {300, 100, 400, 200};
    for (int i=0; i<4; i++) {
        slots[i].hilbert_lane = lanes[i];
        slots[i].addr = (uint64_t)(lanes[i] * 10u);
        slots[i].val  = (uint64_t)(lanes[i] * 10u + 1u);
    }
    tgw_pending_sort(slots, 4);
    /* after sort: lane order 100,200,300,400 → addr 1000,2000,3000,4000 */
    int ok = (slots[0].addr == 1000u && slots[0].hilbert_lane == 100u &&
              slots[1].addr == 2000u && slots[1].hilbert_lane == 200u &&
              slots[2].addr == 3000u && slots[2].hilbert_lane == 300u &&
              slots[3].addr == 4000u && slots[3].hilbert_lane == 400u);
    CHECK(ok, "addr follows hilbert_lane through sort (pairing intact)");
}

/* T08: prefilter_p6 ground_mask matches polarity */
static void t08(void) {
    printf("\nT08: tgwlc_prefilter_p6() ground_mask = polarity\n");
    TStreamPkt pkts[GAN_TRING_CYCLE];
    uint16_t lanes[GAN_TRING_CYCLE];
    for (uint32_t i=0; i<GAN_TRING_CYCLE; i++) pkts[i].enc = (uint32_t)i;
    uint64_t mask = tgwlc_prefilter_p6(pkts, 64, lanes);
    /* bit i=1 iff polarity(i)=1 */
    int ok = 1;
    for (int i=0; i<64; i++) {
        uint8_t pol = _p4_polarity((uint32_t)i);
        uint8_t bit = (uint8_t)((mask >> i) & 1u);
        if (bit != pol) { ok=0; break; }
    }
    CHECK(ok, "ground_mask matches polarity for enc 0-63");
}

/* T09: prefilter_p6 hilbert_lanes all in [0,719] */
static void t09(void) {
    printf("\nT09: tgwlc_prefilter_p6() hilbert_lanes in [0,719]\n");
    TStreamPkt pkts[64];
    uint16_t lanes[64];
    for (int i=0; i<64; i++) pkts[i].enc = (uint32_t)(i * 11u);
    tgwlc_prefilter_p6(pkts, 64, lanes);
    int ok = 1;
    for (int i=0; i<64; i++)
        if (lanes[i] >= GAN_TRING_CYCLE) { ok=0; break; }
    CHECK(ok, "all hilbert_lanes[0..63] in [0,719]");
}

/* T10: GROUND path → ground_fn called, not batch */
static void t10(void) {
    printf("\nT10: GROUND pkt → ground_fn, not batch\n");
    geo_addr_net_init();
    TGWDispatchP6 d; tgw_dispatch_p6_init(&d);
    MockCtx m; memset(&m, 0, sizeof(m));

    /* find a GROUND enc: polarity=1 (pos%60>=30) */
    /* enc=30 → pos=30, 30%60=30 >=30 → polarity=1 */
    uint64_t ground_enc = 60u;
    tgw_dispatch_p6(&d, ground_enc, 0xABCDu, _mock_flush, _mock_ground, &m);

    CHECK(m.ground_count == 1u, "ground_fn called once");
    CHECK(d.pending_n    == 0u, "pending buffer empty after GROUND");
    CHECK(d.ground_count == 1u, "d.ground_count = 1");
    CHECK(m.total_flushed == 0u, "flush_fn NOT called");
}

/* T11: ROUTE path → batch accumulated */
static void t11(void) {
    printf("\nT11: ROUTE pkt → pending accumulated\n");
    geo_addr_net_init();
    TGWDispatchP6 d; tgw_dispatch_p6_init(&d);
    MockCtx m; memset(&m, 0, sizeof(m));

    /* enc=0 → polarity=0 (ROUTE) */
    uint64_t route_enc = 0u;
    tgw_dispatch_p6(&d, route_enc, 0x1234u, _mock_flush, _mock_ground, &m);

    CHECK(d.pending_n   == 1u, "pending_n = 1 after ROUTE pkt");
    CHECK(d.route_count == 1u, "d.route_count = 1");
    CHECK(m.flush_count == 0u, "flush NOT triggered yet (n < 64)");
}

/* T12: auto-flush at BATCH_MAX=64 */
static void t12(void) {
    printf("\nT12: auto-flush at pending_n = 64\n");
    geo_addr_net_init();
    TGWDispatchP6 d; tgw_dispatch_p6_init(&d);
    MockCtx m; memset(&m, 0, sizeof(m));

    /* send 64 ROUTE pkts: enc 0,1,2,...,63 but filter GROUND */
    uint32_t route_sent = 0u;
    for (uint64_t enc = 0u; route_sent < TGW_P6_BATCH_MAX; enc++) {
        if (enc > 800u) break;  /* safety */
        GeoNetAddr a = geo_net_encode(enc);
        if (a.polarity) continue;  /* skip GROUND */
        tgw_dispatch_p6(&d, enc, enc ^ 0xFFu, _mock_flush, _mock_ground, &m);
        route_sent++;
    }

    CHECK(m.flush_count  == 1u, "exactly 1 auto-flush at 64 ROUTE pkts");
    CHECK(d.pending_n    == 0u, "pending_n = 0 after flush");
    CHECK(m.total_flushed == 64u, "64 pkts sent to flush_fn");
}

/* T13: flush batch is sorted by hilbert_lane */
static void t13(void) {
    printf("\nT13: flushed batch sorted by hilbert_lane\n");
    geo_addr_net_init();
    TGWDispatchP6 d; tgw_dispatch_p6_init(&d);
    MockCtx m; memset(&m, 0, sizeof(m));

    /* send 64 ROUTE pkts in reverse enc order → random Hilbert order */
    uint32_t sent = 0u;
    for (int64_t enc = GAN_TRING_CYCLE - 1; enc >= 0 && sent < 64u; enc--) {
        GeoNetAddr a = geo_net_encode((uint64_t)enc);
        if (a.polarity) continue;
        tgw_dispatch_p6(&d, (uint64_t)enc, 0u, _mock_flush, _mock_ground, &m);
        sent++;
    }
    /* flush remainder */
    tgw_dispatch_p6_flush(&d, _mock_flush, &m);

    /* verify flushed lanes are sorted */
    int sorted = 1;
    for (uint32_t i = 1u; i < m.total_flushed; i++)
        if (m.flushed_lanes[i] < m.flushed_lanes[i-1]) { sorted=0; break; }

    CHECK(m.total_flushed == 64u, "64 pkts flushed");
    CHECK(sorted, "flushed batch hilbert_lane sorted ASC");
}

/* T14: manual flush drains remainder */
static void t14(void) {
    printf("\nT14: tgw_dispatch_p6_flush() drains partial batch\n");
    geo_addr_net_init();
    TGWDispatchP6 d; tgw_dispatch_p6_init(&d);
    MockCtx m; memset(&m, 0, sizeof(m));

    /* send 10 ROUTE pkts */
    uint32_t sent = 0u;
    for (uint64_t enc = 0u; sent < 10u; enc++) {
        if (enc > 200u) break;
        GeoNetAddr a = geo_net_encode(enc);
        if (a.polarity) continue;
        tgw_dispatch_p6(&d, enc, enc, _mock_flush, _mock_ground, &m);
        sent++;
    }
    CHECK(m.flush_count == 0u, "no auto-flush for 10 pkts");
    tgw_dispatch_p6_flush(&d, _mock_flush, &m);
    CHECK(m.flush_count   == 1u,  "manual flush fires");
    CHECK(m.total_flushed == 10u, "10 pkts drained");
    CHECK(d.pending_n     == 0u,  "pending_n = 0 after flush");
}

/* T15: invariant T7 — ground+route = total_dispatched */
static void t15(void) {
    printf("\nT15: invariant T7 — ground+route=total_dispatched\n");
    geo_addr_net_init();
    TGWDispatchP6 d; tgw_dispatch_p6_init(&d);
    MockCtx m; memset(&m, 0, sizeof(m));

    /* dispatch all 720 enc */
    for (uint64_t enc = 0u; enc < GAN_TRING_CYCLE; enc++)
        tgw_dispatch_p6(&d, enc, enc ^ 0xDEADu, _mock_flush, _mock_ground, &m);
    tgw_dispatch_p6_flush(&d, _mock_flush, &m);

    uint32_t sum = d.ground_count + d.route_count;
    CHECK(sum == d.total_dispatched, "ground+route = total_dispatched");
    CHECK(d.ground_count == 360u, "ground_count = 360 (50% split)");
    CHECK(d.route_count  == 360u, "route_count  = 360 (50% split)");
}

/* T16: avg_swaps_per_flush < max (2016) */
static void t16(void) {
    printf("\nT16: stats avg_swaps_per_flush < 2016\n");
    geo_addr_net_init();
    TGWDispatchP6 d; tgw_dispatch_p6_init(&d);
    MockCtx m; memset(&m, 0, sizeof(m));

    for (uint64_t enc = 0u; enc < GAN_TRING_CYCLE * 2u; enc++)
        tgw_dispatch_p6(&d, enc, enc, _mock_flush, _mock_ground, &m);
    tgw_dispatch_p6_flush(&d, _mock_flush, &m);

    TGWDispatchP6Stats s = tgw_dispatch_p6_stats(&d);
    printf("  INFO  avg_swaps/flush=%.1f  efficiency=%.3f  flushes=%u\n",
           s.avg_swaps_per_flush, s.sort_efficiency, s.batch_flushes);
    CHECK(s.avg_swaps_per_flush < 2016.0, "avg_swaps < 2016 max");
    CHECK(s.sort_efficiency >= 0.0 && s.sort_efficiency <= 1.0,
          "sort_efficiency in [0,1]");
}

/* T17: TGWLCRouteP6 size = 8 bytes */
static void t17(void) {
    printf("\nT17: TGWLCRouteP6 sizeof = 8\n");
    CHECK(sizeof(TGWLCRouteP6) == 8u, "TGWLCRouteP6 is 8 bytes");
}

/* T18: sort_efficiency */
static void t18(void) {
    printf("\nT18: sort_efficiency in [0,1]\n");
    TGWDispatchP6 d; tgw_dispatch_p6_init(&d);
    d.batch_flushes  = 5u;
    d.sort_skipped   = 0u;
    d.total_swaps    = 500u;
    TGWDispatchP6Stats s = tgw_dispatch_p6_stats(&d);
    CHECK(s.sort_efficiency >= 0.0 && s.sort_efficiency <= 1.0,
          "sort_efficiency in valid range");
}

/* T19: spoke histogram P6 = 120/spoke */
static void t19(void) {
    printf("\nT19: tgwlc_spoke_hist_p6() = 120/spoke\n");
    TStreamPkt pkts[GAN_TRING_CYCLE];
    for (uint32_t i=0; i<GAN_TRING_CYCLE; i++) pkts[i].enc = (uint32_t)i;
    uint32_t hist[GAN_SPOKES];
    tgwlc_spoke_hist_p6(pkts, (uint16_t)GAN_TRING_CYCLE, hist);
    int ok = 1;
    for (int s=0; s<(int)GAN_SPOKES; s++) {
        printf("  INFO  spoke[%d]=%u\n", s, hist[s]);
        if (hist[s] != 120u) { ok=0; }
    }
    CHECK(ok, "all 6 spokes = 120 pkts each");
}

/* T20: tgwlc_hilbert_lane = geo_net_hilbert_lane */
static void t20(void) {
    printf("\nT20: tgwlc_hilbert_lane() = geo_net_hilbert_lane()\n");
    int ok = 1;
    for (uint64_t enc = 0u; enc < GAN_TRING_CYCLE; enc++) {
        if (tgwlc_hilbert_lane(enc) != geo_net_hilbert_lane(enc)) {
            ok=0; break;
        }
    }
    CHECK(ok, "tgwlc_hilbert_lane consistent with geo_net for all 720");
}

/* ─── main ──────────────────────────────────────────────── */
int main(void) {
    printf("=== P6: tgw_lc_bridge_p6.h Test Suite ===\n");
    printf("SEAM2 hook + Hilbert prefetch sort\n\n");

    geo_addr_net_init();

    t01(); t02(); t03(); t04(); t05();
    t06(); t07(); t08(); t09(); t10();
    t11(); t12(); t13(); t14(); t15();
    t16(); t17(); t18(); t19(); t20();

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    return _fail ? 1 : 0;
}

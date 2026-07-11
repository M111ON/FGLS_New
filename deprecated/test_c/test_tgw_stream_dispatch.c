/*
 * test_tgw_stream_dispatch.c — Stream Dispatch Integration Test
 * ==============================================================
 * Verifies tgw_stream_dispatch() correctness:
 *
 *   SD1: spoke coverage — 6 spokes get packets (uniform-ish)
 *   SD2: polarity split — ~50% GROUND (mirror), ~50% ROUTE (live)
 *   SD3: stats consistency — pkts_rx = ground + route + no_bp + gap
 *   SD4: GROUND read-back — pl_read finds mirror-side packets
 *   SD5: no GROUND from tgw_stream_file (old path) — confirms SEAM 1
 *        was real: old path never sets polarity → all bypass GROUND
 *
 * Compile:
 *   gcc -O2 -o test_tgw_stream_dispatch test_tgw_stream_dispatch.c \
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

#include "tgw_stream_dispatch.h"

/* ── test data ─────────────────────────────────────────────── */
static const GeoSeed TEST_SEED = {
    0xDEADBEEF12345678ULL,
    0xCAFEBABE87654321ULL
};

static const uint64_t BUNDLE[GEO_BUNDLE_WORDS] = {
    0x1111111111111111ULL, 0x2222222222222222ULL,
    0x3333333333333333ULL, 0x4444444444444444ULL,
    0x5555555555555555ULL, 0x6666666666666666ULL,
    0x7777777777777777ULL, 0x8888888888888888ULL,
};

/* generate deterministic test payload — 4 chunks worth */
static void make_payload(uint8_t *buf, uint32_t size) {
    for (uint32_t i = 0; i < size; i++)
        buf[i] = (uint8_t)((i * 0x9Eu) ^ (i >> 3));
}

/* ═══════════════════════════════════════════════════════════
 * SD1: spoke coverage — all 6 spokes receive packets
 * ═══════════════════════════════════════════════════════════ */
static int test_sd1_spoke_coverage(void) {
    printf("[SD1] spoke coverage — 6 spokes must all receive packets\n");

    /* build payload large enough for multiple chunks */
    uint32_t sz = TSTREAM_DATA_BYTES * 12u;   /* 12 chunks = 2 per spoke */
    uint8_t *buf = malloc(sz);
    make_payload(buf, sz);

    /* slice into pkts manually to inspect enc distribution */
    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    uint16_t n = tstream_slice_file(pkts, buf, sz);
    free(buf);

    uint32_t spoke_cnt[TRING_SPOKES] = {0};
    tgw_stream_spoke_coverage(pkts, n, spoke_cnt);

    printf("  pkts=%u  spoke distribution:\n", n);
    int all_hit = 1;
    for (int s = 0; s < (int)TRING_SPOKES; s++) {
        printf("    spoke[%d] = %u\n", s, spoke_cnt[s]);
        if (spoke_cnt[s] == 0) all_hit = 0;
    }

    if (all_hit)
        printf("  PASS: all 6 spokes covered ✓\n");
    else
        printf("  FAIL: some spokes empty — GEO_WALK distribution issue\n");
    return all_hit;
}

/* ═══════════════════════════════════════════════════════════
 * SD2: polarity split — live vs mirror ~50/50
 * ═══════════════════════════════════════════════════════════ */
static int test_sd2_polarity_split(void) {
    printf("[SD2] polarity split — live(ROUTE) vs mirror(GROUND)\n");

    uint32_t sz = TSTREAM_DATA_BYTES * 60u;   /* 60 chunks = 1 per pent pos */
    uint8_t *buf = malloc(sz);
    make_payload(buf, sz);

    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    uint16_t n = tstream_slice_file(pkts, buf, sz);
    free(buf);

    uint32_t live = 0, mirror = 0, unknown = 0;
    for (uint16_t i = 0; i < n; i++) {
        TRingRoute rt = tring_route_from_enc(pkts[i].enc);
        if (rt.pos == 0xFFFFu) { unknown++; continue; }
        if (rt.polarity == 1) mirror++;
        else                  live++;
    }

    printf("  pkts=%u  live=%u  mirror=%u  unknown=%u\n",
           n, live, mirror, unknown);

    /* expect roughly equal — allow 30% skew for small N */
    int total = (int)(live + mirror);
    int ok = total > 0
          && (int)live   >= total * 30 / 100
          && (int)mirror >= total * 30 / 100
          && unknown == 0;

    if (ok)
        printf("  PASS: polarity split balanced ✓\n");
    else
        printf("  FAIL: skewed split or unknown enc\n");
    return ok;
}

/* ═══════════════════════════════════════════════════════════
 * SD3: stats consistency
 * ═══════════════════════════════════════════════════════════ */
static int test_sd3_stats(void) {
    printf("[SD3] stats consistency — rx = ground+route+no_bp+gap\n");

    TGWCtx ctx;
    TGWDispatch d;
    TGWStreamDispatch sd;
    tgw_init(&ctx, TEST_SEED, BUNDLE);
    tgw_dispatch_init(&d, 0xC0FFEE0000000000ULL);
    tgw_stream_dispatch_init(&sd);

    uint32_t sz = TSTREAM_DATA_BYTES * 20u;
    uint8_t *buf = malloc(sz);
    make_payload(buf, sz);

    tgw_stream_dispatch(&ctx, &d, &sd, buf, sz);
    free(buf);

    TGWStreamStats ss = tgw_stream_dispatch_stats(&sd);
    uint32_t sum = ss.pkts_ground + ss.pkts_route + ss.pkts_no_bp + ss.pkts_gap;

    printf("  rx=%u  ground=%u  route=%u  no_bp=%u  gap=%u  sum=%u\n",
           ss.pkts_rx, ss.pkts_ground, ss.pkts_route,
           ss.pkts_no_bp, ss.pkts_gap, sum);

    int ok = (sum == ss.pkts_rx);
    if (ok)
        printf("  PASS: stats consistent ✓\n");
    else
        printf("  FAIL: leaked %d ops\n", (int)ss.pkts_rx - (int)sum);
    return ok;
}

/* ═══════════════════════════════════════════════════════════
 * SD4: GROUND read-back — mirror packets in pl_read
 * ═══════════════════════════════════════════════════════════ */
static int test_sd4_ground_readback(void) {
    printf("[SD4] GROUND read-back — mirror packets findable via pl_read\n");

    TGWCtx ctx;
    TGWDispatch d;
    TGWStreamDispatch sd;
    tgw_init(&ctx, TEST_SEED, BUNDLE);
    tgw_dispatch_init(&d, 0xBEEF000000000000ULL);
    tgw_stream_dispatch_init(&sd);

    uint32_t sz = TSTREAM_DATA_BYTES * 60u;
    uint8_t *buf = malloc(sz);
    make_payload(buf, sz);

    /* collect mirror enc before dispatch */
    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    uint16_t n = tstream_slice_file(pkts, buf, sz);

    /* find first mirror-side packet */
    int mirror_idx = -1;
    uint64_t mirror_addr = 0, mirror_value = 0;
    for (uint16_t i = 0; i < n; i++) {
        TRingRoute rt = tring_route_from_enc(pkts[i].enc);
        if (rt.polarity == 1) {
            mirror_idx  = i;
            mirror_addr  = (uint64_t)pkts[i].enc;
            mirror_value = (uint64_t)pkts[i].crc16
                         | ((uint64_t)pkts[i].size << 16);
            break;
        }
    }

    tgw_stream_dispatch(&ctx, &d, &sd, buf, sz);
    free(buf);

    if (mirror_idx < 0) {
        printf("  WARN: no mirror packet in payload (payload too small?)\n");
        return 1;
    }

    PayloadResult pr = pl_read(&d.ps, mirror_addr);
    printf("  mirror_idx=%d  found=%d  value_match=%d\n",
           mirror_idx, pr.found, pr.found && pr.value == mirror_value);

    int ok = pr.found && pr.value == mirror_value;
    if (ok)
        printf("  PASS: GROUND mirror packet read-back ✓\n");
    else
        printf("  FAIL: mirror packet not in PayloadStore\n");
    return ok;
}

/* ═══════════════════════════════════════════════════════════
 * SD5: SEAM 1 proof — old tgw_stream_file never hits GROUND
 *      new tgw_stream_dispatch does
 * ═══════════════════════════════════════════════════════════ */
static int test_sd5_seam1_proof(void) {
    printf("[SD5] SEAM 1 proof — old path=0 GROUND, new path>0 GROUND\n");

    uint32_t sz = TSTREAM_DATA_BYTES * 60u;
    uint8_t *buf = malloc(sz);
    make_payload(buf, sz);

    /* OLD path: tgw_stream_file → tgw_dispatch */
    TGWCtx ctx_old;
    TGWDispatch d_old;
    tgw_init(&ctx_old, TEST_SEED, BUNDLE);
    tgw_dispatch_init(&d_old, 0xDEAD000000000000ULL);

    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    uint16_t n = tstream_slice_file(pkts, buf, sz);
    for (uint16_t i = 0; i < n; i++) {
        int snap = tstream_recv_pkt(&ctx_old.tr, ctx_old.store, &pkts[i]);
        if (snap < 0) continue;
        uint64_t addr  = (uint64_t)pkts[i].enc;
        uint64_t value = (uint64_t)pkts[i].crc16
                       | ((uint64_t)pkts[i].size << 16);
        TGWResult r = tgw_write(&ctx_old, addr, value, 0);
        tgw_dispatch(&d_old, &r, addr, value, BUNDLE);
    }
    uint32_t old_ground = d_old.ground_count;

    /* NEW path: tgw_stream_dispatch */
    TGWCtx ctx_new;
    TGWDispatch d_new;
    TGWStreamDispatch sd_new;
    tgw_init(&ctx_new, TEST_SEED, BUNDLE);
    tgw_dispatch_init(&d_new, 0xDEAD000000000000ULL);
    tgw_stream_dispatch_init(&sd_new);
    tgw_stream_dispatch(&ctx_new, &d_new, &sd_new, buf, sz);
    uint32_t new_ground = d_new.ground_count;

    free(buf);

    printf("  old path ground=%u  (expect 0 — SEAM 1)\n", old_ground);
    printf("  new path ground=%u  (expect >0)\n", new_ground);

    int ok = (old_ground == 0) && (new_ground > 0);
    if (ok)
        printf("  PASS: SEAM 1 confirmed and fixed ✓\n");
    else if (old_ground > 0)
        printf("  FAIL: old path already had GROUND — SEAM 1 may not exist\n");
    else
        printf("  FAIL: new path still 0 GROUND — polarity derive broken\n");
    return ok;
}

/* ═══════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔═══════════════════════════════════════════════════╗\n");
    printf("║  TGW Stream Dispatch Test  SD1-SD5               ║\n");
    printf("║  tgw_stream_dispatch → ROUTE/GROUND via TRing    ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n\n");

    int r[5];
    r[0] = test_sd1_spoke_coverage();  printf("\n");
    r[1] = test_sd2_polarity_split();  printf("\n");
    r[2] = test_sd3_stats();           printf("\n");
    r[3] = test_sd4_ground_readback(); printf("\n");
    r[4] = test_sd5_seam1_proof();     printf("\n");

    int passed = r[0]+r[1]+r[2]+r[3]+r[4];
    printf("══════════════════════════════════════════════════════\n");
    printf("RESULT: %d/5 passed\n", passed);
    printf("STATUS: %s\n", passed == 5
        ? "✅ STREAM DISPATCH WIRED AND VERIFIED"
        : "⚠️  check failures above");

    printf("\n── Test Map ──────────────────────────────────────────\n");
    printf("spoke coverage (6 spokes hit)        %s\n", r[0]?"✅":"❌");
    printf("polarity split (~50/50 live/mirror)  %s\n", r[1]?"✅":"❌");
    printf("stats consistent (no leaked ops)     %s\n", r[2]?"✅":"❌");
    printf("GROUND read-back via pl_read         %s\n", r[3]?"✅":"❌");
    printf("SEAM 1 confirmed + fixed             %s\n", r[4]?"✅":"❌");
    printf("──────────────────────────────────────────────────────\n");

    printf("\n── Polarity Math (frozen) ────────────────────────────\n");
    printf("pos      = tring_pos(enc)      // O(1) LUT 0..719\n");
    printf("pentagon = pos / 60            // 0..11\n");
    printf("spoke    = pentagon %% 6        // 0..5\n");
    printf("polarity = (pos %% 60) >= 30   // 0=live  1=mirror=GROUND\n");
    printf("──────────────────────────────────────────────────────\n");

    return (passed == 5) ? 0 : 1;
}

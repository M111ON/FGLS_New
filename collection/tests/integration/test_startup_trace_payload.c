/*
 * test_startup_trace_payload.c
 * Startup trace + synthetic payload benchmark for dispatch pipeline.
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

/* timing */
static inline uint64_t ns_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

#ifndef TEST_STUB_MODE
#define TEST_STUB_MODE 0
#endif
#include "geo_addr_net.h"
#include "geo_tring_walk.h"
#include "lc_hdr.h"
#include "tgw_lc_bridge_p6.h"
#include "tgw_dispatch_v2.h"
#include "tgw_ground_lcgw.h"
#define GEOPIXEL_SESSION_FEED_IMPL
#include "geopixel_session_feed.h"

static uint32_t g_flushed = 0;
static uint32_t g_ground = 0;
static void trace_flush(const uint64_t *a, const uint64_t *v, uint32_t n, void *ctx)
{
    (void)a; (void)v; (void)ctx;
    g_flushed += n;
}

static void trace_ground(uint64_t addr, uint64_t val, void *ctx)
{
    (void)addr; (void)val; (void)ctx;
    g_ground++;
}

typedef struct {
    const char *name;
    const uint8_t *data;
    size_t len;
    uint32_t loops;
    int force_ground_val;
} SimPayload;

static void run_trace_payload(const SimPayload *sp)
{
    GeopixelSessionMetrics m;
    TGWDispatchV2 d;
    TGWResult r;
    uint64_t t0, dt;
    uint64_t total_pkts = 0;
    uint32_t i, rep;

    memset(&r, 0, sizeof(r));
    r.gpr.blueprint_ready = 0; /* startup path */

    if (geopixel_session_scan_buf(sp->data, sp->len, &m) != 0) {
        printf("  [ERR] scan failed: %s\n", sp->name);
        return;
    }

    tgw_dispatch_v2_init(&d, 0x1234ULL);
    g_flushed = 0;
    g_ground = 0;

    printf("\n=== TRACE: %s ===\n", sp->name);
    printf("startup: bytes=%" PRIu64 " chunks=%u passthru=%u tail=%u seed_xor=0x%016" PRIx64 "\n",
        m.file_bytes, m.total_chunks, m.passthru_chunks, m.tail_chunks, m.seed_xor);

    /* trace first 12 events */
    printf("first 12 events:\n");
    for (i = 0; i < 12u; i++) {
        uint64_t addr = tring_walk_enc(i);
        GeoNetAddr g = geo_net_encode(addr);
        uint64_t val = sp->force_ground_val
            ? UINT64_C(0xFF)
            : ((uint64_t)sp->data[i % sp->len] ^ ((uint64_t)i << 8));
        tgw_dispatch_v2(&d, NULL, &r, addr, val, NULL, trace_flush, trace_ground, NULL);
        printf("  i=%02u addr=%3" PRIu64 " spoke=%u pol=%u hilbert=%u val=0x%04" PRIx64 "\n",
            i, addr, g.spoke, g.polarity, g.hilbert_idx, (val & UINT64_C(0xFFFF)));
    }
    tgw_dispatch_v2_flush(&d, trace_flush, NULL);

    /* benchmark loop */
    t0 = ns_now();
    for (rep = 0; rep < sp->loops; rep++) {
        for (i = 0; i < m.total_chunks; i++) {
            uint64_t addr = tring_walk_enc(i);
            uint64_t val = sp->force_ground_val
                ? UINT64_C(0xFF)
                : ((uint64_t)sp->data[i % sp->len] ^ ((uint64_t)rep << 16));
            tgw_dispatch_v2(&d, NULL, &r, addr, val, NULL, trace_flush, trace_ground, NULL);
            total_pkts++;
        }
        tgw_dispatch_v2_flush(&d, trace_flush, NULL);
    }
    dt = ns_now() - t0;

    {
        TGWDispatchV2Stats s = tgw_dispatch_v2_stats(&d);
        double ns_per_pkt = total_pkts ? ((double)dt / (double)total_pkts) : 0.0;
        double mpps = total_pkts ? ((double)total_pkts / ((double)dt / 1e9) / 1e6) : 0.0;
        printf("bench: loops=%u pkts=%" PRIu64 " %.2f ns/pkt %.2f Mpkts/s\n",
            sp->loops, total_pkts, ns_per_pkt, mpps);
        printf("stats: route=%u ground=%u flushes=%u avg_swaps=%.1f sort_eff=%.3f\n",
            s.route_count, s.ground_count, s.batch_flushes, s.avg_swaps_per_flush, s.sort_efficiency);
        printf("sinks: flushed=%u ground_cb=%u\n", g_flushed, g_ground);
    }
}

static void run_strict_ground_trace(void)
{
    LCGWGroundStore gs;
    uint32_t i;
    uint32_t ground_hits = 0;
    uint64_t t0, dt;

    lcgw_reset();
    lcgw_ground_init(&gs);

    printf("\n=== TRACE: strict-ground (direct ground hook) ===\n");
    printf("first 12 events:\n");
    for (i = 0; i < 12u; i++) {
        uint64_t addr = tring_walk_enc(i);
        GeoNetAddr g = geo_net_encode(addr);
        uint64_t val = UINT64_C(0xAA00) | (uint64_t)i;
        if (g.polarity) {
            tgw_ground_fn(addr, val, &gs);
            ground_hits++;
        }
        printf("  i=%02u addr=%3" PRIu64 " spoke=%u pol=%u -> %s\n",
            i, addr, g.spoke, g.polarity, g.polarity ? "GROUND" : "ROUTE");
    }

    t0 = ns_now();
    for (i = 0; i < 720000u; i++) {
        uint64_t addr = tring_walk_enc(i % 720u);
        GeoNetAddr g = geo_net_encode(addr);
        if (g.polarity) {
            tgw_ground_fn(addr, (uint64_t)i ^ UINT64_C(0xDEADBEEF), &gs);
            ground_hits++;
        }
    }
    dt = ns_now() - t0;

    {
        LCGWGroundStats s = lcgw_ground_stats(&gs);
        double ns_per = (ground_hits > 0u) ? ((double)dt / (double)ground_hits) : 0.0;
        double mops = (ground_hits > 0u) ? ((double)ground_hits / ((double)dt / 1e9) / 1e6) : 0.0;
        printf("strict-ground bench: hits=%u total_writes=%u %.2f ns/hit %.2f Mop/s\n",
            ground_hits, s.total_writes, ns_per, mops);
        printf("spokes: [0]=%u [1]=%u [2]=%u [3]=%u [4]=%u [5]=%u\n",
            s.writes[0], s.writes[1], s.writes[2], s.writes[3], s.writes[4], s.writes[5]);
    }
}

int main(void)
{
    /* simulated payload classes */
    static const uint8_t payload_bin[] = {
        0x10,0x22,0x34,0x48,0x55,0x60,0x77,0x88,0x90,0xA5,0xBE,0xCF,0xDD,0xE1,0xF2,0x04
    };
    static const uint8_t payload_apng_like[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A, /* PNG magic */
        0x66,0x63,0x54,0x4C,0x66,0x64,0x41,0x54,0x49,0x44,0x41,0x54,0x49,0x45,0x4E,0x44
    };
    static const uint8_t payload_seq_like[] = {
        'F','0','0','0','R','G','B',0x00,
        'F','0','0','1','R','G','B',0x01,
        'F','0','0','2','R','G','B',0x02,
        'F','0','0','3','R','G','B',0x03
    };

    SimPayload sim[] = {
        { "raw-binary", payload_bin, sizeof(payload_bin), 20000u, 0 },
        { "apng-like", payload_apng_like, sizeof(payload_apng_like), 20000u, 0 },
        { "sequence-like", payload_seq_like, sizeof(payload_seq_like), 20000u, 0 },
        { "ground-stress", payload_bin, sizeof(payload_bin), 20000u, 1 }
    };
    uint32_t i;

    geo_addr_net_init();
    printf("startup: geo_addr_net_init done, begin trace+bench\n");

    for (i = 0; i < (uint32_t)(sizeof(sim)/sizeof(sim[0])); i++)
        run_trace_payload(&sim[i]);
    run_strict_ground_trace();

    printf("\nDONE: trace+bench completed\n");
    return 0;
}

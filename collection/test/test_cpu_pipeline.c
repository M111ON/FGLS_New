/*
 * test_cpu_pipeline.c — CPU Pipeline v1 Integration Test
 * ═══════════════════════════════════════════════════════
 * Wires: geopixel tile_enc (SEAM 3) → tgw_dispatch_v2 (P4+P6) → stats
 *
 * Tests:
 *   IT1: spoke distribution — all 6 spokes receive packets
 *   IT2: ground+route = total_dispatched (invariant)
 *   IT3: polarity split 50/50 over full 720 cycle
 *   IT4: tile walk enc feeds dispatch without drops
 *   IT5: batch auto-flush fires correctly
 *   IT6: Hilbert sort efficiency > 0.5
 *   IT7: pipeline stats consistent after mixed tile stream
 *
 * Compile:
 *   gcc -O2 -Wall -I. -o test_cpu_pipeline test_cpu_pipeline.c && ./test_cpu_pipeline
 *
 * Sacred constants: 720=TRING_CYCLE, 64=BATCH_MAX, 6=SPOKES
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* ── Stubs (same pattern as test_dispatch_v2.c) ───────────────── */
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
    { (void)b; (void)u; (void)s; return true; }
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

/* ── Real headers ─────────────────────────────────────────────── */
#include "geo_addr_net.h"
#include "geo_tring_walk.h"
#include "lc_hdr.h"
#include "tgw_lc_bridge_p6.h"
#include "tgw_dispatch_v2.h"

/* ── Harness ──────────────────────────────────────────────────── */
static int _pass=0, _fail=0;
#define CHECK(cond, msg) do { \
    if(cond){printf("  PASS  %s\n",msg);_pass++;} \
    else    {printf("  FAIL  %s\n",msg);_fail++;} \
} while(0)

/* ── Pipeline mock context ────────────────────────────────────── */
typedef struct {
    /* flush accumulator */
    uint32_t flush_count;
    uint32_t total_flushed;
    /* spoke histogram from flushed addrs */
    uint32_t spoke_hist[6];
    /* ground accumulator */
    uint32_t ground_count;
    uint32_t ground_spokes[6];
    /* sorted check */
    bool     last_flush_sorted;
} PipelineMock;

static void _pipe_flush(const uint64_t *addrs, const uint64_t *vals,
                         uint32_t n, void *ctx)
{
    PipelineMock *m = (PipelineMock*)ctx;
    (void)vals;
    uint16_t prev_lane = 0;
    bool sorted = true;
    for (uint32_t i=0; i<n; i++) {
        uint16_t lane = tgwlc_hilbert_lane(addrs[i]);
        uint8_t  spk  = tring_walk_spoke((uint32_t)(addrs[i] % GAN_TRING_CYCLE));
        m->spoke_hist[spk]++;
        if (i > 0 && lane < prev_lane) sorted = false;
        prev_lane = lane;
    }
    m->total_flushed += n;
    m->flush_count++;
    m->last_flush_sorted = sorted;
}

static void _pipe_ground(uint64_t addr, uint64_t val, void *ctx)
{
    PipelineMock *m = (PipelineMock*)ctx;
    (void)val;
    uint8_t spk = tring_walk_spoke((uint32_t)(addr % GAN_TRING_CYCLE));
    m->ground_spokes[spk]++;
    m->ground_count++;
}

/* ── Simulate tile stream (SEAM 3 → dispatch) ─────────────────── */
static void run_tile_stream(TGWDispatchV2 *d, PipelineMock *m,
                             uint32_t NT, int bp_ready)
{
    TGWResult r; memset(&r, 0, sizeof(r));
    r.gpr.blueprint_ready = bp_ready;
    if (bp_ready) {
        r.gpr.bp.stamp_hash  = 0xABCDu;
        r.gpr.bp.spatial_xor = 0x1234u;
    }

    for (uint32_t i = 0; i < NT; i++) {
        /* SEAM 3: tile i → TRing enc via walk */
        uint16_t enc = tring_walk_enc(i);
        uint64_t addr = (uint64_t)enc;
        uint64_t val  = (uint64_t)i ^ 0xDEADu;
        tgw_dispatch_v2(d, NULL, &r, addr, val, NULL,
                        _pipe_flush, _pipe_ground, m);
    }
    tgw_dispatch_v2_flush(d, _pipe_flush, m);
}

/* ════════════════════════════════════════════════════════════════
   TESTS
   ════════════════════════════════════════════════════════════════ */

static void it1(void) {
    printf("\nIT1: all 6 spokes receive packets (NT=720)\n");
    geo_addr_net_init();
    TGWDispatchV2 d; tgw_dispatch_v2_init(&d, 0x1111);
    PipelineMock  m; memset(&m, 0, sizeof(m));

    run_tile_stream(&d, &m, 720u, 0);

    int all_hit = 1;
    for (int s=0; s<6; s++) {
        uint32_t total = m.spoke_hist[s] + m.ground_spokes[s];
        printf("  INFO  spoke[%d] route=%u ground=%u total=%u\n",
               s, m.spoke_hist[s], m.ground_spokes[s], total);
        if (total == 0) all_hit = 0;
    }
    CHECK(all_hit, "all 6 spokes hit");
}

static void it2(void) {
    printf("\nIT2: ground+route = total_dispatched (invariant)\n");
    geo_addr_net_init();
    TGWDispatchV2 d; tgw_dispatch_v2_init(&d, 0x2222);
    PipelineMock  m; memset(&m, 0, sizeof(m));

    run_tile_stream(&d, &m, 720u, 0);

    uint32_t sum = d.ground_count + d.route_count;
    CHECK(sum == d.total_dispatched,        "ground+route=total");
    CHECK(d.total_dispatched == 720u,       "total=720");
}

static void it3(void) {
    printf("\nIT3: polarity split ~50/50 over 720 tiles\n");
    geo_addr_net_init();
    TGWDispatchV2 d; tgw_dispatch_v2_init(&d, 0x3333);
    PipelineMock  m; memset(&m, 0, sizeof(m));

    run_tile_stream(&d, &m, 720u, 0);

    printf("  INFO  route=%u ground=%u total=%u\n",
           d.route_count, d.ground_count, d.total_dispatched);
    CHECK(d.route_count  == 360u, "route=360 (50%)");
    CHECK(d.ground_count == 360u, "ground=360 (50%)");
}

static void it4(void) {
    printf("\nIT4: tile walk enc feeds dispatch without drops\n");
    geo_addr_net_init();
    TGWDispatchV2 d; tgw_dispatch_v2_init(&d, 0x4444);
    PipelineMock  m; memset(&m, 0, sizeof(m));

    /* NT=256: common image 512×512 at TILE=32 */
    run_tile_stream(&d, &m, 256u, 0);

    uint32_t delivered = m.total_flushed + m.ground_count;
    printf("  INFO  NT=256 flushed=%u ground=%u delivered=%u\n",
           m.total_flushed, m.ground_count, delivered);
    CHECK(delivered == 256u, "all 256 tiles delivered");
    CHECK(d.total_dispatched == 256u, "total_dispatched=256");
}

static void it5(void) {
    printf("\nIT5: auto-flush fires at 64 accumulation\n");
    geo_addr_net_init();
    TGWDispatchV2 d; tgw_dispatch_v2_init(&d, 0x5555);
    PipelineMock  m; memset(&m, 0, sizeof(m));

    /* Send exactly 64 ROUTE pkts — need enc with polarity=0 */
    TGWResult r; memset(&r, 0, sizeof(r));
    uint32_t sent = 0;
    for (uint64_t enc = 0; sent < 64u; enc++) {
        GeoNetAddr a = geo_net_encode(enc);
        if (a.polarity) continue;
        tgw_dispatch_v2(&d, NULL, &r, enc, enc^0xFFu, NULL,
                        _pipe_flush, _pipe_ground, &m);
        sent++;
    }
    CHECK(m.flush_count   == 1u,  "1 auto-flush at 64");
    CHECK(m.total_flushed == 64u, "64 pkts flushed");
    CHECK(d.pending_n     == 0u,  "pending=0 after auto-flush");
}

static void it6(void) {
    printf("\nIT6: Hilbert sort efficiency > 0.5 (walk order helps)\n");
    geo_addr_net_init();
    TGWDispatchV2 d; tgw_dispatch_v2_init(&d, 0x6666);
    PipelineMock  m; memset(&m, 0, sizeof(m));

    /* Large stream to get meaningful sort stats */
    run_tile_stream(&d, &m, 720u * 3u, 0);

    TGWDispatchV2Stats s = tgw_dispatch_v2_stats(&d);
    printf("  INFO  sort_efficiency=%.3f avg_swaps=%.1f flushes=%u\n",
           s.sort_efficiency, s.avg_swaps_per_flush, s.batch_flushes);
    CHECK(s.sort_efficiency >= 0.5, "sort_efficiency ≥ 0.5");
    CHECK(s.batch_flushes   >  0u,  "at least one flush");
}

static void it7(void) {
    printf("\nIT7: stats consistent after mixed tile stream\n");
    geo_addr_net_init();
    TGWDispatchV2 d; tgw_dispatch_v2_init(&d, 0x7777);
    PipelineMock  m; memset(&m, 0, sizeof(m));

    /* Mix: 720 tiles no-bp + 720 tiles with bp (blueprint ready) */
    run_tile_stream(&d, &m, 720u, 0);   /* no blueprint */
    run_tile_stream(&d, &m, 720u, 1);   /* blueprint ready → geomatrix */

    TGWDispatchV2Stats s = tgw_dispatch_v2_stats(&d);
    printf("  INFO  total=%u route=%u ground=%u no_bp=%u route_cnt=%u\n",
           s.total_dispatched, s.route_count, s.ground_count,
           s.no_blueprint, s.route_count);
    CHECK(s.ground_count + s.route_count == s.total_dispatched,
          "invariant: ground+route=total");
    CHECK(s.total_dispatched == 1440u, "total=1440 (720×2)");
    CHECK(s.sort_efficiency >= 0.0 && s.sort_efficiency <= 1.0,
          "sort_efficiency in [0,1]");
}

/* ════════════════════════════════════════════════════════════════
   MAIN
   ════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("=== CPU Pipeline v1 Integration Test ===\n");
    printf("SEAM3(geo_tring_walk) → TGWDispatchV2(P4+P6) → stats\n");

    geo_addr_net_init();
    it1(); it2(); it3(); it4(); it5(); it6(); it7();

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);

    if (_fail == 0)
        printf("\n✓ CPU Pipeline v1 LOCKED — all layers verified\n");

    return _fail ? 1 : 0;
}

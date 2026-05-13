/*
 * bench_ground.c — GROUND path throughput vs ROUTE baseline
 * ═══════════════════════════════════════════════════════════════
 * Benchmarks:
 *   BG1: geo_net_encode polarity split — LUT cost (same for both)
 *   BG2: ROUTE-only dispatch throughput (null flush, skip ground)
 *   BG3: GROUND-only write throughput  (lcgw_ground_write direct)
 *   BG4: mixed 50/50 dispatch throughput (realistic: both paths live)
 *   BG5: ghost delete overhead — active vs ghosted spoke
 *   BG6: per-spoke write distribution — verify 60 pkts/spoke/cycle
 *
 * Expected ratio target: GROUND ≤ 2× slower than ROUTE
 * (GROUND does seed mixing + chunk rebuild; ROUTE is null-flushed here)
 *
 * Compile:
 *   gcc -O2 -Wall -I. -o bench_ground bench_ground.c && ./bench_ground
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

/* ── timing ──────────────────────────────────────────────────── */
static inline uint64_t ns_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ── stubs (identical to bench_pipeline_v2.c) ────────────────── */
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

#include "geo_addr_net.h"
#include "geo_tring_walk.h"
#include "lc_hdr.h"
#include "tgw_lc_bridge_p6.h"
#include "tgw_dispatch_v2.h"
#include "tgw_ground_lcgw.h"

/* ── bench sinks ─────────────────────────────────────────────── */
static volatile uint32_t _sink_route = 0;
static volatile uint32_t _sink_ground_null = 0;

static void _route_flush(const uint64_t *a, const uint64_t *v,
                          uint32_t n, void *ctx)
{ (void)a;(void)v;(void)ctx; _sink_route += n; }

static void _ground_null(uint64_t addr, uint64_t val, void *ctx)
{ (void)addr;(void)val;(void)ctx; _sink_ground_null++; }

/* ── bench constants ─────────────────────────────────────────── */
#define BENCH_CYCLE    720u        /* one TRing cycle */
#define BENCH_REPS     2000u       /* repetitions */
#define BENCH_TOTAL    (BENCH_CYCLE * BENCH_REPS)   /* 1.44M pkts */

/* ══════════════════════════════════════════════════════════════ */

int main(void) {
    geo_addr_net_init();

    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║  bench_ground.c — GROUND vs ROUTE Throughput          ║\n");
    printf("║  TRing 720 cycle × %4u reps = %7u total pkts       ║\n",
           BENCH_REPS, BENCH_TOTAL);
    printf("╚═══════════════════════════════════════════════════════╝\n\n");

    /* ── BG1: LUT encode cost (shared by both paths) ─────────── */
    printf("▶ BG1: geo_net_encode LUT — polarity+spoke (2M calls)\n");
    {
        volatile uint32_t sink = 0;
        uint64_t t0 = ns_now();
        for (uint32_t i = 0; i < 2000000u; i++) {
            GeoNetAddr a = geo_net_encode(i);
            sink += a.polarity + a.spoke;
        }
        uint64_t dt = ns_now() - t0;
        printf("  %.2f ns/call  %.0f Mop/s  (sink=%u)\n\n",
               (double)dt / 2e6, 2e9 / (double)dt, (unsigned)sink);
    }

    /* ── BG2: ROUTE-only throughput (GROUND → null callback) ─── */
    printf("▶ BG2: ROUTE-only dispatch — null ground callback (%u pkts)\n", BENCH_TOTAL);
    uint64_t route_ns = 0;
    double   route_mops = 0;
    {
        TGWDispatchV2 d; tgw_dispatch_v2_init(&d, 0xBEEF);
        TGWResult r; memset(&r, 0, sizeof(r));
        _sink_route = _sink_ground_null = 0;

        /* warmup 1 cycle */
        for (uint32_t i = 0; i < BENCH_CYCLE; i++)
            tgw_dispatch_v2(&d, NULL, &r, tring_walk_enc(i), i,
                            NULL, _route_flush, _ground_null, NULL);
        tgw_dispatch_v2_flush(&d, _route_flush, NULL);
        tgw_dispatch_v2_init(&d, 0xBEEF);
        _sink_route = _sink_ground_null = 0;

        uint64_t t0 = ns_now();
        for (uint32_t rep = 0; rep < BENCH_REPS; rep++) {
            for (uint32_t i = 0; i < BENCH_CYCLE; i++)
                tgw_dispatch_v2(&d, NULL, &r, tring_walk_enc(i),
                                (uint64_t)i ^ rep,
                                NULL, _route_flush, _ground_null, NULL);
            tgw_dispatch_v2_flush(&d, _route_flush, NULL);
        }
        route_ns = ns_now() - t0;
        route_mops = (double)BENCH_TOTAL / ((double)route_ns / 1e9) / 1e6;

        TGWDispatchV2Stats s = tgw_dispatch_v2_stats(&d);
        printf("  %.2f ns/pkt  %.1f Mop/s\n", (double)route_ns / BENCH_TOTAL, route_mops);
        printf("  route_flushed=%u  ground_null=%u\n",
               _sink_route, _sink_ground_null);
        printf("  avg_swaps=%.1f  sort_eff=%.3f\n\n",
               s.avg_swaps_per_flush, s.sort_efficiency);
    }

    /* ── BG3: GROUND-only write (lcgw_ground_write direct) ───── */
    printf("▶ BG3: GROUND-only lcgw_ground_write direct (%u calls)\n", BENCH_TOTAL);
    uint64_t ground_ns = 0;
    double   ground_mops = 0;
    {
        LCGWGroundStore gs; lcgw_ground_init(&gs);

        /* warmup */
        for (uint32_t i = 0; i < BENCH_CYCLE; i++)
            lcgw_ground_write(&gs, (uint64_t)i, (uint64_t)i ^ 0xDEAD);

        lcgw_reset();            /* close all file slots before reinit */
        lcgw_ground_init(&gs);   /* reset */

        uint64_t t0 = ns_now();
        for (uint32_t rep = 0; rep < BENCH_REPS; rep++) {
            for (uint32_t i = 0; i < BENCH_CYCLE; i++) {
                /* all 720 addrs: lcgw_ground_write handles polarity internally */
                uint64_t addr = (uint64_t)(i % GAN_TRING_CYCLE);
                lcgw_ground_write(&gs, addr, addr ^ (uint64_t)rep);
            }
        }
        ground_ns = ns_now() - t0;

        LCGWGroundStats s = lcgw_ground_stats(&gs);
        ground_mops = (double)(BENCH_REPS * BENCH_CYCLE) / ((double)ground_ns / 1e9) / 1e6;

        printf("  %.2f ns/write  %.1f Mop/s  (all 720 addrs, 50%% hit GROUND)\n",
               (double)ground_ns / (double)(s.total_writes ? s.total_writes : 1),
               ground_mops);
        printf("  total_writes=%u  active_spokes=%u\n",
               s.total_writes, lcgw_ground_active_spokes(&gs));

        /* per-spoke breakdown */
        printf("  spoke writes: ");
        for (uint32_t sp = 0; sp < 6; sp++)
            printf("[%u]=%u ", sp, s.writes[sp]);
        printf("\n\n");
    }

    /* ── BG4: mixed 50/50 dispatch + real GROUND lcgw ────────── */
    printf("▶ BG4: mixed 50/50 dispatch — dispatch + lcgw_ground (%u pkts)\n", BENCH_TOTAL);
    uint64_t mixed_ns = 0;
    double   mixed_mops = 0;
    {
        TGWDispatchV2  d;   tgw_dispatch_v2_init(&d, 0xCAFE);
        LCGWGroundStore gs; lcgw_reset(); lcgw_ground_init(&gs);
        TGWResult r; memset(&r, 0, sizeof(r));
        _sink_route = 0;

        /* warmup */
        for (uint32_t i = 0; i < BENCH_CYCLE; i++)
            tgw_dispatch_v2(&d, NULL, &r, tring_walk_enc(i), i,
                            NULL, _route_flush, tgw_ground_fn, &gs);
        tgw_dispatch_v2_flush(&d, _route_flush, NULL);
        tgw_dispatch_v2_init(&d, 0xCAFE);
        lcgw_reset();
        lcgw_ground_init(&gs);
        _sink_route = 0;

        uint64_t t0 = ns_now();
        for (uint32_t rep = 0; rep < BENCH_REPS; rep++) {
            for (uint32_t i = 0; i < BENCH_CYCLE; i++)
                tgw_dispatch_v2(&d, NULL, &r, tring_walk_enc(i),
                                (uint64_t)i ^ rep,
                                NULL, _route_flush, tgw_ground_fn, &gs);
            tgw_dispatch_v2_flush(&d, _route_flush, NULL);
        }
        mixed_ns = ns_now() - t0;
        mixed_mops = (double)BENCH_TOTAL / ((double)mixed_ns / 1e9) / 1e6;

        LCGWGroundStats s = lcgw_ground_stats(&gs);
        TGWDispatchV2Stats ds = tgw_dispatch_v2_stats(&d);

        printf("  %.2f ns/pkt  %.1f Mop/s\n",
               (double)mixed_ns / BENCH_TOTAL, mixed_mops);
        printf("  route_flushed=%u  ground_writes=%u\n",
               _sink_route, s.total_writes);
        printf("  avg_swaps=%.1f  sort_eff=%.3f\n\n",
               ds.avg_swaps_per_flush, ds.sort_efficiency);
    }

    /* ── BG5: ghost delete overhead ──────────────────────────── */
    printf("▶ BG5: ghost delete overhead — active vs ghosted spoke\n");
    {
        #define GHOST_WRITES 50000u
        LCGWGroundStore gs; lcgw_reset(); lcgw_ground_init(&gs);

        /* pre-populate all spokes */
        for (uint32_t i = 0; i < BENCH_CYCLE; i++) {
            GeoNetAddr a = geo_net_encode(i);
            if (a.polarity) lcgw_ground_write(&gs, i, i ^ 0xAB);
        }

        /* measure active spoke (spoke 1) writes */
        uint64_t t0 = ns_now();
        for (uint32_t i = 0; i < GHOST_WRITES; i++) {
            /* addr 60 → spoke 0, polarity=1 → always hits spoke 0 */
            lcgw_ground_write(&gs, 60u + (i % 60u), (uint64_t)i);
        }
        uint64_t dt_active = ns_now() - t0;

        /* ghost spoke 0 */
        lcgw_ground_delete(&gs, 0u);

        /* measure ghosted spoke writes (should be ~free: early return) */
        t0 = ns_now();
        for (uint32_t i = 0; i < GHOST_WRITES; i++) {
            lcgw_ground_write(&gs, 60u + (i % 60u), (uint64_t)i);
        }
        uint64_t dt_ghosted = ns_now() - t0;

        printf("  active  spoke: %.2f ns/write  (%u writes)\n",
               (double)dt_active / GHOST_WRITES, GHOST_WRITES);
        printf("  ghosted spoke: %.2f ns/write  (%u writes, early-exit)\n",
               (double)dt_ghosted / GHOST_WRITES, GHOST_WRITES);
        double speedup = (dt_active > 0) ? (double)dt_active / (double)dt_ghosted : 1.0;
        printf("  ghost early-exit speedup: %.1f×\n\n", speedup);
    }

    /* ── BG6: per-spoke distribution over full 720 cycle ─────── */
    printf("▶ BG6: per-spoke GROUND distribution — 720-cycle stream\n");
    {
        LCGWGroundStore gs; lcgw_reset(); lcgw_ground_init(&gs);
        uint32_t spoke_count[6] = {0};

        for (uint32_t pos = 0; pos < GAN_TRING_CYCLE; pos++) {
            GeoNetAddr a = geo_net_encode(pos);
            if (a.polarity) {
                lcgw_ground_write(&gs, pos, pos);
                spoke_count[a.spoke]++;
            }
        }

        LCGWGroundStats s = lcgw_ground_stats(&gs);
        printf("  Total GROUND pkts in 720 cycle: %u  (expect 360 = 50%%)\n",
               s.total_writes);
        printf("  Per-spoke (expect 60 each):\n");

        int all_balanced = 1;
        for (uint32_t sp = 0; sp < 6; sp++) {
            int ok = (s.writes[sp] == 60u);
            if (!ok) all_balanced = 0;
            printf("    spoke[%u]: %u %s\n", sp, s.writes[sp],
                   ok ? "✓" : "✗ UNBALANCED");
        }
        printf("  Balance check: %s\n\n", all_balanced ? "PASS ✅" : "FAIL ❌");
    }

    /* ── summary ──────────────────────────────────────────────── */
    printf("══════════════════════════════════════════════════════════\n");
    printf("Summary\n");
    printf("══════════════════════════════════════════════════════════\n");
    printf("  BG2  ROUTE (null-ground):     %.1f Mop/s\n", route_mops);
    printf("  BG3  GROUND (direct write):   %.1f Mop/s\n", ground_mops);
    printf("  BG4  Mixed 50/50 (full path): %.1f Mop/s\n", mixed_mops);

    double overhead = (ground_mops > 0) ? route_mops / ground_mops : 0;
    printf("\n  GROUND overhead vs ROUTE: %.2f× slower\n", overhead);
    if (overhead <= 2.0)
        printf("  Target (≤2×): PASS ✅\n");
    else
        printf("  Target (≤2×): FAIL ❌  → consider seed-mix optimization\n");

    printf("══════════════════════════════════════════════════════════\n");
    return 0;
}

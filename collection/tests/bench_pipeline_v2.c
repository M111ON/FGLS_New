/*
 * bench_pipeline_v2.c — CPU Pipeline v1 Benchmark
 * ═══════════════════════════════════════════════════════════════
 * Measures hot-path costs for each layer:
 *   B1: geo_net_encode LUT         — SEAM 2 (polarity+hilbert)
 *   B2: tring_walk_enc             — SEAM 3 (tile dispatch enc)
 *   B3: tgw_dispatch_v2 core       — P4+P6 dispatch loop
 *   B4: tile stream throughput     — NT tiles end-to-end
 *   B5: Hilbert sort vs insertion  — batch sort cost
 *
 * P3 baseline (from docs): 62 MB/s full pipeline
 * P4 target estimate:      120-150 MB/s
 *
 * Compile:
 *   gcc -O2 -Wall -I. -o bench_pipeline_v2 bench_pipeline_v2.c && ./bench_pipeline_v2
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

/* ── stubs (same as test_cpu_pipeline.c) ─────────────────────── */
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

#include "geo_addr_net.h"
#include "geo_tring_walk.h"
#include "lc_hdr.h"
#include "tgw_lc_bridge_p6.h"
#include "tgw_dispatch_v2.h"

/* ── null callbacks (zero overhead baseline) ─────────────────── */
static uint32_t _sink_flush = 0;
static uint32_t _sink_ground = 0;

static void _null_flush(const uint64_t *a, const uint64_t *v,
                         uint32_t n, void *ctx)
{ (void)a;(void)v;(void)ctx; _sink_flush += n; }

static void _null_ground(uint64_t addr, uint64_t val, void *ctx)
{ (void)addr;(void)val;(void)ctx; _sink_ground++; }

/* ══════════════════════════════════════════════════════════════ */

int main(void) {
    geo_addr_net_init();

    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║  CPU Pipeline v1 Benchmark (P4+P6+SEAM2+SEAM3)       ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");

    /* ── B1: geo_net_encode LUT (SEAM 2) ──────────────────────── */
    printf("▶ B1: geo_net_encode LUT — SEAM 2 (2M calls)\n");
    {
        volatile uint32_t sink = 0;
        uint64_t t0 = ns_now();
        for (uint32_t rep = 0; rep < 2000000u; rep++) {
            GeoNetAddr a = geo_net_encode(rep & 0x3FFu);
            sink += a.polarity + a.spoke;
        }
        uint64_t dt = ns_now() - t0;
        double ns_per = (double)dt / 2000000.0;
        double mops   = 2000.0 / ((double)dt / 1e9);
        printf("  2M calls: %.2f ns/call  %.0f Mop/s  (sink=%u)\n\n",
               ns_per, mops, (unsigned)sink);
    }

    /* ── B2: tring_walk_enc (SEAM 3) ─────────────────────────── */
    printf("▶ B2: tring_walk_enc — SEAM 3 (2M calls)\n");
    {
        volatile uint32_t sink = 0;
        uint64_t t0 = ns_now();
        for (uint32_t i = 0; i < 2000000u; i++) {
            sink += tring_walk_enc(i & 0x3FFu);
        }
        uint64_t dt = ns_now() - t0;
        double ns_per = (double)dt / 2000000.0;
        double mops   = 2000.0 / ((double)dt / 1e9);
        printf("  2M calls: %.2f ns/call  %.0f Mop/s  (sink=%u)\n\n",
               ns_per, mops, (unsigned)sink);
    }

    /* ── B3: tgw_dispatch_v2 core (full 720 cycle, 1K reps) ──── */
    printf("▶ B3: tgw_dispatch_v2 core — 720 pkts × 1000 reps\n");
    {
        TGWDispatchV2 d; tgw_dispatch_v2_init(&d, 0xBEEF);
        TGWResult r; memset(&r, 0, sizeof(r));
        _sink_flush = _sink_ground = 0;

        /* warmup */
        for (uint64_t enc = 0; enc < 720u; enc++)
            tgw_dispatch_v2(&d, NULL, &r, enc, enc^0xDEAD, NULL,
                            _null_flush, _null_ground, NULL);
        tgw_dispatch_v2_flush(&d, _null_flush, NULL);

        /* reset counters */
        tgw_dispatch_v2_init(&d, 0xBEEF);
        _sink_flush = _sink_ground = 0;

        uint64_t t0 = ns_now();
        for (uint32_t rep = 0; rep < 1000u; rep++) {
            /* use walk order: non-sequential hilbert → realistic sort cost */
            for (uint32_t ti = 0; ti < 720u; ti++) {
                uint64_t enc = tring_walk_enc(ti);
                tgw_dispatch_v2(&d, NULL, &r, enc, enc ^ (uint64_t)rep,
                                NULL, _null_flush, _null_ground, NULL);
            }
            tgw_dispatch_v2_flush(&d, _null_flush, NULL);
        }
        uint64_t dt = ns_now() - t0;

        uint64_t total_pkts = 720ull * 1000ull;
        double ns_per = (double)dt / (double)total_pkts;
        double mops   = (double)total_pkts / ((double)dt / 1e9) / 1e6;
        TGWDispatchV2Stats s = tgw_dispatch_v2_stats(&d);
        printf("  720K pkts: %.2f ns/pkt  %.1f Mop/s\n", ns_per, mops);
        printf("  flushes=%u  sort_eff=%.3f  avg_swaps=%.1f\n\n",
               s.batch_flushes, s.sort_efficiency, s.avg_swaps_per_flush);
    }

    /* ── B4: tile stream throughput (SEAM3→dispatch) ─────────── */
    printf("▶ B4: tile stream throughput — SEAM3 walk → dispatch\n");
    {
        #define BENCH_NT    720u
        #define BENCH_REPS  1000u

        TGWDispatchV2 d; tgw_dispatch_v2_init(&d, 0xCAFE);
        TGWResult r; memset(&r, 0, sizeof(r));
        _sink_flush = _sink_ground = 0;

        /* simulate: each tile → tring_walk_enc → dispatch */
        uint64_t t0 = ns_now();
        for (uint32_t rep = 0; rep < BENCH_REPS; rep++) {
            for (uint32_t i = 0; i < BENCH_NT; i++) {
                uint64_t enc = tring_walk_enc(i);
                uint64_t val = (uint64_t)i ^ (uint64_t)rep;
                tgw_dispatch_v2(&d, NULL, &r, enc, val, NULL,
                                _null_flush, _null_ground, NULL);
            }
            tgw_dispatch_v2_flush(&d, _null_flush, NULL);
        }
        uint64_t dt = ns_now() - t0;

        uint64_t total_tiles = (uint64_t)BENCH_NT * BENCH_REPS;
        /* assume 32×32 tile = 1024 bytes per tile */
        uint64_t total_bytes = total_tiles * 1024ull;
        double secs  = (double)dt / 1e9;
        double mb_s  = ((double)total_bytes / 1048576.0) / secs;
        double mt_s  = (double)total_tiles / 1e6 / secs;
        double ns_per = (double)dt / (double)total_tiles;

        printf("  %lluK tiles (32×32): %.2f ns/tile  %.0f Mtile/s\n",
               (unsigned long long)total_tiles / 1000, ns_per, mt_s);
        printf("  equiv throughput (1KB/tile): %.1f MB/s  [stub callbacks]\n", mb_s);
        printf("  flushed=%u  grounded=%u\n\n",
               _sink_flush, _sink_ground);
    }

    /* ── B5: sort cost — walk-ordered vs reverse-ordered ──────── */
    printf("▶ B5: Hilbert sort cost — walk vs reverse tile order\n");
    {
        TGWDispatchV2 d1, d2;
        TGWResult r; memset(&r, 0, sizeof(r));

        /* forward walk order (pre-sorted → few swaps) */
        tgw_dispatch_v2_init(&d1, 0x1111);
        _sink_flush = _sink_ground = 0;
        uint64_t t0 = ns_now();
        for (uint32_t rep = 0; rep < 500u; rep++) {
            for (uint32_t i = 0; i < 720u; i++) {
                GeoNetAddr a = geo_net_encode(tring_walk_enc(i));
                if (!a.polarity)
                    tgw_dispatch_v2(&d1, NULL, &r, tring_walk_enc(i),
                                    i, NULL, _null_flush, _null_ground, NULL);
            }
            tgw_dispatch_v2_flush(&d1, _null_flush, NULL);
        }
        uint64_t dt_walk = ns_now() - t0;
        TGWDispatchV2Stats s1 = tgw_dispatch_v2_stats(&d1);

        /* reverse order (worst case for sort) */
        tgw_dispatch_v2_init(&d2, 0x2222);
        _sink_flush = _sink_ground = 0;
        t0 = ns_now();
        for (uint32_t rep = 0; rep < 500u; rep++) {
            for (int32_t i = 719; i >= 0; i--) {
                GeoNetAddr a = geo_net_encode(tring_walk_enc((uint32_t)i));
                if (!a.polarity)
                    tgw_dispatch_v2(&d2, NULL, &r, tring_walk_enc((uint32_t)i),
                                    (uint64_t)i, NULL, _null_flush, _null_ground, NULL);
            }
            tgw_dispatch_v2_flush(&d2, _null_flush, NULL);
        }
        uint64_t dt_rev = ns_now() - t0;
        TGWDispatchV2Stats s2 = tgw_dispatch_v2_stats(&d2);

        printf("  walk order:    sort_eff=%.3f  avg_swaps=%6.0f  time=%llu ms\n",
               s1.sort_efficiency, s1.avg_swaps_per_flush,
               (unsigned long long)(dt_walk / 1000000u));
        printf("  reverse order: sort_eff=%.3f  avg_swaps=%6.0f  time=%llu ms\n",
               s2.sort_efficiency, s2.avg_swaps_per_flush,
               (unsigned long long)(dt_rev / 1000000u));
        double speedup = (dt_rev > 0) ? (double)dt_rev / (double)dt_walk : 1.0;
        printf("  walk speedup vs reverse: %.2f×\n\n", speedup);
    }

    /* ── summary ──────────────────────────────────────────────── */
    printf("══════════════════════════════════════════════════════\n");
    printf("P3 baseline (docs): 62 MB/s full pipeline\n");
    printf("P4+SEAM2+SEAM3 (B4 equiv throughput shown above)\n");
    printf("══════════════════════════════════════════════════════\n");

    return 0;
}

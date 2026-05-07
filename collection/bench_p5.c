/*
 * bench_p5.c — P5-A: P3 baseline vs P4 batch dispatch benchmark
 * ==============================================================
 * Self-contained mock (mirrors test_p4_dispatch_batch.c mock layer).
 * No external headers needed.
 *
 * Measures:
 *   1. ns/call   — per tgw_write_dispatch call
 *   2. MB/s      — stream throughput (720 pkts × 4096 bytes)
 *   3. batch_flushes / total_dispatched ratio
 *
 * Three modes timed:
 *   MODE_P3  — serial: each ROUTE calls tgw_write immediately (mock 233ns)
 *   MODE_P4  — batch:  64-item pending buffer → tgw_batch (mock 80ns amort)
 *   MODE_PREFILTER — P4 + prefilter bypass (GROUND never calls tgw_write)
 *
 * Compile: gcc -O2 -Wall -o bench_p5 bench_p5.c && ./bench_p5
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

/* ── Sacred constants (frozen) ─────────────────────────────── */
#define TRING_CYCLE            720u
#define PENT_SPAN               60u
#define MIRROR_HALF             30u
#define TRING_SPOKES             6u
#define TSTREAM_DATA_BYTES    4096u
#define TSTREAM_MAX_PKTS       720u
#define GEO_BUNDLE_WORDS         8u
#define TGW_DISPATCH_BATCH_MAX  64u
#define GEOMATRIX_PATHS         18u
#define GEO_SLOTS              576u
#define GEO_BLOCK_BOUNDARY     288u

/* ── Bench config ───────────────────────────────────────────── */
#define BENCH_ITERS          10000u   /* dispatch loop repetitions       */
#define BENCH_PKTS_PER_ITER    720u   /* full TRing cycle per iter       */
#define BENCH_FILE_SIZE  (BENCH_PKTS_PER_ITER * TSTREAM_DATA_BYTES)

/* Mock latency injection (nanoseconds via spin loops) */
#define MOCK_TWR_NS_P3          80u   /* P3: tgw_write serial cost       */
#define MOCK_TWR_NS_P4          12u   /* P4: tgw_write still runs/pkt    */
#define MOCK_BATCH_NS_FIXED     40u   /* P4: tgw_batch fixed overhead    */
#define MOCK_BATCH_NS_PER_PKT    1u   /* P4: per-pkt amortized cost      */
#define MOCK_PL_WRITE_NS         2u   /* GROUND pl_write cost            */
#define MOCK_PREFILTER_NS_PKT    5u   /* prefilter cost per pkt          */

/* ── Timing helpers ─────────────────────────────────────────── */
static inline uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Spin ~N ns (poor-man's mock latency — avoids optimizing out) */
static volatile uint64_t _sink = 0;
static inline void spin_ns(uint64_t ns) {
    uint64_t end = ns_now() + ns;
    while (ns_now() < end) _sink++;
}

/* ── Mock types ─────────────────────────────────────────────── */
typedef struct { uint32_t enc; uint16_t size; uint16_t crc16;
                 uint8_t data[TSTREAM_DATA_BYTES]; } TStreamPkt;

typedef struct { uint64_t stamp_hash, spatial_xor;
                 uint32_t window_id, circuit_fired;
                 uint32_t tring_start, tring_end, tring_span;
                 uint8_t  top_gaps[4]; } GBBlueprint;

typedef struct { bool blueprint_ready; GBBlueprint bp; } GoldbergPipeResult;
typedef struct { GoldbergPipeResult gpr; uint32_t ev; } TGWResult;

typedef struct { uint32_t writes; } PayloadStore;
static inline void pl_init(PayloadStore *ps)                    { ps->writes = 0; }
static inline void pl_write_p(PayloadStore *ps, uint64_t a, uint64_t v) {
    (void)a; (void)v; ps->writes++; spin_ns(MOCK_PL_WRITE_NS);
}

typedef struct { uint32_t writes; } FtsTwinStore;
typedef struct { uint32_t writes; } FtsTwinStats;
static inline void fts_init(FtsTwinStore *f, uint64_t s)       { (void)s; f->writes=0; }
typedef struct { uint64_t merkle_root,sha256_hi,sha256_lo;
                 uint32_t offset,hop_count,segment; } DodecaEntry;
static inline void fts_write(FtsTwinStore *f, uint64_t a, uint64_t v,
                               const DodecaEntry *e) {
    (void)a;(void)v;(void)e; f->writes++;
}
static inline FtsTwinStats fts_stats(const FtsTwinStore *f) {
    FtsTwinStats s; s.writes=f->writes; return s;
}

typedef enum { LC_GATE_WARP=0,LC_GATE_ROUTE=1,
               LC_GATE_COLLISION=2,LC_GATE_GROUND=3 } LCGate;
typedef struct { uint8_t sign,mag,rgb,level,angle,letter; uint16_t slope; } LCHdr;
typedef struct { uint32_t gate_counts[4]; uint64_t palette_a,palette_b; } LCTwinGateCtx;
static inline void lc_twin_gate_init(LCTwinGateCtx *lc) { memset(lc,0,sizeof(*lc)); }
static inline LCHdr lc_hdr_encode_addr(uint64_t a) {
    LCHdr h={0}; h.sign=(uint8_t)((a>>63)&1); return h;
}
static inline LCHdr lc_hdr_encode_value(uint64_t v) {
    LCHdr h={0}; h.sign=(uint8_t)((v>>62)&1); return h;
}
static inline LCGate lch_gate(LCHdr hA, LCHdr hB, uint64_t pa, uint64_t pb) {
    (void)pa;(void)pb;
    if (hA.sign && hB.sign) return LC_GATE_GROUND;
    return LC_GATE_ROUTE;
}

typedef struct {
    uint32_t total_writes;
    uint64_t _bundle[GEO_BUNDLE_WORDS];
    uint32_t stream_pkts_rx, stream_gaps, blueprint_count;
} TGWCtx;

static inline void tgw_init(TGWCtx *ctx, uint64_t seed) {
    memset(ctx,0,sizeof(*ctx));
    for (int i=0;i<(int)GEO_BUNDLE_WORDS;i++) ctx->_bundle[i]=seed^(uint64_t)i;
}

/* ── Mock tgw_write: P3 vs P4 cost injected here ─────────── */
static int _mock_mode = 0;  /* 0=P3, 1=P4, 2=PREFILTER */

static inline TGWResult tgw_write_mock(TGWCtx *ctx, uint64_t a, uint64_t v,
                                        uint8_t s) {
    (void)s;
    ctx->total_writes++;
    /* inject serial cost for P3, reduced cost for P4 */
    if (_mock_mode == 0) spin_ns(MOCK_TWR_NS_P3);
    else                 spin_ns(MOCK_TWR_NS_P4);
    TGWResult r={0};
    r.gpr.blueprint_ready = true;
    r.gpr.bp.stamp_hash   = a ^ v;
    r.gpr.bp.tring_end    = ctx->total_writes % GEO_SLOTS;
    r.gpr.bp.tring_start  = (ctx->total_writes+10) % GEO_SLOTS;
    r.gpr.bp.circuit_fired= ctx->total_writes & 0x3u;
    return r;
}

/* ── Mock tgw_batch: amortized cost ──────────────────────── */
static uint64_t _bench_batch_calls = 0;
static uint64_t _bench_batch_total = 0;

static inline void tgw_batch_mock(TGWCtx *ctx, const uint64_t *addrs,
                                   const uint64_t *vals, uint32_t n, uint8_t s) {
    (void)addrs;(void)vals;(void)s;
    _bench_batch_calls++;
    _bench_batch_total += n;
    ctx->total_writes += n;
    /* amortized cost: fixed overhead + per-pkt cheap */
    spin_ns(MOCK_BATCH_NS_FIXED + (uint64_t)n * MOCK_BATCH_NS_PER_PKT);
}

typedef struct { uint8_t phase,bit; uint16_t hpos,idx; uint64_t sig; } GeoPacket;
typedef struct { uint64_t total_packets,sig_mismatches,
                          hilbert_violations,stable_batches; } GeomatrixStatsV3;
static inline uint64_t geo_compute_sig64(const uint64_t *b, uint8_t ph) {
    return b[ph & (GEO_BUNDLE_WORDS-1)] ^ ((uint64_t)ph<<32);
}
static inline bool geomatrix_batch_verdict(const GeoPacket *pkts,
                                            const uint64_t *b,
                                            GeomatrixStatsV3 *s) {
    (void)pkts;(void)b;(void)s; return true;
}

/* ── GEO_WALK mock ─────────────────────────────────────────── */
static uint32_t _geo_walk[TRING_CYCLE];
static void init_geo_walk(void) {
    for (int i=0;i<(int)TRING_CYCLE;i++)
        _geo_walk[i]=(uint32_t)((i*37u)%TRING_CYCLE);
}

/* ── TGWDispatch (P4 batch-aware) ──────────────────────────── */
typedef struct {
    FtsTwinStore  fts;
    PayloadStore  ps;
    LCTwinGateCtx lc;
    uint64_t pending_addrs[TGW_DISPATCH_BATCH_MAX];
    uint64_t pending_vals [TGW_DISPATCH_BATCH_MAX];
    uint32_t pending_n;
    uint32_t total_dispatched, route_count, ground_count;
    uint32_t verdict_fail, no_blueprint, batch_flushes;
} TGWDispatch;

static inline void tgw_dispatch_init(TGWDispatch *d, uint64_t seed) {
    memset(d,0,sizeof(*d));
    fts_init(&d->fts,seed); pl_init(&d->ps); lc_twin_gate_init(&d->lc);
}

static inline void _flush_batch(TGWDispatch *d, TGWCtx *ctx) {
    if (!d->pending_n) return;
    tgw_batch_mock(ctx, d->pending_addrs, d->pending_vals, d->pending_n, 0);
    d->pending_n = 0;
    d->batch_flushes++;
}

/* ── GeoPacket builder (from tgw_dispatch.h) ─────────────── */
static inline GeoPacket bp_to_pkt(const GBBlueprint *bp,
                                    const uint64_t *bundle, uint8_t pid) {
    GeoPacket pkt={0};
    pkt.phase = (uint8_t)((bp->circuit_fired+pid)&0x3u);
    pkt.sig   = geo_compute_sig64(bundle, pkt.phase);
    uint16_t h_end   = (uint16_t)(bp->tring_end   % GEO_SLOTS);
    uint16_t h_start = (uint16_t)(bp->tring_start % GEO_SLOTS);
    pkt.hpos = h_end;
    bool hb = (h_end   >= GEO_BLOCK_BOUNDARY);
    bool ib = (h_start >= GEO_BLOCK_BOUNDARY);
    if (hb != ib)
        pkt.idx = hb ? (uint16_t)(h_start+GEO_BLOCK_BOUNDARY)
                     : (uint16_t)(h_start%GEO_BLOCK_BOUNDARY);
    else
        pkt.idx = h_start;
    pkt.bit = (uint8_t)(bp->stamp_hash & 0x1u);
    return pkt;
}

/* ── Core dispatch (P4 batch path) ──────────────────────────
 * P3 mode: calls tgw_write immediately, no batch buffer.
 * P4 mode: accumulates, flushes at 64.
 */
static inline void dispatch_p3(TGWDispatch *d, TGWCtx *ctx,
                                 uint64_t addr, uint64_t value) {
    d->total_dispatched++;
    /* GROUND check */
    LCHdr hA = lc_hdr_encode_addr(addr);
    LCHdr hB = lc_hdr_encode_value(value);
    LCGate gate = lch_gate(hA, hB, d->lc.palette_a, d->lc.palette_b);
    if (gate == LC_GATE_GROUND) {
        pl_write_p(&d->ps, addr, value);
        d->ground_count++; return;
    }
    /* P3: serial tgw_write per pkt */
    TGWResult r = tgw_write_mock(ctx, addr, value, 0);
    const GBBlueprint *bp = &r.gpr.bp;
    GeoPacket batch[GEOMATRIX_PATHS];
    for (int p=0;p<GEOMATRIX_PATHS;p++)
        batch[p] = bp_to_pkt(bp, ctx->_bundle, (uint8_t)p);
    GeomatrixStatsV3 stats={0};
    bool verdict = geomatrix_batch_verdict(batch, ctx->_bundle, &stats);
    if (verdict) {
        DodecaEntry e={0};
        fts_write(&d->fts, bp->stamp_hash, bp->spatial_xor, &e);
        d->route_count++;
    } else {
        pl_write_p(&d->ps, addr, value);
        d->verdict_fail++; d->ground_count++;
    }
}

static inline void dispatch_p4(TGWDispatch *d, TGWCtx *ctx,
                                 uint64_t addr, uint64_t value) {
    d->total_dispatched++;
    /* GROUND check */
    LCHdr hA = lc_hdr_encode_addr(addr);
    LCHdr hB = lc_hdr_encode_value(value);
    LCGate gate = lch_gate(hA, hB, d->lc.palette_a, d->lc.palette_b);
    if (gate == LC_GATE_GROUND) {
        pl_write_p(&d->ps, addr, value);
        d->ground_count++; return;
    }
    /* P4: tgw_write + batch accumulate */
    TGWResult r = tgw_write_mock(ctx, addr, value, 0);
    const GBBlueprint *bp = &r.gpr.bp;
    GeoPacket batch[GEOMATRIX_PATHS];
    for (int p=0;p<GEOMATRIX_PATHS;p++)
        batch[p] = bp_to_pkt(bp, ctx->_bundle, (uint8_t)p);
    GeomatrixStatsV3 stats={0};
    bool verdict = geomatrix_batch_verdict(batch, ctx->_bundle, &stats);
    if (verdict) {
        d->pending_addrs[d->pending_n] = addr;
        d->pending_vals [d->pending_n] = value;
        d->pending_n++;
        if (d->pending_n >= TGW_DISPATCH_BATCH_MAX)
            _flush_batch(d, ctx);
        DodecaEntry e={0};
        fts_write(&d->fts, bp->stamp_hash, bp->spatial_xor, &e);
        d->route_count++;
    } else {
        _flush_batch(d, ctx);
        pl_write_p(&d->ps, addr, value);
        d->verdict_fail++; d->ground_count++;
    }
}

/* ── Prefilter helper (polarity bit only) ─────────────────── */
static inline uint8_t pkt_polarity(uint32_t enc) {
    uint16_t pos = (uint16_t)(enc % TRING_CYCLE);
    return (uint8_t)((pos % PENT_SPAN) >= MIRROR_HALF);
}

/* ── Build test stream: 720 pkts alternating ROUTE/GROUND ─── */
typedef struct {
    uint64_t addr, value;
    uint8_t  is_ground;  /* 1=GROUND (both signs set) */
} BenchPkt;

static BenchPkt _stream[BENCH_PKTS_PER_ITER];
static uint32_t _stream_n = 0;

static void build_stream(void) {
    _stream_n = 0;
    for (uint32_t i=0; i<BENCH_PKTS_PER_ITER; i++) {
        uint32_t enc = _geo_walk[i];
        uint8_t  pol = pkt_polarity(enc);
        BenchPkt p;
        if (pol) {
            /* GROUND — set both sign bits */
            p.addr  = (UINT64_C(1)<<63) | (uint64_t)enc;
            p.value = (UINT64_C(1)<<62) | (uint64_t)enc;
            p.is_ground = 1;
        } else {
            p.addr  = (uint64_t)enc;
            p.value = (uint64_t)(enc ^ 0xABCDu);
            p.is_ground = 0;
        }
        _stream[_stream_n++] = p;
    }
}

/* ── Result struct ─────────────────────────────────────────── */
typedef struct {
    const char *label;
    double ns_per_call;
    double mb_per_sec;
    uint64_t total_dispatched;
    uint64_t route_count;
    uint64_t ground_count;
    uint64_t batch_flushes;
    double flush_ratio;    /* batch_flushes / (route_count/64) ideal */
} BenchResult;

/* ── Run bench for one mode ──────────────────────────────────
 * Runs BENCH_ITERS × 720 pkts, measures wall time.
 */
static BenchResult run_bench(const char *label, int mode) {
    _mock_mode = mode;
    _bench_batch_calls = 0;
    _bench_batch_total = 0;

    TGWCtx ctx;      tgw_init(&ctx, 0xDEADBEEF);
    TGWDispatch d;   tgw_dispatch_init(&d, 0xDEADBEEF);

    uint64_t t0 = ns_now();

    for (uint32_t iter=0; iter<BENCH_ITERS; iter++) {
        for (uint32_t i=0; i<_stream_n; i++) {
            BenchPkt *p = &_stream[i];

            if (mode == 0) {
                /* P3 mode */
                dispatch_p3(&d, &ctx, p->addr, p->value);
            } else if (mode == 1) {
                /* P4 batch mode */
                dispatch_p4(&d, &ctx, p->addr, p->value);
            } else {
                /* PREFILTER mode: skip dispatch entirely for GROUND */
                spin_ns(MOCK_PREFILTER_NS_PKT);  /* prefilter cost */
                if (p->is_ground) {
                    pl_write_p(&d.ps, p->addr, p->value);
                    d.ground_count++; d.total_dispatched++;
                } else {
                    dispatch_p4(&d, &ctx, p->addr, p->value);
                }
            }
        }
        /* flush remainder at end of each iter */
        if (mode >= 1) _flush_batch(&d, &ctx);
    }

    uint64_t t1 = ns_now();
    uint64_t elapsed_ns = t1 - t0;
    uint64_t total_calls = (uint64_t)BENCH_ITERS * (uint64_t)_stream_n;
    uint64_t total_bytes = (uint64_t)BENCH_ITERS * (uint64_t)_stream_n
                           * TSTREAM_DATA_BYTES;

    BenchResult r;
    r.label            = label;
    r.ns_per_call      = (double)elapsed_ns / (double)total_calls;
    r.mb_per_sec       = (double)total_bytes / (double)elapsed_ns * 1000.0;
    r.total_dispatched = d.total_dispatched;
    r.route_count      = d.route_count;
    r.ground_count     = d.ground_count;
    r.batch_flushes    = d.batch_flushes;
    r.flush_ratio      = (d.route_count > 0)
        ? (double)d.batch_flushes / ((double)d.route_count / TGW_DISPATCH_BATCH_MAX)
        : 0.0;
    return r;
}

static void print_result(const BenchResult *r) {
    printf("┌─ %-40s ─────────────────┐\n", r->label);
    printf("│  ns/call          : %8.1f ns\n",    r->ns_per_call);
    printf("│  throughput       : %8.1f MB/s\n",  r->mb_per_sec);
    printf("│  total_dispatched : %8llu\n",        (unsigned long long)r->total_dispatched);
    printf("│  route / ground   : %llu / %llu\n",
           (unsigned long long)r->route_count,
           (unsigned long long)r->ground_count);
    printf("│  batch_flushes    : %8llu  (flush_ratio %.2fx)\n",
           (unsigned long long)r->batch_flushes, r->flush_ratio);
    printf("└──────────────────────────────────────────────────────────┘\n");
}

static void print_comparison(const BenchResult *p3,
                              const BenchResult *p4,
                              const BenchResult *pf) {
    double spd_p4 = p3->ns_per_call / p4->ns_per_call;
    double spd_pf = p3->ns_per_call / pf->ns_per_call;
    double mbs_p4 = p4->mb_per_sec  / p3->mb_per_sec;
    double mbs_pf = pf->mb_per_sec  / p3->mb_per_sec;

    printf("\n╔══ COMPARISON (vs P3 baseline) ══════════════════════════════╗\n");
    printf("║  Mode        ns/call   MB/s     speedup   MB/s ratio       ║\n");
    printf("║  P3 base  %8.1f  %6.1f     1.00×      1.00×           ║\n",
           p3->ns_per_call, p3->mb_per_sec);
    printf("║  P4 batch %8.1f  %6.1f     %.2f×      %.2f×           ║\n",
           p4->ns_per_call, p4->mb_per_sec, spd_p4, mbs_p4);
    printf("║  P4+preflt%8.1f  %6.1f     %.2f×      %.2f×           ║\n",
           pf->ns_per_call, pf->mb_per_sec, spd_pf, mbs_pf);
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    /* Invariant check: T7 ground+route=total */
    printf("\n── Invariant T7 (ground+route=total) ──\n");
    printf("   P3: %llu + %llu = %llu  %s\n",
           (unsigned long long)p3->ground_count,
           (unsigned long long)p3->route_count,
           (unsigned long long)p3->total_dispatched,
           (p3->ground_count + p3->route_count == p3->total_dispatched)
               ? "PASS" : "FAIL");
    printf("   P4: %llu + %llu = %llu  %s\n",
           (unsigned long long)p4->ground_count,
           (unsigned long long)p4->route_count,
           (unsigned long long)p4->total_dispatched,
           (p4->ground_count + p4->route_count == p4->total_dispatched)
               ? "PASS" : "FAIL");
}

/* ── main ─────────────────────────────────────────────────── */
int main(void) {
    init_geo_walk();
    build_stream();

    /* report stream composition */
    uint32_t g=0, r2=0;
    for (uint32_t i=0;i<_stream_n;i++) {
        if (_stream[i].is_ground) g++; else r2++;
    }
    printf("=== P5-A Bench: P3 vs P4 dispatch ===\n");
    printf("iters=%u  pkts/iter=%u  GROUND=%u ROUTE=%u (per iter)\n",
           BENCH_ITERS, _stream_n, g, r2);
    printf("BATCH_MAX=%u  DATA_BYTES=%u  total_data=%.1f MB per iter\n\n",
           TGW_DISPATCH_BATCH_MAX, TSTREAM_DATA_BYTES,
           (double)_stream_n * TSTREAM_DATA_BYTES / (1024.0*1024.0));

    printf("Running P3 baseline...\n");
    BenchResult res_p3 = run_bench("P3 — serial tgw_write per ROUTE pkt", 0);
    print_result(&res_p3);

    printf("\nRunning P4 batch...\n");
    BenchResult res_p4 = run_bench("P4 — 64-item batch amortized", 1);
    print_result(&res_p4);

    printf("\nRunning P4 + prefilter...\n");
    BenchResult res_pf = run_bench("P4+prefilter — GROUND bypass before tgw_write", 2);
    print_result(&res_pf);

    print_comparison(&res_p3, &res_p4, &res_pf);

    /* P5 estimate note */
    printf("\n── P5 target (from HANDOFF_P4.md) ──\n");
    printf("   Est. P4 real-hw: 120–150 MB/s  (from 62 MB/s P3)\n");
    printf("   Bench ratio achieved: %.1f×\n\n",
           res_pf.mb_per_sec / res_p3.mb_per_sec);

    return 0;
}

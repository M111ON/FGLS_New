/*
 * bench_tensor_track.c — Performance Benchmark
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Measures:
 *   1. Throughput: chunks/sec, MB/sec
 *   2. Latency: ns per chunk (ingest + route + store)
 *   3. Memory: bytes per record, context size
 *   4. Entropy scoring speed
 *   5. Frame range computation speed
 *   6. Full pipeline speed (data → enc → route → store)
 *
 * No malloc in hot path. All O(1).
 * ═══════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <time.h>

#include "gls_enclosure.h"
#include "tensor_track.h"
#include "geo_frame_seek.h"

/* ── Clock ──────────────────────────────────────────────────────────── */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── Data generators ────────────────────────────────────────────────── */

static void gen_zeros(uint8_t *buf, uint32_t n) { memset(buf, 0, n); }

static void gen_random(uint8_t *buf, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) buf[i] = (uint8_t)(rand() & 0xFF);
}

static void gen_structured(uint8_t *buf, uint32_t n)
{
    /* repeating pattern — compressible */
    for (uint32_t i = 0; i < n; i++) buf[i] = (uint8_t)((i % 4) * 0x55);
}

/* ── Benchmark: ingest throughput ───────────────────────────────────── */

static void bench_ingest(uint32_t n_chunks, uint32_t chunk_sz)
{
    uint8_t data[64];
    gen_random(data, chunk_sz);

    TTContext ctx;
    tt_init(&ctx);

    TTChunkRecord rec;
    double t0 = now_sec();

    for (uint32_t i = 0; i < n_chunks; i++) {
        /* Vary data slightly each chunk */
        data[0] = (uint8_t)(i & 0xFF);
        tt_ingest(&ctx, data, chunk_sz, &rec);
    }

    double dt = now_sec() - t0;
    double total_bytes = (double)n_chunks * chunk_sz;
    double throughput_mbps = total_bytes / dt / 1e6;
    double latency_ns = dt / n_chunks * 1e9;
    double chunks_per_sec = n_chunks / dt;

    printf("  ingest:    %u chunks × %uB = %.1f KB\n",
           n_chunks, chunk_sz, total_bytes / 1024);
    printf("             %.0f chunks/sec, %.1f MB/s, %.0f ns/chunk\n",
           chunks_per_sec, throughput_mbps, latency_ns);
}

/* ── Benchmark: entropy scoring ─────────────────────────────────────── */

static void bench_entropy(uint32_t n_chunks, uint32_t chunk_sz)
{
    uint8_t data[64];
    gen_random(data, chunk_sz);

    double t0 = now_sec();
    volatile uint8_t sink = 0;

    for (uint32_t i = 0; i < n_chunks; i++) {
        data[0] = (uint8_t)(i & 0xFF);
        sink = tt_entropy_score(data, chunk_sz);
    }

    double dt = now_sec() - t0;
    double latency_ns = dt / n_chunks * 1e9;

    printf("  entropy:   %u × %uB → score in %.0f ns/chunk\n",
           n_chunks, chunk_sz, latency_ns);
}

/* ── Benchmark: frame_range ─────────────────────────────────────────── */

static void bench_frame_range(uint32_t n_iters)
{
    double t0 = now_sec();
    volatile uint8_t sink = 0;

    for (uint32_t i = 0; i < n_iters; i++) {
        FrameRange fr = frame_range((uint16_t)(i % 1440), (uint8_t)(i % 4));
        sink = fr.frame_lo + fr.frame_hi;
    }

    double dt = now_sec() - t0;
    double latency_ns = dt / n_iters * 1e9;

    printf("  frame_range: %u calls in %.0f ns/call\n", n_iters, latency_ns);
}

/* ── Benchmark: route ───────────────────────────────────────────────── */

static void bench_route(uint32_t n_iters)
{
    double t0 = now_sec();
    volatile uint8_t sink = 0;

    for (uint32_t i = 0; i < n_iters; i++) {
        TTRoute r = tt_route((uint16_t)(i % 1440), (uint8_t)(i % 4));
        sink = r.strategy;
    }

    double dt = now_sec() - t0;
    double latency_ns = dt / n_iters * 1e9;

    printf("  route:      %u calls in %.0f ns/call\n", n_iters, latency_ns);
}

/* ── Benchmark: full pipeline (ingest + route + store) ──────────────── */

static void bench_full_pipeline(uint32_t n_chunks, uint32_t chunk_sz)
{
    uint8_t data[64];
    gen_random(data, chunk_sz);

    /* Enclosure */
    EncCtx enc_ctx;
    enc_init(&enc_ctx, 1);
    uint8_t store_buf[20736];

    /* Tracker */
    TTContext ctx;
    tt_init(&ctx);
    TTChunkRecord rec;

    double t0 = now_sec();

    for (uint32_t i = 0; i < n_chunks; i++) {
        data[0] = (uint8_t)(i & 0xFF);
        tt_ingest(&ctx, data, chunk_sz, &rec);
        TTRoute route = tt_route_record(&rec);
        (void)route;
        TTStoreResult sr = tt_store(&enc_ctx, &rec, store_buf);
        (void)sr;
    }

    double dt = now_sec() - t0;
    double total_bytes = (double)n_chunks * chunk_sz;
    double throughput_mbps = total_bytes / dt / 1e6;
    double latency_ns = dt / n_chunks * 1e9;
    double chunks_per_sec = n_chunks / dt;

    printf("  full pipe:  %u chunks × %uB = %.1f KB\n",
           n_chunks, chunk_sz, total_bytes / 1024);
    printf("             %.0f chunks/sec, %.1f MB/s, %.0f ns/chunk\n",
           chunks_per_sec, throughput_mbps, latency_ns);
}

/* ── Benchmark: memory footprint ────────────────────────────────────── */

static void bench_memory(void)
{
    printf("  ── Memory per component ──\n");
    printf("  TTChunkRecord:   %zu bytes (enc + entropy + data + len)\n",
           sizeof(TTChunkRecord));
    printf("  TTRoute:         %zu bytes\n", sizeof(TTRoute));
    printf("  TTStoreResult:   %zu bytes\n", sizeof(TTStoreResult));
    printf("  FrameRange:      %zu bytes\n", sizeof(FrameRange));
    printf("  DualFrame:       %zu bytes\n", sizeof(DualFrame));
    printf("  TTContext:       %zu bytes (ring buffer + stats)\n",
           sizeof(TTContext));
    printf("  EncCtx:          %zu bytes (enclosure context)\n",
           sizeof(EncCtx));

    double ctx_total = sizeof(TTContext) + sizeof(EncCtx) + sizeof(TTStoreResult);
    printf("  Total context:   %.0f bytes\n", ctx_total);
    printf("  Ring capacity:   %d chunks (%d bytes data)\n",
           TT_RING_SIZE, TT_RING_SIZE * 64);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Tensor Track Performance Benchmark                    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    srand(42);

    printf("── 1. Ingest Throughput ──\n");
    bench_ingest(100000, 48);
    bench_ingest(100000, 64);
    printf("\n");

    printf("── 2. Entropy Scoring ──\n");
    bench_entropy(500000, 48);
    bench_entropy(500000, 64);
    printf("\n");

    printf("── 3. Frame Range ──\n");
    bench_frame_range(1000000);
    printf("\n");

    printf("── 4. Route ──\n");
    bench_route(1000000);
    printf("\n");

    printf("── 5. Full Pipeline (ingest + route + store) ──\n");
    bench_full_pipeline(100000, 48);
    bench_full_pipeline(100000, 64);
    printf("\n");

    printf("── 6. Memory Footprint ──\n");
    bench_memory();
    printf("\n");

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  All benchmarks complete.\n");

    return 0;
}

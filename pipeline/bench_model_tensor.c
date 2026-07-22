/*
 * bench_model_tensor.c — Feed real GGUF model through tensor_track pipeline
 * ═══════════════════════════════════════════════════════════════════════════
 * Reads a model file in 48-byte chunks, runs through:
 *   data → rdh_capture → enc → entropy → routing (three views)
 *
 * Shows: how real model data maps to the geometric field.
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "gls_enclosure.h"
#include "tensor_track.h"
#include "geo_frame_seek.h"

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "I:/model/Qwen3-0.6B-Q8_0.gguf";

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Model → Tensor Track Pipeline (Three-View Routing)    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* Read file */
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);
    printf("File: %s (%.1f MB)\n\n", path, fsize / 1e6);

    uint8_t *buf = (uint8_t *)malloc(fsize > 0 ? fsize : 1);
    size_t nread = fread(buf, 1, fsize, fp);
    fclose(fp);

    /* Init tracker + enclosure */
    TTContext ctx;
    tt_init(&ctx);

    EncCtx enc_ctx;
    enc_init(&enc_ctx, 1);
    uint8_t store_buf[20736];

    /* Stats counters */
    uint32_t class_count[4] = {0};
    uint32_t strat_count[5] = {0};
    uint32_t tick_count[12] = {0};
    uint32_t face_count[12] = {0};
    uint32_t barrier_count = 0;
    uint32_t bridge_count = 0;

    /* Process in 48-byte chunks */
    uint32_t chunk_sz = 48;
    uint32_t n_chunks = (uint32_t)(nread / chunk_sz);

    printf("── Processing %u chunks (48B each, %.1f MB) ──\n\n",
           n_chunks, n_chunks * 48.0 / 1e6);

    double t0 = now_sec();

    /* Show first 20 chunks in detail */
    uint32_t show_max = n_chunks < 20 ? n_chunks : 20;

    for (uint32_t i = 0; i < n_chunks; i++) {
        uint8_t *data = buf + i * chunk_sz;
        TTChunkRecord rec;
        tt_ingest(&ctx, data, chunk_sz, &rec);
        TTRoute r = tt_route_record(&rec);

        /* Store in enclosure */
        TTStoreResult sr = tt_store(&enc_ctx, &rec, store_buf);
        (void)sr;

        /* Count stats */
        class_count[r.entropy_class]++;
        strat_count[r.strategy]++;
        tick_count[r.tick]++;
        face_count[r.face]++;
        if (r.tick == 0) barrier_count++;
        if (r.tick == 11) bridge_count++;

        /* Show first N chunks */
        if (i < show_max) {
            /* Print data preview (first 8 bytes) */
            printf("  [%4u] ", i);
            for (int j = 0; j < 8 && j < (int)chunk_sz; j++)
                printf("%02x", data[j]);
            printf("... ");

            printf("enc=%-4u face=%-2u slot=%-3u | pipe=%-4u tick=%-2u | flower=%-4u | %s\n",
                   r.enc, r.face, r.slot, r.pipe_id, r.tick,
                   r.flower_id,
                   r.is_compressed ? "[CMP]" : "[RAW]");
        }
    }

    double dt = now_sec() - t0;
    double throughput_mbps = nread / dt / 1e6;

    printf("\n── Results ──\n\n");

    printf("  Processed:    %u chunks (%.1f MB)\n", n_chunks, nread / 1e6);
    printf("  Time:         %.3f sec (%.1f MB/s)\n", dt, throughput_mbps);
    printf("  Throughput:   %.0f chunks/sec\n", n_chunks / dt);
    printf("  Avg latency:  %.0f ns/chunk\n", dt / n_chunks * 1e9);

    printf("\n  ── Entropy Class Distribution ──\n");
    printf("    structured (0): %u (%.1f%%)\n", class_count[0], class_count[0] * 100.0 / n_chunks);
    printf("    moderate   (1): %u (%.1f%%)\n", class_count[1], class_count[1] * 100.0 / n_chunks);
    printf("    high       (2): %u (%.1f%%)\n", class_count[2], class_count[2] * 100.0 / n_chunks);
    printf("    random     (3): %u (%.1f%%)\n", class_count[3], class_count[3] * 100.0 / n_chunks);

    printf("\n  ── Strategy Distribution ──\n");
    printf("    STANDARD (0): %u\n", strat_count[0]);
    printf("    COMPRESS (1): %u (%.1f%%)\n", strat_count[1], strat_count[1] * 100.0 / n_chunks);
    printf("    RAW      (2): %u (%.1f%%)\n", strat_count[2], strat_count[2] * 100.0 / n_chunks);
    printf("    BRIDGE_RAW (3): %u\n", strat_count[3]);
    printf("    BRIDGE_CMP (4): %u\n", strat_count[4]);

    printf("\n  ── Tick Distribution (sync axis) ──\n");
    for (int t = 0; t < 12; t++) {
        const char *label = (t == 0) ? "FREEZE" :
                           (t == 11) ? "BRIDGE" :
                           (t >= 2 && t <= 10) ? "PIPE" : "MAIN";
        printf("    tick=%2d [%-6s]: %u\n", t, label, tick_count[t]);
    }
    printf("    barrier (tick=0):  %u\n", barrier_count);
    printf("    bridge  (tick=11): %u\n", bridge_count);

    printf("\n  ── Face Distribution (icosahedron) ──\n");
    for (int f = 0; f < 12; f++) {
        printf("    face=%2d: %u\n", f, face_count[f]);
    }

    printf("\n  ── Enclosure State ──\n");
    printf("    chunks stored: %u / %u\n", enc_ctx.n_chunks, 20736u);
    printf("    total bytes:   %llu\n", (unsigned long long)enc_ctx.total_bytes);

    printf("\n═══════════════════════════════════════════════════════════\n");

    free(buf);
    return 0;
}

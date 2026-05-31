/*
 * bench_pipeline.c — POGLS Pipeline benchmark
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pogls_pipeline.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
#include <sys/time.h>
static double now_ms(void) {
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec * 1000.0 + t.tv_usec / 1000.0;
}
#endif

/* Fill buffer with controlled-range data to get ~50/50 HOT/COLD mix */
static void fill_data(uint8_t *buf, size_t sz, unsigned seed) {
    srand(seed);
    for (size_t i = 0; i < sz; i += 64) {
        int use_hot = (rand() % 2);         /* 50/50 */
        uint8_t base = (uint8_t)(rand() % 256);
        size_t end = (i + 64 < sz) ? 64 : sz - i;
        for (size_t j = 0; j < end; j++) {
            if (use_hot)
                buf[i + j] = base + (uint8_t)(j % 48);  /* range < 64 → HOT */
            else
                buf[i + j] = (uint8_t)(rand() % 256);    /* full range → COLD */
        }
    }
}

int main(void) {
    printf("POGLS Pipeline Benchmark\n");
    printf("════════════════════════\n\n");

    /* warm up verify */
    int v = pogls_pipeline_verify();
    printf("Verify: %s\n\n", v == 0 ? "PASS" : "FAIL");

    /* sizes to test */
    const size_t sizes[] = {
        1024ULL,           /* 16 chunks */
        1024ULL * 64,      /* 1024 chunks */
        1024ULL * 1024,    /* 16K chunks */
    };
    const char *labels[] = { "1 KB", "64 KB", "1 MB" };

    /* overflow buffer + occupancy flags (occ follows entries) */
#define OVERFLOW_CAP 4096
    static uint8_t overflow_mem[sizeof(ColdEntry) * OVERFLOW_CAP + OVERFLOW_CAP];
    static ColdEntry *overflow = (ColdEntry *)overflow_mem;

    for (int si = 0; si < 3; si++) {
        size_t data_sz = sizes[si];
        uint8_t *data = (uint8_t *)malloc(data_sz);
        if (!data) { printf("OOM\n"); return 1; }

        fill_data(data, data_sz, (unsigned)(42 + si));
        uint64_t n_chunks = (data_sz + PGFE_CHUNK_SZ - 1) / PGFE_CHUNK_SZ;

        /* ── Encode ── */
        PoglsPipeline pipe;
        pogls_pipeline_init(&pipe, 0xBEEFCAFE42ULL, 12u, 256u, 1u,
                             NULL, NULL, overflow, OVERFLOW_CAP);

        double t0 = now_ms();
        int rc = pogls_encode(&pipe, data, data_sz);
        double t1 = now_ms();
        double enc_ms = t1 - t0;

        PipelineStats es = pogls_stats(&pipe);

        /* ── Decode ── */
        double dec_ms = 0;
        uint64_t dec_ok = 0, dec_miss = 0, cold_ok = 0, cold_total = 0;
        uint8_t *recovered = (uint8_t *)calloc(1, data_sz);
        if (recovered) {
            double td0 = now_ms();
            for (uint64_t ci = 0; ci < n_chunks; ci++) {
                uint8_t out64[PGFE_CHUNK_SZ];
                memset(out64, 0, PGFE_CHUNK_SZ);
                int r = pogls_decode_chunk(&pipe, ci, out64, NULL);
                if (r >= 0) {
                    if (r == 0) { /* COLD: data written */
                        memcpy(recovered + ci * PGFE_CHUNK_SZ, out64, PGFE_CHUNK_SZ);
                        cold_ok += (memcmp(out64, data + ci * PGFE_CHUNK_SZ, PGFE_CHUNK_SZ) == 0);
                        cold_total++;
                    }
                    /* HOT: leave zeros in recovered (no FGLS store in bench) */
                    dec_ok++;
                } else {
                    dec_miss++;
                }
            }
            double td1 = now_ms();
            dec_ms = td1 - td0;

            /* verify correctness (COLD chunks only) */
            int cold_match = (cold_ok == cold_total);
            printf("─── %s (%llu chunks) ───\n", labels[si],
                   (unsigned long long)n_chunks);
            printf("  Encode:  %.3f ms  (%.2f MB/s)\n",
                   enc_ms, (double)data_sz / (1024.0 * 1024.0) / (enc_ms / 1000.0));
            printf("  Decode:  %.3f ms  (%.2f MB/s)\n",
                   dec_ms, (double)data_sz / (1024.0 * 1024.0) / (dec_ms / 1000.0));
            printf("  Total:   %.3f ms\n", enc_ms + dec_ms);
            printf("  Decoded: %llu  miss: %llu\n",
                   (unsigned long long)dec_ok, (unsigned long long)dec_miss);
            printf("  COLD correct: %llu/%llu  %s\n",
                   (unsigned long long)cold_ok, (unsigned long long)cold_total,
                   cold_match ? "PASS" : "FAIL");
            printf("  HOT: %llu  COLD: %llu  (%.0f%% cold)\n",
                   (unsigned long long)es.hot, (unsigned long long)es.cold,
                   es.hot + es.cold > 0 ? 100.0 * es.cold / (es.hot + es.cold) : 0.0);
            printf("  Cold ring: %u/%u  overflow: %u/%u  evict: %u\n",
                   es.cold_ring_count, POGLS_COLD_RING_CAP,
                   es.cold_overflow_count, OVERFLOW_CAP,
                   es.cold_evictions);
        }
        free(recovered);
        free(data);
        printf("\n");
    }

    return 0;
}

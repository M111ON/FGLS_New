/* ═══════════════════════════════════════════════════════════════════════════
 * kis_cli_test.c — Standalone test for KIS codec in CLI context
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "core/kis_codec_v4.h"
#include "collection/fgls_profile.h"

int main(void) {
    printf("╔══ KIS CODEC CLI Integration Test ══╗\n\n");

    /* Test 1: Profile detection for Q8_0-like data */
    printf("Test 1: Profile detection for Q8_0 data...\n");
    {
        uint32_t n = 1024;
        int8_t *weights = (int8_t *)malloc(n);
        srand(42);
        for (uint32_t i = 0; i < n; i++) {
            /* Simulate Q8_0 distribution: mostly non-zero, full int8 range */
            weights[i] = (int8_t)(rand() & 0xFF);
        }

        FglsProfile p;
        fgls_profile((uint8_t *)weights, n, &p);
        FglsRoute r = fgls_route(&p);

        printf("  Profile: size=%u, nz=%u (%.1f%%), uniq=%u, max=%u\n",
               p.size, p.nonzero_count, p.nonzero_count * 100.0 / p.size,
               p.unique_values, p.max_value);
        printf("  Entropy=%.3f bits/B, Locality=%.3f\n",
               p.entropy_x1000 / 1000.0, p.locality_x1000 / 1000.0);
        printf("  Route: %s (expected: KIS)\n", fgls_route_name(r));

        free(weights);
    }

    /* Test 2: Encode/decode with profile routing */
    printf("\nTest 2: Full encode/decode via route...\n");
    {
        uint32_t n = 4096;
        int8_t *weights = (int8_t *)malloc(n);
        srand(12345);
        for (uint32_t i = 0; i < n; i++) {
            weights[i] = (int8_t)(rand() & 0xFF);
        }

        /* Profile and route */
        FglsProfile p;
        fgls_profile((uint8_t *)weights, n, &p);
        FglsRoute r = fgls_route(&p);

        if (r != FGLS_ROUTE_KIS) {
            printf("  WARNING: Got route %s instead of KIS\n", fgls_route_name(r));
        } else {
            printf("  Correctly routed to KIS\n");
        }

        /* Encode */
        clock_t t0 = clock();
        uint32_t buf_size = n * 2;
        uint8_t *buf = (uint8_t *)malloc(buf_size);
        uint32_t enc = kis_v4_encode(weights, n, buf, buf_size);
        clock_t t1 = clock();

        /* Decode */
        int8_t *decoded = (int8_t *)malloc(n);
        int rc = kis_v4_decode(buf, enc, decoded, n);
        clock_t t2 = clock();

        /* Verify */
        uint32_t mismatches = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (decoded[i] != weights[i]) mismatches++;
        }

        double enc_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000;
        double dec_ms = (double)(t2 - t1) / CLOCKS_PER_SEC * 1000;

        printf("  Encode: %.2f ms, Decode: %.2f ms\n", enc_ms, dec_ms);
        printf("  Codec: %u bytes, Raw: %u bytes, Ratio: %.2fx\n",
               enc, n, (double)n / enc);
        printf("  Mismatches: %u / %u (expected: 0)\n", mismatches, n);

        free(weights);
        free(buf);
        free(decoded);
    }

    /* Test 3: Chunk-level encoding (simulating CLI chunk size) */
    printf("\nTest 3: Chunk-level encoding (64B chunks)...\n");
    {
        uint32_t total_size = 4096;
        int8_t *weights = (int8_t *)malloc(total_size);
        srand(99999);
        for (uint32_t i = 0; i < total_size; i++) {
            weights[i] = (int8_t)(rand() & 0xFF);
        }

        uint32_t chunk_size = 64;
        uint32_t n_chunks = total_size / chunk_size;
        uint32_t total_enc = 0;
        uint32_t route_hist[10] = {0};

        clock_t t0 = clock();

        for (uint32_t i = 0; i < n_chunks; i++) {
            uint8_t *chunk = (uint8_t *)(weights + i * chunk_size);

            /* Profile chunk */
            FglsProfile p;
            fgls_profile(chunk, chunk_size, &p);
            FglsRoute r = fgls_route(&p);
            route_hist[r]++;

            /* Encode chunk with KIS */
            uint8_t enc_buf[512];
            uint32_t enc = kis_v4_encode((int8_t *)chunk, chunk_size, enc_buf, sizeof(enc_buf));
            total_enc += enc;
        }

        clock_t t1 = clock();
        double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000;

        printf("  Chunks: %u × %uB\n", n_chunks, chunk_size);
        printf("  Total encoded: %u bytes (%.2fx)\n", total_enc, (double)total_size / total_enc);
        printf("  Time: %.2f ms\n", elapsed);
        printf("  Route distribution:\n");
        for (int i = 0; i < 10; i++) {
            if (route_hist[i] > 0) {
                printf("    %s: %u chunks\n", fgls_route_name(i), route_hist[i]);
            }
        }

        free(weights);
    }

    printf("\n══════════════════════════════════════\n");
    printf("  KIS CLI Integration Test Complete\n");
    printf("══════════════════════════════════════\n");
    return 0;
}
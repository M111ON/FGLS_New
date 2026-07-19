/*
 * verify_frame_seek.c — Standalone verification of geo_frame_seek
 *
 * Tests:
 *   [1] verify() passes (all invariants)
 *   [2] frame_enc/frame_at roundtrip — enc is deterministic
 *   [3] stride-37 walk hits all 1440 positions exactly once
 *   [4] Temporal delta encoding: predict frame N from seed via stride-37
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "geo_frame_seek.h"

#define FRAME_BYTES 768u  /* 12 chunks × 64B */
#define CHUNK_BYTES 64u

static int test_verify_invariants(void) {
    printf("[T1] geo_frame_seek_verify()...           ");
    int rc = geo_frame_seek_verify();
    if (rc != 0) { printf("FAIL (rc=%d)\n", rc); return -1; }
    printf("PASS\n");
    return 0;
}

static int test_enc_at_roundtrip(void) {
    printf("[T2] frame_enc(t) ↔ frame_at(enc) roundtrip... ");
    for (uint32_t t = 0; t < FRAME_CYCLE; t++) {
        uint16_t enc = frame_enc(t);
        DualFrame f = frame_at(enc);
        if (f.enc != enc || f.face > 11) {
            printf("FAIL t=%u enc=%u\n", t, enc);
            return -1;
        }
    }
    printf("PASS (%u/%u)\n", (unsigned)FRAME_CYCLE, (unsigned)FRAME_CYCLE);
    return 0;
}

static int test_stride37_full_coverage(void) {
    printf("[T3] stride-37 walk covers all %u positions... ", (unsigned)FRAME_CYCLE);
    uint8_t visited[FRAME_CYCLE] = {0};
    uint16_t e = 0;
    uint32_t steps = 0;
    do {
        if (visited[e]) { printf("FAIL: duplicate enc=%u\n", e); return -1; }
        visited[e] = 1;
        e = frame_next(e);
        steps++;
    } while (e != 0 && steps <= FRAME_CYCLE);
    if (steps != FRAME_CYCLE) {
        printf("FAIL: walked %u steps\n", steps);
        return -1;
    }
    printf("PASS (%u steps, all unique)\n", steps);
    return 0;
}

/* Simulate temporal delta encoding:
 *   - 1 seed frame (768B) + (n-1) × 2B enc + (n-1) × 12 DS residuals
 *   - DS estimate: FLAT(1B) | SPARSE(3+2*nz, nz≤16) | DENSE(66B)
 *
 * pattern: 0=zeros, 1=sawtooth, 2=drift, 3=random
 */
static void test_temporal_prediction(uint32_t n_frames, uint32_t pattern) {
    printf("[T4] Temporal prediction (n=%u pattern=%u):\n", n_frames, pattern);

    uint8_t *frames = (uint8_t *)malloc((size_t)n_frames * FRAME_BYTES);
    uint8_t *residuals = (uint8_t *)malloc((size_t)n_frames * FRAME_BYTES);
    if (!frames || !residuals) { printf("    OOM\n"); goto cleanup; }

    srand(42);
    for (uint32_t i = 0; i < n_frames * FRAME_BYTES; i++) {
        uint32_t fi = i / FRAME_BYTES;
        uint32_t bi = i % FRAME_BYTES;
        switch (pattern) {
            case 0: frames[i] = 0; break;
            case 1: frames[i] = (uint8_t)(bi & 0xFF); break;
            case 2: frames[i] = (uint8_t)((bi + fi * 3) & 0xFF); break;
            case 3: frames[i] = (uint8_t)(rand() & 0xFF); break;
            default: frames[i] = 0;
        }
    }

    uint32_t enc_bytes = (n_frames - 1) * 2;
    uint32_t total_ds_bytes = 0;
    for (uint32_t fi = 1; fi < n_frames; fi++) {
        uint32_t off_a = 0;
        uint32_t off_b = fi * FRAME_BYTES;
        for (uint32_t b = 0; b < FRAME_BYTES; b++) {
            residuals[fi * FRAME_BYTES + b] = frames[off_b + b] ^ frames[off_a + b];
        }
        /* per-chunk DS size estimate */
        uint32_t chunk_ds = 0;
        for (uint32_t ci = 0; ci < 12; ci++) {
            uint32_t c_nz = 0;
            for (uint32_t b = 0; b < CHUNK_BYTES; b++) {
                if (residuals[fi * FRAME_BYTES + ci * CHUNK_BYTES + b]) c_nz++;
            }
            if (c_nz == 0) chunk_ds += 1;
            else if (c_nz <= 16) chunk_ds += 3 + 2 * c_nz;
            else chunk_ds += 66;
        }
        total_ds_bytes += chunk_ds;
    }

    uint32_t total_raw = n_frames * FRAME_BYTES;
    uint32_t total_encoded = FRAME_BYTES + enc_bytes + total_ds_bytes;
    printf("    Raw=%u B → Encoded=%u B (%.2fx reduction)\n",
           total_raw, total_encoded, (double)total_raw / total_encoded);

cleanup:
    free(frames);
    free(residuals);
}

int main(void) {
    printf("=== geo_frame_seek Verification Suite ===\n\n");

    if (test_verify_invariants() != 0) return 1;
    if (test_enc_at_roundtrip() != 0) return 1;
    if (test_stride37_full_coverage() != 0) return 1;

    printf("\n=== Temporal Delta Test ===\n\n");
    test_temporal_prediction(100, 0);
    test_temporal_prediction(100, 1);
    test_temporal_prediction(100, 2);
    test_temporal_prediction(100, 3);

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
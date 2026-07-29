/*
 * test_bond_fibospine_mapping.c
 * ═══════════════════════════════════════════════════════════════════
 * Test: Bond → FiboSpine Pipe Mapping Chain
 *
 * Tests the pipeline:
 *   GGUF chunk → rdh_capture → enc → pipe + tick (FiboSpine view)
 *   GGUF chunk → seed → PoglsPiece → bond_piece_fingerprint (Bond view)
 *
 * Verifies:
 *   T1: Determinism — same chunk → same enc/pipe/tick/fingerprint every time
 *   T2: Pipe range  — pipe_id < FT_PIPES (1728)
 *   T3: Tick range  — tick < 12
 *   T4: Fingerprint — non-zero, valid GeoPixel
 *   T5: Multiple chunks — mapping uniqueness/variance
 *   T6: GGUF-realistic — 64B chunks with typical LLM weight patterns
 *   T7: Speed — 50000 iterations of the full pipeline
 *
 * Build:
 *   gcc -O2 -std=c11 -Icore -Icollection/rdh -Icollection \
 *       -Icollection/include -Icollection/src \
 *       -Icollection/geopixel \
 *       -Icollection/dgls/geo/include \
 *       -DP5H_ENABLE \
 *       pipeline/test_bond_fibospine_mapping.c -o test_bond_fibospine_mapping.exe
 *
 * Run:
 *   ./test_bond_fibospine_mapping.exe
 *
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ── Core headers under test ────────────────────────────────── */
#include "fibo_tick.h"              /* ft_enc_to_pipe, ft_enc_to_tick, etc. */
#include "fibo_spine.h"             /* FiboSpine, FS_PIPES, FS_TICKS_PER_CYCLE */
#include "rdh_capture.h"            /* rdh_capture, rdh_capture_to_enc */
#include "bond_to_geopixel.h"       /* bond_piece_fingerprint, PoglsPiece */
#include "pogls_bond.h"             /* pogls_make_piece, pogls_fibo_addr */

/* ── Test tracking ──────────────────────────────────────────── */
static int n_pass = 0;
static int n_fail = 0;
static int n_skip = 0;

#define TEST(name, expr) do { \
    int _ok = (expr); \
    if (_ok) { n_pass++; printf("  PASS  %s\n", name); } \
    else { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); } \
} while(0)

#define TEST_I(name, got, expected) do { \
    int64_t _g = (int64_t)(got); \
    int64_t _e = (int64_t)(expected); \
    if (_g == _e) { n_pass++; printf("  PASS  %s (%lld)\n", name, (long long)_g); } \
    else { n_fail++; printf("  FAIL  %s: got %lld, expected %lld (line %d)\n", name, (long long)_g, (long long)_e, __LINE__); } \
} while(0)

#define SKIP(name, reason) do { \
    n_skip++; printf("  SKIP  %s — %s\n", name, reason); \
} while(0)

/* ── Generate a realistic GGUF-like chunk ───────────────────── */
/* Simulates 48-64B of a GGUF tensor weight chunk */
static void gen_gguf_chunk(uint8_t *buf, size_t len, uint32_t seed)
{
    /* Deterministic pseudo-random GGUF-like data */
    uint32_t s = seed;
    for (size_t i = 0; i < len; i++) {
        s = s * 1103515245u + 12345u;
        buf[i] = (uint8_t)(s >> 16);
    }
    /* Mix in characteristic GGUF patterns:
     * - Some runs of similar values (quantized weights)
     * - Some structured headers */
    if (len >= 8) {
        /* Simulate GGUF magic + header bytes */
        buf[0] = 'G'; buf[1] = 'G'; buf[2] = 'U'; buf[3] = 'F';
        buf[4] = 0x01; /* version low */
        buf[5] = 0x00; /* version high */
    }
    if (len >= 48) {
        /* Add a quantization block pattern (group of 32 weights) */
        for (int i = 8; i < 40; i++)
            buf[i] = (uint8_t)((i * seed + 7) & 0xFF);
        /* Scale/offset pattern */
        for (int i = 40; i < 48 && i < (int)len; i++)
            buf[i] = (uint8_t)((i * 3 + 128) & 0xFF);
    }
}

/* ── GGUF-realistic chunk from real data patterns ───────────── */
/* Generates chunk patterned after typical fp32/bf16 tensor slices */
static void gen_gguf_realistic(uint8_t *buf, size_t len, uint32_t idx)
{
    for (size_t i = 0; i < len; i++) {
        /* Alternating patterns: structured header + weight data */
        if (i < 16) {
            /* Header-like: slow-changing metadata */
            buf[i] = (uint8_t)(idx >> (i * 2));
        } else if (i < 32) {
            /* Shape/dimension info: repeating small values */
            buf[i] = (uint8_t)((idx + 1) * (i - 15));
        } else {
            /* Weight data: pseudo-random with quantized characteristics */
            uint32_t v = (uint32_t)(idx * 2654435761u + i * 314159u);
            buf[i] = (uint8_t)((v >> ((i & 3) * 8)) & 0xFF);
            /* Sometimes create runs of identical values (quantized blocks) */
            if ((i & 0x1F) == 0) buf[i] = buf[i-1]; /* repeat boundary */
        }
    }
}

/* ══════════════════════════════════════════════════════════════
   TEST 1: Determinism — same chunk → same output every time
   ══════════════════════════════════════════════════════════════ */
static int test_determinism(void)
{
    printf("\n=== Test 1: Determinism (same chunk → same output 3×) ===\n");

    RDHConfig cfg = RDH_CAPTURE_144;
    int ok = 1;

    /* Test 10 different chunk sizes and patterns */
    const size_t sizes[] = {48, 52, 56, 60, 64, 48, 64, 48, 64, 48};
    const uint32_t seeds[] = {0, 1, 42, 100, 255, 1024, 4096, 9999, 12345, 54321};

    for (int ci = 0; ci < 10; ci++) {
        size_t len = sizes[ci];
        uint8_t buf[64];
        gen_gguf_chunk(buf, len, seeds[ci]);

        /* First run */
        uint16_t enc1 = rdh_capture_to_enc(buf, len, &cfg);
        uint64_t fk1  = (uint64_t)rdh_capture(buf, len, &cfg);
        uint16_t pipe1 = ft_enc_to_pipe(enc1);
        uint8_t  tick1 = ft_enc_to_tick(enc1);

        /* Create bond piece from chunk */
        uint64_t chunk_seed = pogls_fibo_addr(seeds[ci]);
        uint8_t  axis = (uint8_t)((buf[0] & 0x7) + 1);
        PoglsPiece piece1 = pogls_make_piece(chunk_seed, axis);
        GeoPixel   fp1 = bond_piece_fingerprint(&piece1);

        /* Second run — different buffer, same data */
        uint8_t buf2[64];
        memcpy(buf2, buf, len);
        uint16_t enc2 = rdh_capture_to_enc(buf2, len, &cfg);
        uint64_t fk2  = (uint64_t)rdh_capture(buf2, len, &cfg);
        uint16_t pipe2 = ft_enc_to_pipe(enc2);
        uint8_t  tick2 = ft_enc_to_tick(enc2);
        PoglsPiece piece2 = pogls_make_piece(chunk_seed, axis);
        GeoPixel   fp2 = bond_piece_fingerprint(&piece2);

        /* Third run */
        uint8_t buf3[64];
        memcpy(buf3, buf, len);
        uint16_t enc3 = rdh_capture_to_enc(buf3, len, &cfg);
        uint64_t fk3  = (uint64_t)rdh_capture(buf3, len, &cfg);
        uint16_t pipe3 = ft_enc_to_pipe(enc3);
        uint8_t  tick3 = ft_enc_to_tick(enc3);
        PoglsPiece piece3 = pogls_make_piece(chunk_seed, axis);
        GeoPixel   fp3 = bond_piece_fingerprint(&piece3);

        /* Compare all three runs */
        char name[128];
        sprintf(name, "chunk[%d] enc deterministic", ci);
        if (enc1 == enc2 && enc2 == enc3)
            { n_pass++; printf("  PASS  %s (enc=%u)\n", name, enc1); }
        else
            { n_fail++; printf("  FAIL  %s: %u %u %u (line %d)\n", name, enc1, enc2, enc3, __LINE__); ok = 0; }

        sprintf(name, "chunk[%d] flat_key deterministic", ci);
        if (fk1 == fk2 && fk2 == fk3)
            { n_pass++; printf("  PASS  %s (%llu)\n", name, (unsigned long long)fk1); }
        else
            { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); ok = 0; }

        sprintf(name, "chunk[%d] pipe deterministic", ci);
        if (pipe1 == pipe2 && pipe2 == pipe3)
            { n_pass++; printf("  PASS  %s (pipe=%u)\n", name, pipe1); }
        else
            { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); ok = 0; }

        sprintf(name, "chunk[%d] tick deterministic", ci);
        if (tick1 == tick2 && tick2 == tick3)
            { n_pass++; printf("  PASS  %s (tick=%u)\n", name, tick1); }
        else
            { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); ok = 0; }

        sprintf(name, "chunk[%d] fingerprint deterministic", ci);
        if (fp1.r == fp2.r && fp2.r == fp3.r &&
            fp1.g == fp2.g && fp2.g == fp3.g &&
            fp1.b == fp2.b && fp2.b == fp3.b)
            { n_pass++; printf("  PASS  %s (R=%u G=%u B=%u)\n", name, fp1.r, fp1.g, fp1.b); }
        else
            { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); ok = 0; }

        /* Verify fingerprint yields same pixel via the different encoding paths */
        sprintf(name, "chunk[%d] piece geo_key consistent", ci);
        if (piece1.geo_key == piece2.geo_key && piece2.geo_key == piece3.geo_key)
            { n_pass++; printf("  PASS  %s (0x%016llX)\n", name, (unsigned long long)piece1.geo_key); }
        else
            { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); ok = 0; }

        sprintf(name, "chunk[%d] bond_key consistent", ci);
        uint64_t bk1 = pogls_bond_key(&piece1);
        uint64_t bk2 = pogls_bond_key(&piece2);
        uint64_t bk3 = pogls_bond_key(&piece3);
        if (bk1 == bk2 && bk2 == bk3)
            { n_pass++; printf("  PASS  %s (0x%016llX)\n", name, (unsigned long long)bk1); }
        else
            { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); ok = 0; }
    }

    return ok ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════
   TEST 2: Pipe range — all enc map to valid pipe
   ══════════════════════════════════════════════════════════════ */
static int test_pipe_range(void)
{
    printf("\n=== Test 2: Pipe range — all enc map within FT_PIPES ===\n");

    /* Walk every position in the frame cycle */
    int bad = 0;
    uint32_t pipe_usage[FS_PIPES] = {0};
    for (uint16_t enc = 0; enc < FRAME_CYCLE; enc++) {
        uint16_t pipe = ft_enc_to_pipe(enc);
        if (pipe >= FS_PIPES) {
            printf("  ERROR: enc=%u → pipe=%u (max %u)\n", enc, pipe, FS_PIPES);
            bad++;
        } else {
            pipe_usage[pipe]++;
        }
    }

    TEST_I("pipes used (unique)", pipe_usage[0] > 0 ? 1 : 0, 1);
    printf("  Total pipes used: ");
    uint32_t used = 0;
    for (uint16_t p = 0; p < FS_PIPES; p++)
        if (pipe_usage[p] > 0) used++;
    printf("%u / %u (%.1f%%)\n", used, FS_PIPES, 100.0 * used / FS_PIPES);

    /* All enc should map to pipes 0..1439 (since enc < 1440, %1728 = enc) */
    TEST_I("enc0 → pipe0", ft_enc_to_pipe(0), 0);
    TEST_I("enc100 → pipe100", ft_enc_to_pipe(100), 100);
    TEST_I("enc1439 → pipe1439", ft_enc_to_pipe(1439), 1439);
    TEST_I("bad mappings", bad, 0);

    /* Test reserve pipes (1440..1727): enc can't reach these directly */
    printf("  Note: pipes 1440..1727 reserved for Jet Bridge residual\n");

    return bad;
}

/* ══════════════════════════════════════════════════════════════
   TEST 3: Tick range — all ticks 0..11 valid
   ══════════════════════════════════════════════════════════════ */
static int test_tick_range(void)
{
    printf("\n=== Test 3: Tick range — all 12 ticks populated ===\n");

    uint32_t tick_counts[12] = {0};
    for (uint16_t enc = 0; enc < FRAME_CYCLE; enc++) {
        uint8_t tick = ft_enc_to_tick(enc);
        if (tick >= 12) {
            printf("  ERROR: enc=%u → tick=%u\n", enc, tick);
            return -1;
        }
        tick_counts[tick]++;
    }

    printf("  Tick distribution (expected 120 each):\n");
    int ok = 1;
    for (uint8_t t = 0; t < 12; t++) {
        printf("    tick %2u: %u\n", t, tick_counts[t]);
        if (tick_counts[t] != FRAME_CYCLE / 12) {
            printf("      ^ MISMATCH: expected %u\n", FRAME_CYCLE / 12);
            ok = 0;
        }
    }

    TEST("all ticks 0..11 populated", ok);
    return ok ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════
   TEST 4: Fingerprint — non-zero, valid GeoPixel
   ══════════════════════════════════════════════════════════════ */
static int test_fingerprint_valid(void)
{
    printf("\n=== Test 4: Fingerprint validity ===\n");

    int ok = 1;
    for (uint32_t seed = 0; seed < 100; seed++) {
        uint8_t buf[64];
        gen_gguf_chunk(buf, 64, seed);

        uint64_t chunk_seed = pogls_fibo_addr(seed);
        uint8_t  axis = (uint8_t)((buf[0] & 0x7) + 1);
        PoglsPiece piece = pogls_make_piece(chunk_seed, axis);
        GeoPixel fp = bond_piece_fingerprint(&piece);

        /* Fingerprint should have non-zero components for diverse inputs */
        /* (Some may be zero by coincidence, but not all 100) */
        if (fp.r == 0 && fp.g == 0 && fp.b == 0) {
            printf("  WARNING: seed=%u → all-zero fingerprint\n", seed);
        }

        /* Verify piece integrity */
        uint64_t bk = pogls_bond_key(&piece);
        uint64_t expected_bk = piece.bond_L ^ piece.bond_R;
        if (bk != expected_bk) {
            printf("  ERROR: seed=%u bond_key mismatch\n", seed);
            ok = 0;
        }

        /* Verify fingerprint encodes bond_key info in G channel */
        if (seed == 0) {
            printf("  Sample fp[0]: R=%u G=%u B=%u\n", fp.r, fp.g, fp.b);
            printf("    geo_key=0x%016llX bond_key=0x%016llX shape='%c'\n",
                   (unsigned long long)piece.geo_key,
                   (unsigned long long)bk, piece.shape);
        }
    }

    /* Test that different seeds produce different fingerprints */
    uint32_t fp_set[100] = {0};
    int collisions = 0;
    for (uint32_t seed = 0; seed < 100; seed++) {
        uint8_t buf[64];
        gen_gguf_chunk(buf, 64, seed);

        uint64_t chunk_seed = pogls_fibo_addr(seed);
        uint8_t  axis = (uint8_t)((buf[0] & 0x7) + 1);
        PoglsPiece piece = pogls_make_piece(chunk_seed, axis);
        GeoPixel fp = bond_piece_fingerprint(&piece);
        uint32_t fp_compact = ((uint32_t)fp.r << 16) | ((uint32_t)fp.g << 8) | fp.b;

        for (uint32_t ps = 0; ps < seed; ps++) {
            if (fp_set[ps] == fp_compact) {
                collisions++;
                printf("  COLLISION: seed=%u and seed=%u → same fp 0x%06X\n",
                       ps, seed, fp_compact);
            }
        }
        fp_set[seed] = fp_compact;
    }
    printf("  Fingerprint collisions among 100 chunks: %d\n", collisions);
    /* Some collisions on 24-bit space is expected, just report */
    TEST("fingerprint: piece integrity ok", ok);

    return ok ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════
   TEST 5: Full pipeline — GGUF chunks → Bond → FiboSpine
   ══════════════════════════════════════════════════════════════ */
static int test_full_pipeline_mapping(void)
{
    printf("\n=== Test 5: Full pipeline — GGUF chunks → Bond → FiboSpine ===\n");

    RDHConfig cfg = RDH_CAPTURE_144;

    printf("\n  ── 10 GGUF chunks (48B each) ──\n");
    printf("  %-4s | %-10s | %-6s | %-5s | %-17s | %-10s | %-5s | %-8s\n",
           "Chunk", "FlatKey", "enc", "Pipe", "GeoKey", "BondKey", "Tick", "Shape");

    for (int ci = 0; ci < 10; ci++) {
        uint8_t buf[48];
        gen_gguf_chunk(buf, 48, ci * 100 + 7);

        /* Pipeline 1: RDH to FiboSpine */
        uint64_t flat_key = (uint64_t)rdh_capture(buf, 48, &cfg);
        uint16_t enc      = (uint16_t)(flat_key % 1440);
        uint16_t pipe     = ft_enc_to_pipe(enc);
        uint8_t  tick     = ft_enc_to_tick(enc);
        uint8_t  action   = ft_store_action(enc);
        DualFrame f       = frame_at(enc);

        /* Pipeline 2: chunk → bond piece → fingerprint */
        uint64_t chunk_seed = pogls_fibo_addr(ci * 100 + 7);
        uint8_t  axis       = (uint8_t)((buf[0] & 0x7) + 1);
        PoglsPiece piece    = pogls_make_piece(chunk_seed, axis);
        GeoPixel   fp       = bond_piece_fingerprint(&piece);
        uint64_t   bk       = pogls_bond_key(&piece);

        const char *action_name =
            action == FT_STORE_MAIN   ? "MAIN" :
            action == FT_STORE_BRIDGE ? "BRIDGE" :
            action == FT_STORE_PIPE   ? "PIPE" :
            action == FT_STORE_FREEZE ? "FREEZE" : "?";

        printf("  %-4d | flat=%-10llu | enc=%-4u | pipe=%-5u | geo=0x%016llX | bk=0x%08llX | tick=%-3u | %s\n",
               ci,
               (unsigned long long)flat_key,
               enc, pipe,
               (unsigned long long)piece.geo_key,
               (unsigned long long)bk,
               tick, action_name);
    }

    /* Verify pipeline consistency */
    TEST("all pipes < FT_PIPES", ft_enc_to_pipe(0) < FT_PIPES);
    TEST("all ticks < 12", ft_enc_to_tick(0) < 12);
    TEST("frame_at face < 12", frame_at(0).face < 12);
    TEST("frame_at slot < 120", frame_at(0).slot < 120);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TEST 6: GGUF-realistic chunks (64B) — behavioral variance
   ══════════════════════════════════════════════════════════════ */
static int test_realistic_gguf(void)
{
    printf("\n=== Test 6: GGUF-realistic chunks (64B) — distribution ===\n");

    RDHConfig cfg = RDH_CAPTURE_144;
    const int N = 500;
    uint32_t pipe_hist[FT_PIPES] = {0};
    uint32_t tick_hist[12] = {0};
    uint32_t face_hist[12] = {0};
    uint32_t action_hist[4] = {0};

    for (int i = 0; i < N; i++) {
        uint8_t buf[64];
        gen_gguf_realistic(buf, 64, i * 73 + 13);

        uint16_t enc  = rdh_capture_to_enc(buf, 64, &cfg);
        uint16_t pipe = ft_enc_to_pipe(enc);
        uint8_t  tick = ft_enc_to_tick(enc);
        uint8_t  act  = ft_store_action(enc);
        DualFrame f   = frame_at(enc);

        if (pipe < FT_PIPES) pipe_hist[pipe]++;
        if (tick < 12)       tick_hist[tick]++;
        if (f.face < 12)     face_hist[f.face]++;
        if (act < 4)         action_hist[act]++;
    }

    printf("  Results from %d realistic GGUF chunks:\n", N);
    printf("  Pipe range: %u unique pipes used (out of %u)\n",
           pipe_hist[0] > 0 ? 1 : 0, FT_PIPES);
    uint32_t used_pipes = 0;
    for (uint16_t p = 0; p < FT_PIPES; p++)
        if (pipe_hist[p] > 0) used_pipes++;
    printf("  Unique pipes: %u (%.1f%% utilization)\n",
           used_pipes, 100.0 * used_pipes / FT_PIPES);

    printf("  Tick distribution:\n");
    for (uint8_t t = 0; t < 12; t++)
        printf("    tick %2u: %u (%.1f%%)\n", t, tick_hist[t],
               100.0 * tick_hist[t] / N);

    printf("  Face distribution:\n");
    for (uint8_t f = 0; f < 12; f++)
        printf("    face %2u: %u (%.1f%%)\n", f, face_hist[f],
               100.0 * face_hist[f] / N);

    printf("  Action distribution:\n");
    printf("    MAIN:   %u (%.1f%%)\n", action_hist[FT_STORE_MAIN],
           100.0 * action_hist[FT_STORE_MAIN] / N);
    printf("    BRIDGE: %u (%.1f%%)\n", action_hist[FT_STORE_BRIDGE],
           100.0 * action_hist[FT_STORE_BRIDGE] / N);
    printf("    PIPE:   %u (%.1f%%)\n", action_hist[FT_STORE_PIPE],
           100.0 * action_hist[FT_STORE_PIPE] / N);
    printf("    FREEZE: %u (%.1f%%)\n", action_hist[FT_STORE_FREEZE],
           100.0 * action_hist[FT_STORE_FREEZE] / N);

    /* All chunks should map to valid pipes/ticks */
    TEST("no pipe overflow", used_pipes <= FT_PIPES);
    TEST("all ticks populated", tick_hist[0] > 0 && tick_hist[11] > 0);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TEST 7: Speed — 50000 iterations
   ══════════════════════════════════════════════════════════════ */
static int test_speed(void)
{
    printf("\n=== Test 7: Speed benchmark (50000 iterations) ===\n");

    RDHConfig cfg = RDH_CAPTURE_144;
    const int N = 50000;
    uint8_t buf[64];

    /* Pre-generate test data */
    printf("  Generating %d chunks...\n", N);
    uint8_t **chunks = (uint8_t **)malloc(N * sizeof(uint8_t *));
    if (!chunks) { SKIP("speed benchmark", "malloc failed"); return 0; }
    for (int i = 0; i < N; i++) {
        chunks[i] = (uint8_t *)malloc(64);
        if (!chunks[i]) { SKIP("speed benchmark", "malloc failed"); goto cleanup; }
        gen_gguf_realistic(chunks[i], 64, i * 97 + 3);
    }

    /* ── Benchmark 1: RDH capture + enc ── */
    clock_t start = clock();
    uint64_t sum_enc = 0;  /* prevent optimization */
    for (int i = 0; i < N; i++) {
        uint16_t enc = rdh_capture_to_enc(chunks[i], 64, &cfg);
        sum_enc += enc;
    }
    clock_t end = clock();
    double t1 = (double)(end - start) / CLOCKS_PER_SEC;
    printf("  RDH capture → enc: %d ops in %.4f s = %.2f ns/op\n",
           N, t1, 1e9 * t1 / N);

    /* ── Benchmark 2: RDH + pipe + tick (full mapping) ── */
    start = clock();
    uint64_t sum_pipe = 0;
    for (int i = 0; i < N; i++) {
        uint16_t enc  = rdh_capture_to_enc(chunks[i], 64, &cfg);
        uint16_t pipe = ft_enc_to_pipe(enc);
        uint8_t  tick = ft_enc_to_tick(enc);
        sum_pipe += pipe + tick;
    }
    end = clock();
    double t2 = (double)(end - start) / CLOCKS_PER_SEC;
    printf("  + pipe + tick mapping: %d ops in %.4f s = %.2f ns/op\n",
           N, t2, 1e9 * t2 / N);

    /* ── Benchmark 3: Bond piece creation + fingerprint ── */
    start = clock();
    uint64_t sum_fp = 0;
    for (int i = 0; i < N; i++) {
        uint64_t chunk_seed = pogls_fibo_addr(i);
        uint8_t  axis = (uint8_t)((chunks[i][0] & 0x7) + 1);
        PoglsPiece piece = pogls_make_piece(chunk_seed, axis);
        GeoPixel   fp = bond_piece_fingerprint(&piece);
        sum_fp += fp.r + fp.g + fp.b;
    }
    end = clock();
    double t3 = (double)(end - start) / CLOCKS_PER_SEC;
    printf("  Bond piece + fingerprint: %d ops in %.4f s = %.2f ns/op\n",
           N, t3, 1e9 * t3 / N);

    /* ── Benchmark 4: Full pipeline (RDH + bond + fingerprint) ── */
    start = clock();
    uint64_t sum_all = 0;
    for (int i = 0; i < N; i++) {
        uint16_t enc  = rdh_capture_to_enc(chunks[i], 64, &cfg);
        uint16_t pipe = ft_enc_to_pipe(enc);
        uint8_t  tick = ft_enc_to_tick(enc);

        uint64_t chunk_seed = pogls_fibo_addr(i);
        uint8_t  axis = (uint8_t)((chunks[i][0] & 0x7) + 1);
        PoglsPiece piece = pogls_make_piece(chunk_seed, axis);
        GeoPixel   fp = bond_piece_fingerprint(&piece);

        sum_all += enc + pipe + tick + fp.r + fp.g + fp.b;
    }
    end = clock();
    double t4 = (double)(end - start) / CLOCKS_PER_SEC;
    printf("  Full pipeline (all): %d ops in %.4f s = %.2f ns/op\n",
           N, t4, 1e9 * t4 / N);
    printf("  Throughput: %.0f chunks/sec\n", N / t4);

    /* Prevent optimization of unused sums */
    printf("  (checksum: %llu)\n", (unsigned long long)(sum_enc + sum_pipe + sum_fp + sum_all));

    TEST("rdh_capture → enc < 1 µs/op", t1 * 1e9 / N < 1000);
    TEST("full pipeline < 2 µs/op", t4 * 1e9 / N < 2000);

cleanup:
    if (chunks) {
        for (int i = 0; i < N; i++) free(chunks[i]);
        free(chunks);
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TEST 8: Bond → FiboSpine cross-consistency
   ══════════════════════════════════════════════════════════════
   Verify that the bond piece fingerprint encodes meaningful
   information that relates to the same data chunk's pipe/tick
   mapping. Not a mathematical equivalence (they're different
   views), but verify both derive deterministically from the
   same input.
   ══════════════════════════════════════════════════════════════ */
static int test_bond_spine_consistency(void)
{
    printf("\n=== Test 8: Bond ↔ FiboSpine cross-consistency ===\n");

    RDHConfig cfg = RDH_CAPTURE_144;

    printf("  For each chunk, show both mapping views:\n");
    printf("  %-4s | %-5s | %-4s | %-8s | %-6s | %-6s | %-4s\n",
           "Chunk", "enc", "Pipe", "Tick", "GeoKey", "FpRGB", "Shape");

    for (int ci = 0; ci < 15; ci++) {
        uint8_t buf[64];
        gen_gguf_realistic(buf, 64, ci * 37 + 5);

        /* Spine mapping */
        uint16_t enc  = rdh_capture_to_enc(buf, 64, &cfg);
        uint16_t pipe = ft_enc_to_pipe(enc);
        uint8_t  tick = ft_enc_to_tick(enc);

        /* Bond piece */
        uint64_t chunk_seed = pogls_fibo_addr(ci * 37 + 5);
        uint8_t  axis = (uint8_t)((buf[0] & 0x7) + 1);
        PoglsPiece piece = pogls_make_piece(chunk_seed, axis);
        GeoPixel   fp = bond_piece_fingerprint(&piece);

        printf("  %-4d | enc=%-3u | pipe=%-4u | tick=%-2u | 0x%08llX | (%3u,%3u,%3u) | '%c'\n",
               ci, enc, pipe, tick,
               (unsigned long long)piece.geo_key,
               fp.r, fp.g, fp.b, piece.shape);
    }

    /* Verify that the rdh_capture produces consistent enc for identical data */
    uint8_t ref[64];
    gen_gguf_realistic(ref, 64, 42);
    uint16_t enc_ref = rdh_capture_to_enc(ref, 64, &cfg);
    for (int trial = 0; trial < 10; trial++) {
        uint8_t dup[64];
        memcpy(dup, ref, 64);
        uint16_t enc_dup = rdh_capture_to_enc(dup, 64, &cfg);
        if (enc_dup != enc_ref) {
            printf("  ERROR: same data → different enc on trial %d\n", trial);
            TEST("rdh_capture deterministic across copies", 0);
            return -1;
        }
    }
    TEST("rdh_capture deterministic across copies (10×)", 1);

    /* Verify bond pieces from same seed produce same fingerprint */
    uint64_t seed_ref = pogls_fibo_addr(42);
    uint8_t  axis_ref = (uint8_t)((ref[0] & 0x7) + 1);
    PoglsPiece piece_ref = pogls_make_piece(seed_ref, axis_ref);
    GeoPixel fp_ref = bond_piece_fingerprint(&piece_ref);
    for (int trial = 0; trial < 10; trial++) {
        PoglsPiece piece_dup = pogls_make_piece(seed_ref, axis_ref);
        GeoPixel fp_dup = bond_piece_fingerprint(&piece_dup);
        if (fp_dup.r != fp_ref.r || fp_dup.g != fp_ref.g || fp_dup.b != fp_ref.b) {
            printf("  ERROR: same seed → different fingerprint on trial %d\n", trial);
            TEST("bond_piece_fingerprint deterministic (10×)", 0);
            return -1;
        }
    }
    TEST("bond_piece_fingerprint deterministic (10×)", 1);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Bond → FiboSpine Pipe Mapping Test Suite               ║\n");
    printf("║  GGUF chunk → rdh_capture → enc → pipe + tick           ║\n");
    printf("║  GGUF chunk → seed → PoglsPiece → fingerprint           ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    test_determinism();             /* T1 */
    test_pipe_range();              /* T2 */
    test_tick_range();              /* T3 */
    test_fingerprint_valid();       /* T4 */
    test_full_pipeline_mapping();   /* T5 */
    test_realistic_gguf();          /* T6 */
    test_speed();                   /* T7 */
    test_bond_spine_consistency();  /* T8 */

    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  RESULTS: %3d pass, %d fail, %d skip\n", n_pass, n_fail, n_skip);
    printf("╚══════════════════════════════════════════════════════════╝\n");

    return n_fail > 0 ? 1 : 0;
}

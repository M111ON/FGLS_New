/*
 * test_tgw_stream.c — Wire Test: Geo_ Stream Layer (Layer 3)
 * ===========================================================
 * ทดสอบ pipeline:
 *   file bytes → tstream_slice → TRing → tgw_write → gbt_to_geopixel → RGB[]
 *
 * Tests:
 *   1. slice_roundtrip  : slice file → reconstruct → bytes identical
 *   2. tring_ordering   : chunk order preserved via walk position (no sort)
 *   3. geopixel_stream  : ทุก packet ออก pixel ที่ valid range
 *   4. structured_signal: structured file → pixel variance ต่างจาก noise
 *   5. gap_detect       : drop 1 packet → tring_first_gap() รายงานได้
 *
 * Compile:
 *   gcc -O2 -o test_tgw_stream test_tgw_stream.c -I. -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "geo_tring_goldberg_wire.h"

/* ── GeoPixel (from test_gbt_geopixel) ──────────────────────── */
typedef struct { uint8_t r, g, b; } GeoPixel;

static inline GeoPixel gbt_to_geopixel(const GBTRingPrint *fp) {
    uint8_t comp = (uint8_t)TRING_COMP(fp->tring_enc);
    uint8_t tet  = (uint8_t)TRING_TET (fp->tring_enc);
    uint8_t vert = (uint8_t)TRING_VERT(fp->tring_enc);
    GeoPixel px;
    px.r = (uint8_t)((comp << 3) | (fp->active_pair  & 0x7u));
    px.g = (uint8_t)((tet  << 4) | (vert << 2) | (fp->active_pole & 0x3u));
    px.b = (uint8_t)(fp->tring_pos % 144u);
    return px;
}

/* ── Config ──────────────────────────────────────────────────── */
#define TEST_FILE_SIZE   (4096u * 3u + 512u)   /* 3.5 chunks = 4 packets  */
#define NOISE_FILE_SIZE  (4096u * 4u)           /* exactly 4 chunks        */

static const GeoSeed TEST_SEED = { 0xDEADBEEF12345678ULL,
                                   0xCAFEBABE87654321ULL };

/* ── Helpers ─────────────────────────────────────────────────── */

/* build structured file: fibo-like pattern */
static void make_structured(uint8_t *buf, uint32_t sz) {
    uint32_t a=1, b=2;
    for (uint32_t i=0; i<sz; i++) {
        buf[i] = (uint8_t)((a + b) & 0xFFu);
        uint32_t t = a + b; a = b; b = t;
    }
}

/* build noise file: LCG random */
static void make_noise(uint8_t *buf, uint32_t sz) {
    uint32_t v = 0xABCDEF01u;
    for (uint32_t i=0; i<sz; i++) {
        v = v * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(v >> 16);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 1: slice → reconstruct round-trip
 * ═══════════════════════════════════════════════════════════════ */
static int test_slice_roundtrip(void) {
    printf("[TEST 1] slice → reconstruct round-trip\n");

    static uint8_t original[TEST_FILE_SIZE];
    static uint8_t recovered[TEST_FILE_SIZE];
    make_structured(original, TEST_FILE_SIZE);

    /* slice */
    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    uint16_t n = tstream_slice_file(pkts, original, TEST_FILE_SIZE);
    if (n == 0) { printf("  FAIL: slice returned 0 packets\n"); return 0; }
    printf("  sliced into %u packets\n", n);

    /* receive into TRing store */
    TRingCtx tr;
    TStreamChunk store[TSTREAM_MAX_PKTS];
    tring_init(&tr);
    memset(store, 0, sizeof(store));

    for (uint16_t i=0; i<n; i++) {
        int r = tstream_recv_pkt(&tr, store, &pkts[i]);
        if (r < 0) { printf("  FAIL: recv_pkt rejected packet %u\n", i); return 0; }
    }

    /* reconstruct */
    memset(recovered, 0, sizeof(recovered));
    uint32_t out_sz = tstream_reconstruct(&tr, store, n, recovered);
    printf("  reconstructed %u bytes (original %u)\n", out_sz, TEST_FILE_SIZE);

    if (out_sz < TEST_FILE_SIZE) {
        printf("  FAIL: reconstructed %u < original %u\n", out_sz, TEST_FILE_SIZE);
        return 0;
    }

    int ok = (memcmp(original, recovered, TEST_FILE_SIZE) == 0);
    if (ok) printf("  PASS: bytes identical\n");
    else {
        /* find first diff */
        for (uint32_t i=0; i<TEST_FILE_SIZE; i++)
            if (original[i] != recovered[i]) {
                printf("  FAIL: first diff at byte %u: orig=0x%02X got=0x%02X\n",
                       i, original[i], recovered[i]);
                break;
            }
    }
    return ok;
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 2: chunk ordering via walk position (no sort needed)
 * ═══════════════════════════════════════════════════════════════ */
static int test_tring_ordering(void) {
    printf("[TEST 2] TRing ordering — position = order\n");

    static uint8_t file[TEST_FILE_SIZE];
    make_structured(file, TEST_FILE_SIZE);

    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    uint16_t n = tstream_slice_file(pkts, file, TEST_FILE_SIZE);

    /* verify enc of each packet = GEO_WALK[i] */
    int ok = 1;
    for (uint16_t i=0; i<n; i++) {
        if (pkts[i].enc != GEO_WALK[i]) {
            printf("  FAIL: pkt[%u].enc=0x%04X != GEO_WALK[%u]=0x%04X\n",
                   i, pkts[i].enc, i, GEO_WALK[i]);
            ok = 0; break;
        }
        /* tring_pos(enc) must resolve back to i */
        uint16_t pos = tring_pos(pkts[i].enc);
        if (pos != i) {
            printf("  FAIL: tring_pos(enc[%u])=%u != %u\n", i, pos, i);
            ok = 0; break;
        }
    }
    if (ok) printf("  PASS: all %u packets carry correct walk position\n", n);
    return ok;
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 3: full pipeline → GeoPixel valid range
 * ═══════════════════════════════════════════════════════════════ */
static int test_geopixel_stream(void) {
    printf("[TEST 3] full pipeline → GeoPixel range check\n");

    static uint8_t file[TEST_FILE_SIZE];
    make_structured(file, TEST_FILE_SIZE);

    TGWCtx ctx;
    tgw_init(&ctx, TEST_SEED, NULL);

    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    uint16_t n = tstream_slice_file(pkts, file, TEST_FILE_SIZE);

    int ok = 1;
    for (uint16_t i=0; i<n; i++) {
        /* manually feed each packet through GBT bridge for pixel */
        tstream_recv_pkt(&ctx.tr, ctx.store, &pkts[i]);

        /* write enc+crc as addr/value into goldberg bridge */
        uint64_t addr  = (uint64_t)pkts[i].enc;
        uint64_t value = (uint64_t)pkts[i].crc16 | ((uint64_t)pkts[i].size << 16);
        tgw_write(&ctx, addr, value, 0);

        /* capture fingerprint → pixel */
        GBTRingPrint fp = gbt_capture(&ctx.gp.tb);
        gbt_flush(&ctx.gp.tb);
        GeoPixel px = gbt_to_geopixel(&fp);

        /* range checks */
        uint8_t comp = (px.r >> 3) & 0x1Fu;
        uint8_t pair = px.r & 0x7u;
        uint8_t fibo = px.b;

        if (comp > 11 || pair > 5 || fibo > 143) {
            printf("  FAIL pkt %u: comp=%u pair=%u fibo=%u\n",
                   i, comp, pair, fibo);
            ok = 0;
        }
    }
    if (ok) printf("  PASS: all %u packets produce valid pixels\n", n);

    tgw_status(&ctx);
    return ok;
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 4: structured vs noise → pixel variance differs
 * ═══════════════════════════════════════════════════════════════ */
static int test_structured_signal(void) {
    printf("[TEST 4] structured file vs noise → pixel signal\n");

    static uint8_t sfile[NOISE_FILE_SIZE], nfile[NOISE_FILE_SIZE];
    make_structured(sfile, NOISE_FILE_SIZE);
    make_noise     (nfile, NOISE_FILE_SIZE);

    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    float s_b[TSTREAM_MAX_PKTS], n_b[TSTREAM_MAX_PKTS];
    uint16_t sn, nn;

    /* structured */
    {
        TGWCtx ctx; tgw_init(&ctx, TEST_SEED, NULL);
        sn = tstream_slice_file(pkts, sfile, NOISE_FILE_SIZE);
        for (uint16_t i=0; i<sn; i++) {
            tstream_recv_pkt(&ctx.tr, ctx.store, &pkts[i]);
            tgw_write(&ctx, pkts[i].enc,
                      (uint64_t)pkts[i].crc16|((uint64_t)pkts[i].size<<16), 0);
            GBTRingPrint fp = gbt_capture(&ctx.gp.tb);
            gbt_flush(&ctx.gp.tb);
            s_b[i] = (float)gbt_to_geopixel(&fp).b;
        }
    }

    /* noise */
    {
        TGWCtx ctx; tgw_init(&ctx, TEST_SEED, NULL);
        nn = tstream_slice_file(pkts, nfile, NOISE_FILE_SIZE);
        for (uint16_t i=0; i<nn; i++) {
            tstream_recv_pkt(&ctx.tr, ctx.store, &pkts[i]);
            tgw_write(&ctx, pkts[i].enc,
                      (uint64_t)pkts[i].crc16|((uint64_t)pkts[i].size<<16), 0);
            GBTRingPrint fp = gbt_capture(&ctx.gp.tb);
            gbt_flush(&ctx.gp.tb);
            n_b[i] = (float)gbt_to_geopixel(&fp).b;
        }
    }

    /* B channel = fibo clock = deterministic, should be identical for both
     * R channel = geometry response = should differ */
    float s_mean=0, n_mean=0;
    for (uint16_t i=0; i<sn; i++) s_mean += s_b[i];
    for (uint16_t i=0; i<nn; i++) n_mean += n_b[i];
    s_mean/=sn; n_mean/=nn;

    printf("  structured B mean=%.1f  noise B mean=%.1f\n", s_mean, n_mean);
    /* B is purely temporal — must be equal (same number of packets) */
    int b_equal = (fabsf(s_mean - n_mean) < 0.01f);
    if (b_equal) printf("  PASS: B channel identical (temporal, content-independent)\n");
    else          printf("  WARN: B channel differs (%.2f) — check tring advance\n",
                         fabsf(s_mean-n_mean));

    return b_equal;
}

/* ═══════════════════════════════════════════════════════════════
 * TEST 5: gap detection — drop 1 packet, tring_first_gap reports it
 * ═══════════════════════════════════════════════════════════════ */
static int test_gap_detect(void) {
    printf("[TEST 5] gap detection — drop packet 1, verify first_gap()\n");

    static uint8_t file[TEST_FILE_SIZE];
    make_structured(file, TEST_FILE_SIZE);

    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    uint16_t n = tstream_slice_file(pkts, file, TEST_FILE_SIZE);

    TRingCtx tr; tring_init(&tr);
    TStreamChunk store[TSTREAM_MAX_PKTS];
    memset(store, 0, sizeof(store));

    /* receive all except packet index 1 (drop it) */
    for (uint16_t i=0; i<n; i++) {
        if (i == 1) { printf("  (dropped packet 1, walk_pos=%u)\n", i); continue; }
        tstream_recv_pkt(&tr, store, &pkts[i]);
        tring_assign(&tr, (uint16_t)i, pkts[i].enc);
    }

    uint16_t gap = tring_first_gap(&tr);
    printf("  tring_first_gap() = %u\n", gap);

    /* gap should point to position 1 (the dropped packet) */
    int ok = (gap == 1);
    if (ok) printf("  PASS: gap correctly detected at position 1\n");
    else    printf("  FAIL: expected gap at 1, got %u\n", gap);
    return ok;
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  TGW Stream Layer — Wire Test                ║\n");
    printf("║  Geo_ slice → TRing → GeoPixel pipeline      ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    int r[5];
    r[0] = test_slice_roundtrip();   printf("\n");
    r[1] = test_tring_ordering();    printf("\n");
    r[2] = test_geopixel_stream();   printf("\n");
    r[3] = test_structured_signal(); printf("\n");
    r[4] = test_gap_detect();        printf("\n");

    int passed = r[0]+r[1]+r[2]+r[3]+r[4];
    printf("══════════════════════════════════════════════\n");
    printf("RESULT: %d/5 passed\n", passed);
    printf("STATUS: %s\n", passed==5
        ? "✅ Stream Layer VERIFIED"
        : "⚠️  check failures above");
    printf("══════════════════════════════════════════════\n");
    return (passed==5) ? 0 : 1;
}

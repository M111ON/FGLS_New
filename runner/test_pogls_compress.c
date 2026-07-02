/*
 * test_pogls_compress.c — Compression benchmark for .pogls v2
 *
 * Tests zstd + binary shell on realistic synthetic data.
 *
 * Build:
 *   gcc -O2 -std=c11 -I. -I../collection -I../collection/Hfolder
 *       -I../collection/dgls/diamond/include
 *       -o test_pogls_compress.exe test_pogls_compress.c zstd.dll -lm
 *
 * Run:
 *   .\test_pogls_compress.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <zstd.h>

#include "diamond_shell_v2.h"
#include "diamond_shell_codec.h"
#include "binary_shell_codec.h"

static int pass=0, fail=0;

static double now_s(void) {
    clock_t c = clock();
    return (double)c / (double)CLOCKS_PER_SEC;
}

/* xorshift64 PRNG — deterministic, non-cryptographic */
static uint64_t xs64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x; return x;
}

/* Generate realistic Q4_K_M tensor data:
 *   ~130M parameters, block size 256, packed as 16B scales + 128B 4-bit weights
 *   per 144B block. Random within block-structure to mimic quantized residuals.
 */
static void gen_q4_weights(uint8_t *buf, size_t sz, uint64_t seed) {
    for (size_t off = 0; off < sz; off += 144) {
        size_t remain = sz - off;
        /* 16B scale factors — non-uniform, block-correlated */
        for (size_t i = 0; i < 16 && i < remain; i++)
            buf[off + i] = (uint8_t)(xs64(&seed) & 0xFF);
        /* 128B packed 4-bit weights — dense, structured noise */
        for (size_t i = 16; i < 144 && i < remain; i++)
            buf[off + i] = (uint8_t)((xs64(&seed) & 0xF) | ((xs64(&seed) & 0xF) << 4));
    }
}

/* Generate realistic norm data: 100% non-zero, [-128,127] range */
static void gen_norm(uint8_t *buf, size_t sz, uint64_t seed) {
    for (size_t i = 0; i < sz; i++)
        buf[i] = (uint8_t)(xs64(&seed) & 0xFF);
}

/* Generate sparse bias data: ~80% zeros, ~20% non-zero small values */
static void gen_bias(uint8_t *buf, size_t sz, uint64_t seed) {
    memset(buf, 0, sz);
    uint32_t nz = (uint32_t)(sz / 5);
    if (nz < 1) nz = 1;
    for (uint32_t i = 0; i < nz; i++) {
        uint32_t pos = (uint32_t)(xs64(&seed) % sz);
        buf[pos] = (uint8_t)(xs64(&seed) & 0xFF);
    }
}

/* ── Phase 1: ZSTD full tensor ── */
static void test_zstd_full(const uint8_t *data, size_t sz,
                            const char *label, const char *desc) {
    size_t bound = ZSTD_compressBound(sz);
    uint8_t *comp = (uint8_t*)malloc(bound);
    uint8_t *dec  = (uint8_t*)malloc(sz);
    if (!comp || !dec) { fprintf(stderr,"OOM\n"); exit(1); }

    /* Level 3 (fast) */
    double t0 = now_s();
    size_t csz = ZSTD_compress(comp, bound, data, sz, 3);
    double t1 = now_s();
    if (ZSTD_isError(csz)) goto fail;

    double ratio = (double)sz / (double)csz;
    double enc_speed = (double)sz / (t1 - t0) / (1024*1024);

    double t2 = now_s();
    size_t dsz = ZSTD_decompress(dec, sz, comp, (size_t)csz);
    double t3 = now_s();
    if (ZSTD_isError(dsz) || dsz != sz) goto fail;

    double dec_speed = (double)sz / (t3 - t2) / (1024*1024);
    int ok = (memcmp(data, dec, sz) == 0);
    if (!ok) goto fail;

    fprintf(stderr, "  %-24s zstd3  r=%5.2f enc=%5.0f dec=%5.0f MB/s  %s\n",
            label, ratio, enc_speed, dec_speed, desc);
    pass += 3;

    /* Level 12 (better compression) — only if >1MB */
    if (sz > 1024*1024) {
        t0 = now_s();
        size_t csz12 = ZSTD_compress(comp, bound, data, sz, 12);
        t1 = now_s();
        if (ZSTD_isError(csz12)) { free(comp); free(dec); return; }
        double ratio12 = (double)sz / (double)csz12;
        double enc12 = (double)sz / (t1 - t0) / (1024*1024);
        t2 = now_s();
        dsz = ZSTD_decompress(dec, sz, comp, (size_t)csz12);
        t3 = now_s();
        if (ZSTD_isError(dsz) || dsz != sz) { free(comp); free(dec); return; }
        double dec12 = (double)sz / (t3 - t2) / (1024*1024);
        if (memcmp(data, dec, sz) != 0) { free(comp); free(dec); return; }
        fprintf(stderr, "  %-24s zstd12 r=%5.2f enc=%5.0f dec=%5.0f MB/s\n",
                label, ratio12, enc12, dec12);
        pass += 3;
    }

    free(comp); free(dec);
    return;

fail:
    fprintf(stderr, "  %-24s ERROR\n", label);
    fail++;
    free(comp); free(dec);
}

/* ── Phase 2: Binary shell per-chunk ── */
static void test_binary_shell(const uint8_t *data, size_t sz,
                               const char *label) {
    uint64_t n_chunks = (sz + 63) / 64;
    uint64_t max_enc = n_chunks * 70 + 16;
    uint8_t *enc = (uint8_t*)malloc(max_enc);
    uint8_t *dec = (uint8_t*)calloc(n_chunks, 64);
    if (!enc || !dec) { fprintf(stderr,"OOM\n"); exit(1); }

    double t0 = now_s();
    uint64_t enc_pos = 0;
    uint64_t flat=0, sparse=0, dense=0;
    for (uint64_t i = 0; i < n_chunks; i++) {
        BinChunkResult r;
        uint32_t cs = bin_encode_chunk(enc + enc_pos, data + i*64, &r);
        enc_pos += cs;
        if (r.flag == BIN_FLAG_FLAT) flat++;
        else if (r.flag == BIN_FLAG_SPARSE) sparse++;
        else dense++;
    }
    double t1 = now_s();

    double ratio = (double)sz / (double)enc_pos;
    if (ratio < 0.1) { fprintf(stderr,"  %-24s bin    r=%.2f BAD\n",label,ratio); fail++; free(enc); free(dec); return; }
    double enc_speed = (double)sz / (t1 - t0) / (1024*1024);
    pass++;

    double t2 = now_s();
    uint64_t dec_pos = 0;
    for (uint64_t i = 0; i < n_chunks; i++)
        dec_pos += bin_decode_chunk(enc + dec_pos, dec + i*64);
    double t3 = now_s();
    double dec_speed = (double)sz / (t3 - t2) / (1024*1024);

    int ok = (memcmp(data, dec, sz) == 0 && dec_pos == enc_pos);
    if (!ok) { fprintf(stderr,"  %-24s bin    DATA MISMATCH\n",label); fail++; free(enc); free(dec); return; }
    pass += 2;

    fprintf(stderr, "  %-24s bin    r=%5.2f enc=%5.0f dec=%5.0f MB/s  f=%llu s=%llu d=%llu\n",
            label, ratio, enc_speed, dec_speed,
            (unsigned long long)flat, (unsigned long long)sparse, (unsigned long long)dense);
    free(enc); free(dec);
}

/* ── Phase 3: Diamond shell stream ── */
static void test_shell_stream(const uint8_t *data, size_t sz,
                               const char *label) {
    uint64_t n_chunks = (sz + 63) / 64;
    uint64_t max_enc = n_chunks * 66 + 16;
    uint8_t *enc = (uint8_t*)malloc(max_enc);
    uint8_t *dec = (uint8_t*)calloc(n_chunks, 64);
    if (!enc || !dec) { fprintf(stderr,"OOM\n"); exit(1); }

    double t0 = now_s();
    uint64_t enc_sz = shell_stream_encode(data, n_chunks, enc);
    double t1 = now_s();

    double ratio = (double)sz / (double)enc_sz;
    if (ratio < 0.1) { fprintf(stderr,"  %-24s shell r=%.2f BAD\n",label,ratio); fail++; free(enc); free(dec); return; }
    double enc_speed = (double)sz / (t1 - t0) / (1024*1024);
    pass++;

    double t2 = now_s();
    uint64_t consumed = shell_stream_decode(enc, n_chunks, dec);
    double t3 = now_s();
    double dec_speed = (double)sz / (t3 - t2) / (1024*1024);

    int ok = (consumed == enc_sz && memcmp(data, dec, n_chunks * 64) == 0);
    if (!ok) { fprintf(stderr,"  %-24s shell DATA MISMATCH\n",label); fail++; free(enc); free(dec); return; }
    pass += 2;

    fprintf(stderr, "  %-24s shell r=%5.2f enc=%5.0f dec=%5.0f MB/s\n",
            label, ratio, enc_speed, dec_speed);
    free(enc); free(dec);
}

int main(void) {
    fprintf(stderr, "═══ POGLS Compression Benchmark ═══\n\n");

    /* Sizes matching real model proportions */
    size_t weight_sz = 512 * 1024 * 1024;  /* 512 MB — ~130M params Q4 */
    size_t norm_sz   = 8 * 1024 * 1024;    /* 8 MB — layer norms */
    size_t bias_sz   = 512 * 1024;          /* 512 KB — biases */
    uint64_t seed = 42;

    fprintf(stderr, "Allocating test data...\n");
    uint8_t *weights = (uint8_t*)calloc(weight_sz, 1);
    uint8_t *norms   = (uint8_t*)calloc(norm_sz, 1);
    uint8_t *biases  = (uint8_t*)calloc(bias_sz, 1);
    if (!weights || !norms || !biases) { fprintf(stderr,"OOM\n"); return 1; }

    gen_q4_weights(weights, weight_sz, seed);
    gen_norm(norms, norm_sz, seed + 100);
    gen_bias(biases, bias_sz, seed + 200);

    /* ── ZSTD ── */
    fprintf(stderr, "\n── ZSTD (full tensor) ──\n");
    test_zstd_full(weights, weight_sz, "Q4 weights",
                   "512MB Q4_K_M quantized");
    test_zstd_full(norms, norm_sz, "layer norms",
                   "8MB f32 norm params");
    test_zstd_full(biases, bias_sz, "biases",
                   "512KB f32 bias params");

    /* ── Binary shell (per-chunk) ── */
    fprintf(stderr, "\n── Binary Shell (per-chunk) ──\n");
    test_binary_shell(weights, weight_sz, "Q4 weights");
    test_binary_shell(norms, norm_sz, "layer norms");
    test_binary_shell(biases, bias_sz, "biases");

    /* ── Diamond shell (stream) ── */
    fprintf(stderr, "\n── Diamond Shell (stream) ──\n");
    test_shell_stream(weights, weight_sz, "Q4 weights");
    test_shell_stream(norms, norm_sz, "layer norms");
    test_shell_stream(biases, bias_sz, "biases");

    free(weights); free(norms); free(biases);

    fprintf(stderr, "\n═══ %d pass, %d fail ═══\n", pass, fail);
    return fail > 0 ? 1 : 0;
}

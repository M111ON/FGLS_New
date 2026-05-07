/*
 * bench_compress.c — Tring pipeline vs zstd/lz4 raw tile compression
 * compile: gcc -O2 -o bench_compress bench_compress.c -lzstd -llz4
 *
 * Test: synthetic 1536x1536 RGB image (gradient+noise like neon photo)
 * Measure: encode time + output size for each method
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <zstd.h>
#include <lz4.h>
#include <lz4hc.h>
#include "geo_tring_addr.h"

/* ── Config ─────────────────────────────────────────────────────── */
#define IMG_W       1536
#define IMG_H       1536
#define TILE_SIZE     32
#define TILES_X     (IMG_W / TILE_SIZE)
#define TILES_Y     (IMG_H / TILE_SIZE)
#define TOTAL_TILES (TILES_X * TILES_Y)
#define N_CH          3
#define TILE_BYTES  (TILE_SIZE * TILE_SIZE * N_CH)   /* 3072 bytes/tile */
#define IMG_BYTES   (IMG_W * IMG_H * N_CH)           /* 7,077,888 bytes */

/* ── Timer ───────────────────────────────────────────────────────── */
static inline double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* ── Synthetic image (neon-like: gradient + noise) ───────────────── */
static uint8_t *gen_image(void) {
    uint8_t *img = malloc(IMG_BYTES);
    uint32_t lcg = 0xDEADBEEF;
    for (int y = 0; y < IMG_H; y++) {
        for (int x = 0; x < IMG_W; x++) {
            lcg = lcg * 1664525u + 1013904223u;
            uint8_t noise = (uint8_t)(lcg >> 24) & 0x1F;
            uint8_t r = (uint8_t)(x * 255 / IMG_W) + noise;
            uint8_t g = (uint8_t)(y * 255 / IMG_H) + noise;
            uint8_t b = (uint8_t)(255 - x * 255 / IMG_W) + noise;
            int base = (y * IMG_W + x) * N_CH;
            img[base+0] = r; img[base+1] = g; img[base+2] = b;
        }
    }
    return img;
}

/* ── Extract tile to flat buffer ─────────────────────────────────── */
static void extract_tile(const uint8_t *img, int tx, int ty, uint8_t *tile) {
    int x0 = tx * TILE_SIZE, y0 = ty * TILE_SIZE;
    for (int y = 0; y < TILE_SIZE; y++) {
        int src = ((y0+y) * IMG_W + x0) * N_CH;
        memcpy(tile + y * TILE_SIZE * N_CH, img + src, TILE_SIZE * N_CH);
    }
}

/* ── Tring quantize tile (apply threshold per zone) ─────────────── */
static inline uint8_t classify_zone(TringAddr a) {
    if (a.pattern_id < 48) return 0;
    if (a.pattern_id < 96) return 1;
    return 2;
}
static const uint8_t THR_TABLE[3][2] = {{2,4},{1,2},{0,0}};

static size_t tring_encode_tile(const uint8_t *img, int tx, int ty,
                                 int mode, uint8_t *out, size_t out_cap) {
    uint8_t tile[TILE_BYTES];
    extract_tile(img, tx, ty, tile);

    /* apply per-channel threshold quantization */
    for (int ch = 0; ch < N_CH; ch++) {
        TringAddr ta  = tring_encode((uint8_t)tx, (uint8_t)ty, (uint8_t)ch);
        uint8_t   thr = THR_TABLE[classify_zone(ta)][mode];
        if (thr == 0) continue;
        /* quantize: round to nearest thr */
        for (int i = ch; i < TILE_BYTES; i += N_CH) {
            tile[i] = (uint8_t)((tile[i] / thr) * thr);
        }
    }

    /* delta encode within tile (simple spatial predictor) */
    uint8_t delta[TILE_BYTES];
    delta[0] = tile[0];
    for (int i = 1; i < TILE_BYTES; i++)
        delta[i] = (uint8_t)(tile[i] - tile[i-1]);

    /* zstd compress the quantized+delta tile */
    size_t csize = ZSTD_compress(out, out_cap, delta, TILE_BYTES, 3);
    if (ZSTD_isError(csize)) return TILE_BYTES; /* fallback: raw */
    return csize;
}

/* ── Method: raw zstd (no tring, just compress tile as-is) ──────── */
static size_t raw_zstd_tile(const uint8_t *img, int tx, int ty,
                              int level, uint8_t *out, size_t out_cap) {
    uint8_t tile[TILE_BYTES];
    extract_tile(img, tx, ty, tile);
    size_t csize = ZSTD_compress(out, out_cap, tile, TILE_BYTES, level);
    if (ZSTD_isError(csize)) return TILE_BYTES;
    return csize;
}

/* ── Method: raw lz4 ────────────────────────────────────────────── */
static size_t raw_lz4_tile(const uint8_t *img, int tx, int ty,
                             uint8_t *out, size_t out_cap) {
    uint8_t tile[TILE_BYTES];
    extract_tile(img, tx, ty, tile);
    int csize = LZ4_compress_default((char*)tile, (char*)out,
                                      TILE_BYTES, (int)out_cap);
    return csize > 0 ? (size_t)csize : TILE_BYTES;
}

/* ── Method: lz4hc ──────────────────────────────────────────────── */
static size_t raw_lz4hc_tile(const uint8_t *img, int tx, int ty,
                               uint8_t *out, size_t out_cap) {
    uint8_t tile[TILE_BYTES];
    extract_tile(img, tx, ty, tile);
    int csize = LZ4_compress_HC((char*)tile, (char*)out,
                                 TILE_BYTES, (int)out_cap, LZ4HC_CLEVEL_DEFAULT);
    return csize > 0 ? (size_t)csize : TILE_BYTES;
}

/* ── Run one method over all tiles ──────────────────────────────── */
typedef enum { M_TRING_BAL, M_TRING_AGG, M_ZSTD3, M_ZSTD19, M_LZ4, M_LZ4HC } Method;

static void run_method(const uint8_t *img, Method m,
                        double *out_ms, size_t *out_bytes) {
    static const char *names[] = {
        "Tring+Balanced", "Tring+Aggressive",
        "zstd-3 (raw)", "zstd-19 (raw)", "lz4 (raw)", "lz4hc (raw)"
    };

    size_t out_cap = TILE_BYTES + 64;
    uint8_t *out   = malloc(out_cap);
    size_t   total = 0;

    double t0 = now_ms();
    for (int ty = 0; ty < TILES_Y; ty++) {
        for (int tx = 0; tx < TILES_X; tx++) {
            size_t cs = 0;
            switch(m) {
                case M_TRING_BAL: cs = tring_encode_tile(img,tx,ty,0,out,out_cap); break;
                case M_TRING_AGG: cs = tring_encode_tile(img,tx,ty,1,out,out_cap); break;
                case M_ZSTD3:     cs = raw_zstd_tile(img,tx,ty,3,out,out_cap);    break;
                case M_ZSTD19:    cs = raw_zstd_tile(img,tx,ty,19,out,out_cap);   break;
                case M_LZ4:       cs = raw_lz4_tile(img,tx,ty,out,out_cap);       break;
                case M_LZ4HC:     cs = raw_lz4hc_tile(img,tx,ty,out,out_cap);     break;
            }
            total += cs;
        }
    }
    double t1 = now_ms();

    *out_ms    = t1 - t0;
    *out_bytes = total;

    double ratio  = (double)IMG_BYTES / total;
    double mb_out = total / (1024.0 * 1024.0);
    double mb_s   = (IMG_BYTES / (1024.0*1024.0)) / (*out_ms / 1000.0);

    printf("  %-20s  %6.1f ms  %6.2f MB  ratio=%.2fx  %6.1f MB/s\n",
           names[m], *out_ms, mb_out, ratio, mb_s);

    free(out);
}

/* ── Main ────────────────────────────────────────────────────────── */
int main(void) {
    printf("=== Tring vs Compressor Benchmark ===\n");
    printf("Image: %dx%d RGB  Raw=%.2f MB  Tiles=%d (%dx%d @ %dpx)\n\n",
           IMG_W, IMG_H, IMG_BYTES/1024.0/1024.0,
           TOTAL_TILES, TILES_X, TILES_Y, TILE_SIZE);

    /* verify bijection first */
    uint32_t errs = tring_roundtrip_verify();
    printf("Tring bijection: %s\n\n", errs==0 ? "OK" : "FAIL");

    uint8_t *img = gen_image();

    printf("%-20s  %8s  %8s  %8s  %8s\n",
           "Method", "Time", "Size", "Ratio", "Speed");
    printf("%-20s  %8s  %8s  %8s  %8s\n",
           "------", "----", "----", "-----", "-----");

    double ms; size_t bytes;
    run_method(img, M_LZ4,       &ms, &bytes);
    run_method(img, M_LZ4HC,     &ms, &bytes);
    run_method(img, M_ZSTD3,     &ms, &bytes);
    run_method(img, M_TRING_BAL, &ms, &bytes);
    run_method(img, M_TRING_AGG, &ms, &bytes);
    run_method(img, M_ZSTD19,    &ms, &bytes);

    printf("\nNote: Tring=quantize+delta+zstd3, lossless zone (33%%) untouched\n");

    free(img);
    return 0;
}

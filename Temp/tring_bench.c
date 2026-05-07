/*
 * tring_bench.c — Tring-Enhanced Tile Pipeline
 * compile: gcc -O2 -o tring_bench tring_bench.c
 * requires: geo_tring_addr.h, geo_goldberg_tile.h in same dir
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "geo_tring_addr.h"
#include "geo_goldberg_tile.h"

/* ── Config ─────────────────────────────────────────────────────── */
#define IMG_W       1536
#define IMG_H       1536
#define TILE_SIZE     32
#define TILES_X     (IMG_W / TILE_SIZE)   /* 48 */
#define TILES_Y     (IMG_H / TILE_SIZE)   /* 48 */
#define TOTAL_TILES (TILES_X * TILES_Y)   /* 2304 */
#define N_CHANNELS    3

/* ── ThreshMap entry ─────────────────────────────────────────────── */
typedef struct {
    uint8_t  thr;        /* quantization threshold 0/2/4 */
    uint8_t  zone;       /* 0=hilbert_anchor, 1=hilbert_mid, 2=residual */
    uint16_t addr;       /* tring addr 0..6911 */
} TileEntry;

/* ── Tring zone classifier ───────────────────────────────────────── */
/* Zone based on geometric position in Tring space:
 *   anchor : pattern_id < 48  → center of compound, smooth region
 *   mid    : pattern_id 48-95 → transition band
 *   residual: pattern_id 96+  → edge/detail region, lossless
 */
static inline uint8_t classify_zone(TringAddr a) {
    if (a.pattern_id < 48)  return 0;  /* hilbert anchor */
    if (a.pattern_id < 96)  return 1;  /* hilbert mid    */
    return 2;                           /* residual/edge  */
}

/* ── Mode threshold table [zone][mode] ───────────────────────────── */
static const uint8_t THR_TABLE[3][2] = {
    /* zone 0: hilbert anchor — smooth, safe to quantize */
    {2, 4},
    /* zone 1: hilbert mid — moderate */
    {1, 2},
    /* zone 2: residual/edge — lossless */
    {0, 0},
};

/* ── Pipeline ────────────────────────────────────────────────────── */
static void run_pipeline(int mode, TileEntry *out_map) {
    /* out_map size = TOTAL_TILES, store worst-case zone per tile
     * ch=0,1 → invert=0 (hilbert), ch=2 → invert=1 (residual)
     * per-tile: take max zone across channels → conservative threshold */
    for (uint8_t ty = 0; ty < TILES_Y; ty++) {
        for (uint8_t tx = 0; tx < TILES_X; tx++) {
            int tid = ty * TILES_X + tx;
            uint8_t max_zone = 0;
            uint16_t base_addr = 0;

            for (uint8_t ch = 0; ch < N_CHANNELS; ch++) {
                TringAddr ta   = tring_encode(tx, ty, ch);
                uint8_t   zone = classify_zone(ta);
                if (zone > max_zone) { max_zone = zone; base_addr = ta.addr; }
            }

            out_map[tid].thr  = THR_TABLE[max_zone][mode];
            out_map[tid].zone = max_zone;
            out_map[tid].addr = base_addr;
        }
    }
}

/* ── Stats ───────────────────────────────────────────────────────── */
static void print_stats(const TileEntry *map, int mode, double ms) {
    int zone_cnt[3] = {0};
    int thr_cnt[5]  = {0};

    for (int i = 0; i < TOTAL_TILES; i++) {
        zone_cnt[map[i].zone]++;
        if (map[i].thr < 5) thr_cnt[map[i].thr]++;
    }

    printf("\n[Mode: %s]\n", mode == 0 ? "Balanced" : "Aggressive");
    printf("  Time      : %.4f ms\n", ms);
    printf("  Throughput: %.2f M tiles/sec\n", (TOTAL_TILES / 1000.0) / ms);
    printf("  Zones     : anchor=%d  mid=%d  edge=%d\n",
           zone_cnt[0], zone_cnt[1], zone_cnt[2]);
    printf("  Thresh    : thr0=%d  thr1=%d  thr2=%d  thr4=%d\n",
           thr_cnt[0], thr_cnt[1], thr_cnt[2], thr_cnt[4]);

    /* estimated compression hint */
    double lossless_ratio = (double)zone_cnt[2] / TOTAL_TILES;
    printf("  Lossless  : %.1f%% tiles (residual/edge zone)\n",
           lossless_ratio * 100.0);
}

/* ── Roundtrip verify ────────────────────────────────────────────── */
static void verify_bijection(void) {
    uint32_t errs = tring_roundtrip_verify();
    printf("Tring bijection verify: %s (%u errors)\n",
           errs == 0 ? "OK" : "FAIL", errs);
}

/* ── Main ────────────────────────────────────────────────────────── */
int main(void) {
    printf("=== Tring Pipeline Bench ===\n");
    printf("Image: %dx%d  Tile: %d  Grid: %dx%d  Total: %d\n\n",
           IMG_W, IMG_H, TILE_SIZE, TILES_X, TILES_Y, TOTAL_TILES);

    verify_bijection();

    TileEntry *map = malloc(TOTAL_TILES * sizeof(TileEntry));
    if (!map) { fprintf(stderr, "OOM\n"); return 1; }

    struct timespec t0, t1;
    double ms;

    for (int mode = 0; mode <= 1; mode++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        run_pipeline(mode, map);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (t1.tv_sec  - t0.tv_sec)  * 1000.0
           + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        print_stats(map, mode, ms);
    }

    free(map);
    return 0;
}

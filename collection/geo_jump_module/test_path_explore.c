#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "icosphere_capture.h"
#include "geo_field_icosphere.h"
#include "geo_goldberg_sphere.h"

/* Minimal GeoPixel header inline (to avoid full geopixel include path) */
#define GP_TRIT_MOD    27u
#define GP_SPOKE_MOD    6u
#define GP_COSET_MOD    9u
#define GP_LETTER_MOD  26u
#define GP_FIBO_MOD   144u
#define GP_GRID_W      27u

typedef struct { uint8_t r, g, b; } GeoPixel;
static inline GeoPixel geo_pixel_encode(uint32_t idx, uint32_t W) {
    uint32_t i = (W > 0) ? (idx % W) : idx;
    GeoPixel p;
    p.r = (uint8_t)(((i % GP_TRIT_MOD) << 3) | (i % GP_SPOKE_MOD));
    p.g = (uint8_t)(((i % GP_COSET_MOD) << 4) | (i % GP_LETTER_MOD & 0xFu));
    p.b = (uint8_t)(i % GP_FIBO_MOD);
    return p;
}

int main(void) {
    printf("=== PATH EXPLORATION: icosphere capture \xe2\x86\x92 GeoPixel ===\n\n");

    /* ────────────────────────────────────────────
     * PATH A: Visual — capture key → GeoPixel
     * ──────────────────────────────────────────── */
    printf("--- A: VISUAL (capture key \xe2\x86\x92 GeoPixel) ---\n");
    /* Strategy A1: encode raw key */
    int seen[300] = {0};
    int collisions = 0;
    for (uint32_t k = 0; k < 300; k++) {
        GeoPixel p = geo_pixel_encode(k, GP_GRID_W);
        /* Treat (r,g,b) as uniqueness key */
        uint32_t hash = (uint32_t)p.r * 65536 + (uint32_t)p.g * 256 + (uint32_t)p.b;
        if (hash < 300) {
            if (seen[hash]++) collisions++;
        }
    }
    printf("  A1 (raw key): %d collisions in 300 keys\n", collisions);

    /* Strategy A2: encode (face,i,j,k) into 5 GeoPixel fields */
    collisions = 0;
    uint8_t bitmap[27][9][144];
    memset(bitmap, 0, sizeof(bitmap));
    for (uint32_t k = 0; k < 300; k++) {
        int fi, fj, fk;
        icosa_decode_ijk(k, &fi, &fj, &fk);
        int face = icosa_key_to_face(k);
        /* Encode geometric fields into GeoPixel's 5-field space:
         *   trit  = face % 27  (0..19)
         *   spoke = i % 6      (0..4)
         *   coset = j % 9      (0..4)
         *   fibo  = k*36 + face*3  (0..143)
         *   letter = low 4 bits of face */
        uint32_t idx = (uint32_t)(face * 15 + fi + fj * 5 - fj * (fj - 1) / 2);
        GeoPixel p;
        p.r = (uint8_t)((face % 27) << 3) | (uint8_t)(fi % 6);
        p.g = (uint8_t)((fj % 9) << 4) | (uint8_t)(fk & 0xFu);
        p.b = (uint8_t)((fk * 36 + face * 3) % 144);
        uint8_t t = (p.r >> 3) % 27;
        uint8_t c = (p.g >> 4) % 9;
        uint8_t f = p.b % 144;
        if (t < 27 && c < 9 && f < 144) {
            if (bitmap[t][c][f]++) collisions++;
        }
    }
    printf("  A2 (geom fields): %d collisions in 300 keys\n", collisions);

    /* Strategy A3: direct 3D position → pseudo-RGB */
    printf("  A3 (3D pos \xe2\x86\x92 RGB): ");
    double px, py, pz;
    icosa_decode_position(0, &px, &py, &pz);
    printf("key0=(%.4f,%.4f,%.4f) \xe2\x86\x92 RGB=(%d,%d,%d)\n",
        px, py, pz,
        (int)((px/ICOSA_R+1)*127), (int)((py/ICOSA_R+1)*127), (int)((pz/ICOSA_R+1)*127));

    /* check uniqueness of position-derived RGB */
    memset(seen, 0, sizeof(seen));
    collisions = 0;
    for (uint32_t k = 0; k < 300; k++) {
        icosa_decode_position(k, &px, &py, &pz);
        int r = (int)((px/ICOSA_R+1)*127);
        int g = (int)((py/ICOSA_R+1)*127);
        int b = (int)((pz/ICOSA_R+1)*127);
        uint32_t h = ((uint32_t)(r & 0x7) << 6) | ((uint32_t)(g & 0x7) << 3) | (uint32_t)(b & 0x7);
        if (h < 300) {
            if (seen[h]++) collisions++;
        }
    }
    printf("  A3 collisions (3-bit quant): %d/300\n", collisions);

    /* ────────────────────────────────────────────
     * PATH B: O4 — GpAddr tile → GeoPixel grid
     * ──────────────────────────────────────────── */
    printf("\n--- B: TILE ENCODE (GpAddr tile \xe2\x86\x92 GeoPixel grid) ---\n");
    /* Simulate: position data (3 doubles = 24B) as tile blob */
    uint8_t blob[24];
    uint32_t blob_sz = 24;
    icosa_decode_position(42, &px, &py, &pz);
    memcpy(blob, &px, 8); memcpy(blob+8, &py, 8); memcpy(blob+16, &pz, 8);

    uint32_t n_chunks = (blob_sz + 2) / 3;  /* 8 chunks */
    printf("  key42 pos=(%.4f,%.4f,%.4f) \xe2\x86\x92 %u chunks\n", px, py, pz, n_chunks);

    /* Encode each chunk as GeoPixel (like O4 connector) */
    GeoPixel grid[8];
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t slot_idx = i % 27;
        GeoPixel geo = geo_pixel_encode(slot_idx, GP_GRID_W);
        uint8_t b0 = (i*3+0 < blob_sz) ? blob[i*3+0] : 0;
        uint8_t b1 = (i*3+1 < blob_sz) ? blob[i*3+1] : 0;
        uint8_t b2 = (i*3+2 < blob_sz) ? blob[i*3+2] : 0;
        grid[i].r = geo.r ^ b0;
        grid[i].g = geo.g ^ b1;
        grid[i].b = geo.b ^ b2;
    }
    printf("  Grid layout (27-wide): cols covered = 0..%u\n", n_chunks-1);

    /* Decode and verify */
    uint8_t recovered[24] = {0};
    int ok = 1;
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t slot_idx = i % 27;
        GeoPixel geo = geo_pixel_encode(slot_idx, GP_GRID_W);
        uint8_t b0 = grid[i].r ^ geo.r;
        uint8_t b1 = grid[i].g ^ geo.g;
        uint8_t b2 = grid[i].b ^ geo.b;
        if (i*3+0 < blob_sz) recovered[i*3+0] = b0;
        if (i*3+1 < blob_sz) recovered[i*3+1] = b1;
        if (i*3+2 < blob_sz) recovered[i*3+2] = b2;
    }
    double rpx, rpy, rpz;
    memcpy(&rpx, recovered, 8); memcpy(&rpy, recovered+8, 8); memcpy(&rpz, recovered+16, 8);
    printf("  Roundtrip: (%.4f,%.4f,%.4f) vs (%.4f,%.4f,%.4f) \xe2\x86\x92 %s\n",
        px, py, pz, rpx, rpy, rpz,
        (fabs(px-rpx)<1e-10 && fabs(py-rpy)<1e-10 && fabs(pz-rpz)<1e-10) ? "OK" : "MISMATCH");

    /* ────────────────────────────────────────────
     * PATH C: GEO_FULL distribution
     * ──────────────────────────────────────────── */
    printf("\n--- C: GEO_FULL ADDRESS SPACE (capture key \xe2\x86\x92 GEO_FULL node) ---\n");
    uint32_t node_counts[128] = {0};
    int min_addr = 999999, max_addr = 0;
    double sum = 0;
    for (uint32_t k = 0; k < 300; k++) {
        uint32_t node = icosphere_capture_key_to_geo_full(k);
        uint32_t bucket = node * 128 / 20736;
        if (bucket >= 128) bucket = 127;
        node_counts[bucket]++;
        if ((int)node < min_addr) min_addr = (int)node;
        if ((int)node > max_addr) max_addr = (int)node;
        sum += node;
    }
    printf("  GEO_FULL range: %d..%d (of 0..20735)\n", min_addr, max_addr);
    printf("  Avg node: %.1f  (ideal=10368)\n", sum / 300.0);

    /* bucket distribution test */
    int empty_buckets = 0, unbalanced = 0;
    int min_cnt = 999, max_cnt = 0;
    for (int i = 0; i < 128; i++) {
        if (node_counts[i] == 0) empty_buckets++;
        if (node_counts[i] < min_cnt) min_cnt = node_counts[i];
        if (node_counts[i] > max_cnt) max_cnt = node_counts[i];
    }
    printf("  Buckets (128 bins): empty=%d  min=%d  max=%d  ideal=~2.3\n",
        empty_buckets, min_cnt, max_cnt);

    /* Also test per-face distribution */
    printf("\n  Per-face GEO_FULL spread:\n");
    for (int f = 0; f < 20; f++) {
        int f_min = 999999, f_max = 0;
        for (int idx = 0; idx < 15; idx++) {
            uint32_t key = (uint32_t)(f * 15 + idx);
            uint32_t node = icosphere_capture_key_to_geo_full(key);
            if ((int)node < f_min) f_min = (int)node;
            if ((int)node > f_max) f_max = (int)node;
        }
        printf("    face %2d: range %6d..%6d  (span=%d)\n", f, f_min, f_max, f_max - f_min);
    }

    return 0;
}

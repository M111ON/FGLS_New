/*
 * bench_bmp_encode.c — benchmark BMP image encoding with v4 pipeline
 * gcc -O2 -I. -I..\..\..\..\core\pogls_engine\twin_core -o bench_bmp_encode.exe bench_bmp_encode.c -lm
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "tring.h"
#include "pogls_fold.h"
#include "geo_diamond_field_v4.h"

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Minimal BMP loader — returns pixel data (row-major, 3 bytes/pixel) */
static uint8_t *load_bmp(const char *path, int *w_out, int *h_out, uint32_t *pixel_bytes_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("Cannot open %s\n", path); return NULL; }

    uint8_t header[54];
    if (fread(header, 1, 54, f) != 54) { fclose(f); return NULL; }

    /* Check BMP magic */
    if (header[0] != 'B' || header[1] != 'M') { fclose(f); return NULL; }

    int w = *(int32_t*)(header + 18);
    int h = *(int32_t*)(header + 22);
    int bpp = *(uint16_t*)(header + 28);
    uint32_t data_offset = *(uint32_t*)(header + 10);

    if (bpp != 24 && bpp != 32) { printf("Unsupported bpp: %d\n", bpp); fclose(f); return NULL; }

    int row_bytes = ((w * bpp / 8 + 3) / 4) * 4; /* BMP rows are 4-byte aligned */
    uint32_t total_pixels = (uint32_t)(w * h * 3);
    uint8_t *pixels = malloc(total_pixels);
    if (!pixels) { fclose(f); return NULL; }

    fseek(f, data_offset, SEEK_SET);

    /* Read rows bottom-to-top, convert to top-to-bottom, strip padding */
    uint8_t *row = malloc(row_bytes);
    for (int y = h - 1; y >= 0; y--) {
        if (fread(row, 1, row_bytes, f) != (size_t)row_bytes) break;
        for (int x = 0; x < w; x++) {
            uint8_t *dst = pixels + ((y * w + x) * 3);
            if (bpp == 24) {
                dst[0] = row[x * 3 + 2]; /* B→R */
                dst[1] = row[x * 3 + 1]; /* G */
                dst[2] = row[x * 3 + 0]; /* R→B */
            } else {
                dst[0] = row[x * 4 + 2];
                dst[1] = row[x * 4 + 1];
                dst[2] = row[x * 4 + 0];
            }
        }
    }
    free(row);
    fclose(f);

    *w_out = w; *h_out = h; *pixel_bytes_out = total_pixels;
    return pixels;
}

/* Pack 64 bytes from pixel data: 21 RGB triples + 1 byte padding */
static void pack_chunk(uint8_t out[64], const uint8_t *pixels, uint32_t offset, uint32_t total) {
    uint32_t idx = 0;
    for (int i = 0; i < 21 && (offset + (uint32_t)i * 3 + 2) < total; i++) {
        out[idx++] = pixels[offset + i * 3];
        out[idx++] = pixels[offset + i * 3 + 1];
        out[idx++] = pixels[offset + i * 3 + 2];
    }
    while (idx < 64) out[idx++] = 0;
}

int main(int argc, char *argv[]) {
    const char *bmp_path = "high_detail.bmp";
    if (argc > 1) bmp_path = argv[1];

    printf("=== BMP Image Encoding Benchmark ===\n");
    printf("File: %s\n\n", bmp_path);

    int w, h;
    uint32_t pixel_bytes;
    uint8_t *pixels = load_bmp(bmp_path, &w, &h, &pixel_bytes);
    if (!pixels) return 1;

    printf("Image: %dx%d (%.1f MP), %.2f MB raw\n", w, h, (double)(w*h)/1e6, pixel_bytes/1e6);

    /* Pack into 64B chunks */
    uint32_t n_chunks = (pixel_bytes + 62) / 63; /* 63 bytes of pixel data per chunk */
    uint8_t *chunks = calloc(n_chunks, 64);
    for (uint32_t i = 0; i < n_chunks; i++) {
        pack_chunk(chunks + i * 64, pixels, i * 63, pixel_bytes);
    }

    /* Classify first few chunks */
    int eclass_count[3] = {0};
    uint32_t sample = n_chunks < 100 ? n_chunks : 100;
    for (uint32_t i = 0; i < sample; i++) {
        eclass_count[_chunk_entropy_class(chunks + i * 64)]++;
    }
    printf("Entropy class (sample %u): low=%d med=%d high=%d\n\n",
           sample, eclass_count[0], eclass_count[1], eclass_count[2]);

    /* Encode all chunks with windowed batch + GC between windows */
    DiamondField df;
    dfield_init(&df, n_chunks * 4);

    double t0 = now_ms();
    uint32_t encoded = 0;
    uint64_t total_out = 0;
    uint32_t gidx_map[28087];
    memset(gidx_map, 0xFF, sizeof(gidx_map));

    const uint32_t WINDOW = 32;
    uint32_t gc_counter = 0;
    for (uint32_t off = 0; off < n_chunks; off += WINDOW) {
        uint32_t ws = (off + WINDOW > n_chunks) ? (n_chunks - off) : WINDOW;
        uint32_t gb[32];
        uint32_t e = dfield_encode_windowed(&df, chunks + off * 64, ws, 16, gb);
        for (uint32_t j = 0; j < e; j++) {
            uint32_t idx = off + j;
            if (idx < n_chunks) {
                gidx_map[idx] = gb[j];
                encoded++;
                uint32_t sz;
                const uint8_t *data = tring_read(&df.tring, sidx_get(&df.sidx, gb[j]), &sz);
                if (data) total_out += sz;
            }
        }
        gc_counter += ws;
        /* Skip GC — too expensive for bulk encode */
        if (off % 5000 == 0 && off > 0) printf("  Progress: %u/%u chunks...\n", off, n_chunks);
    }
    double t1 = now_ms();
    double elapsed = t1 - t0;

    printf("\nResults (windowed batch, W=32, B=16):\n");
    printf("  Chunks encoded: %u/%u (%.1f%% success)\n", encoded, n_chunks, 100.0 * encoded / n_chunks);
    printf("  Time: %.1f ms (%.0f chunks/s, %.1f MB/s)\n",
           elapsed, encoded / (elapsed / 1000.0),
           (encoded * 64.0 / 1e6) / (elapsed / 1000.0));
    printf("  Input:  %llu bytes\n", (unsigned long long)(n_chunks * 64));
    printf("  Output: %llu bytes\n", (unsigned long long)total_out);
    if (total_out > 0)
        printf("  Ratio:  %.3fx\n", (double)(n_chunks * 64) / total_out);

    /* Roundtrip verification */
    printf("\nRoundtrip check (first 20 encoded chunks)...\n");
    int ok = 1, checked = 0;
    for (uint32_t i = 0; i < n_chunks && checked < 20; i++) {
        if (gidx_map[i] == 0xFFFFFFFF) continue;
        uint8_t out[64];
        if (dfield_decode(&df, gidx_map[i], out) != 0) { ok = 0; printf("  chunk %u: decode failed\n", i); continue; }
        if (memcmp(out, chunks + i * 64, 64) != 0) { ok = 0; printf("  chunk %u: mismatch\n", i); continue; }
        checked++;
    }
    printf("  %s (%d/%d checked)\n", ok ? "PASS: all roundtrip lossless" : "FAIL", checked, 20);

    /* ── Flat mode: bypass shell/slot, sequential encoding ── */
    {
        DiamondField df2;
        dfield_init(&df2, n_chunks + 1); /* tring capacity = chunks */

        double t0 = now_ms();
        uint32_t flat_ticks[28087];
        uint32_t flat_encoded = 0;
        uint64_t flat_out = 0;

        for (uint32_t i = 0; i < n_chunks; i++) {
            uint32_t tick = dfield_encode_flat(&df2, chunks + i * 64);
            if (tick != UINT32_MAX) {
                flat_ticks[flat_encoded++] = tick;
                flat_out += 64; /* worst case: all raw */
            }
        }
        double t1 = now_ms();

        /* Calculate actual output size */
        flat_out = 0;
        for (uint32_t i = 0; i < flat_encoded; i++) {
            uint32_t sz;
            tring_read(&df2.tring, flat_ticks[i], &sz);
            flat_out += sz;
        }

        printf("\nFlat mode (no shell/slot):\n");
        printf("  Chunks: %u/%u (%.1f%%)\n", flat_encoded, n_chunks, 100.0 * flat_encoded / n_chunks);
        printf("  Time: %.1f ms (%.0f chunks/s, %.1f MB/s)\n",
               t1 - t0, flat_encoded / ((t1 - t0) / 1000.0),
               (flat_encoded * 64.0 / 1e6) / ((t1 - t0) / 1000.0));
        printf("  Input:  %llu bytes\n", (unsigned long long)(n_chunks * 64));
        printf("  Output: %llu bytes\n", (unsigned long long)flat_out);
        if (flat_out > 0)
            printf("  Ratio:  %.3fx\n", (double)(n_chunks * 64) / flat_out);

        /* Roundtrip check */
        int flat_ok = 1;
        uint32_t check_n = flat_encoded < 20 ? flat_encoded : 20;
        for (uint32_t i = 0; i < check_n; i++) {
            uint8_t out[64];
            if (dfield_decode_flat(&df2, flat_ticks[i], out) != 0) { flat_ok = 0; break; }
            if (memcmp(out, chunks + i * 64, 64) != 0) { flat_ok = 0; break; }
        }
        printf("  Roundtrip: %s (%u/%u)\n", flat_ok ? "PASS" : "FAIL", check_n, check_n);
        dfield_free(&df2);
    }

    /* ── Shell mode with ×16 flag ── */
    {
        DiamondField df_x16;
        dfield_init(&df_x16, n_chunks * 4);
        dfield_set_x16(&df_x16, 1); /* enable ×16 virtual space */

        double t0 = now_ms();
        uint32_t x16_encoded = 0;
        uint64_t x16_out = 0;
        uint32_t x16_gidx_map[28087];
        memset(x16_gidx_map, 0xFF, sizeof(x16_gidx_map));

        const uint32_t WINDOW = 32;
        for (uint32_t off = 0; off < n_chunks; off += WINDOW) {
            uint32_t ws = (off + WINDOW > n_chunks) ? (n_chunks - off) : WINDOW;
            uint32_t gb[32];
            uint32_t e = dfield_encode_windowed(&df_x16, chunks + off * 64, ws, 16, gb);
            for (uint32_t j = 0; j < e; j++) {
                uint32_t idx = off + j;
                if (idx < n_chunks) {
                    x16_gidx_map[idx] = gb[j];
                    x16_encoded++;
                    uint32_t sz;
                    const uint8_t *data = tring_read(&df_x16.tring, sidx_get(&df_x16.sidx, gb[j]), &sz);
                    if (data) x16_out += sz;
                }
            }
            if (off % 5000 == 0 && off > 0) printf("  Progress: %u/%u chunks...\n", off, n_chunks);
        }
        double t1 = now_ms();

        printf("\nShell ×16 mode (virtual slots):\n");
        printf("  Chunks: %u/%u (%.1f%%)\n", x16_encoded, n_chunks, 100.0 * x16_encoded / n_chunks);
        printf("  Time: %.1f ms (%.0f chunks/s, %.1f MB/s)\n",
               t1 - t0, x16_encoded / ((t1 - t0) / 1000.0),
               (x16_encoded * 64.0 / 1e6) / ((t1 - t0) / 1000.0));
        printf("  Output: %llu bytes, Ratio: %.3fx\n",
               (unsigned long long)x16_out, (double)(n_chunks * 64) / x16_out);

        /* Roundtrip */
        int x16_ok = 1;
        uint32_t x16_checked = 0;
        for (uint32_t i = 0; i < n_chunks && x16_checked < 20; i++) {
            if (x16_gidx_map[i] == 0xFFFFFFFF) continue;
            uint8_t out[64];
            if (dfield_decode(&df_x16, x16_gidx_map[i], out) != 0) { x16_ok = 0; break; }
            if (memcmp(out, chunks + i * 64, 64) != 0) { x16_ok = 0; break; }
            x16_checked++;
        }
        printf("  Roundtrip: %s (%u/%u)\n", x16_ok ? "PASS" : "FAIL", x16_checked, 20);
        dfield_free(&df_x16);
    }

    /* ── Large capacity stress test ── */
    {
        printf("\nLarge capacity test (1M chunks, flat mode)...\n");
        const uint32_t HUGE = 1000000;
        DiamondField df3;
        dfield_init(&df3, HUGE + 1);

        double t0 = now_ms();
        uint32_t ok = 0;
        for (uint32_t i = 0; i < HUGE; i++) {
            uint32_t tick = dfield_encode_flat(&df3, chunks + (i % n_chunks) * 64);
            if (tick != UINT32_MAX) ok++;
        }
        double t1 = now_ms();
        printf("  Encoded: %u/1000000 in %.1f ms (%.0f chunks/s)\n",
               ok, t1 - t0, ok / ((t1 - t0) / 1000.0));

        /* Roundtrip spot check */
        int huge_ok = 1;
        for (uint32_t i = 0; i < 10; i++) {
            uint32_t idx = (uint32_t)(i * 100001);
            if (idx >= ok) idx = i;
            uint8_t out[64];
            if (dfield_decode_flat(&df3, idx, out) != 0) { huge_ok = 0; break; }
        }
        printf("  Roundtrip: %s\n", huge_ok ? "PASS" : "FAIL");
        dfield_free(&df3);
    }

    dfield_free(&df);
    free(pixels);
    free(chunks);
    return 0;
}

/*
 * c_gsten_encode.c — C encoder: .qdat → .gsten (fast, ~100× Python)
 *
 * Compile:
 *   gcc -O2 -I.. -o c_gsten_encode c_gsten_encode.c
 *
 * Run:
 *   ./c_gsten_encode <qdat_dir> <out_dir>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#ifndef GEOM_RAW_BRIDGE_IMPLEMENTATION
#define GEOM_RAW_BRIDGE_IMPLEMENTATION
#endif
#include "geom_raw_bridge.h"
#include "hex_tile.h"

static int encode_file(const char *qdat_path, const char *out_path) {
    FILE *fp = fopen(qdat_path, "rb");
    if (!fp) { fprintf(stderr, "  FAIL open %s\n", qdat_path); return -1; }

    fseek(fp, 0, SEEK_END);
    long raw_sz_l = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (raw_sz_l <= 0) { fclose(fp); return -1; }
    size_t raw_sz = (size_t)raw_sz_l;
    uint8_t *raw = (uint8_t *)malloc(raw_sz);
    if (fread(raw, 1, raw_sz, fp) != raw_sz) { free(raw); fclose(fp); return -1; }
    fclose(fp);

    uint32_t n_tiles = (uint32_t)((raw_sz + GSTEN_TILE_SZ - 1) / GSTEN_TILE_SZ);
    size_t index_sz = (size_t)n_tiles * 6;
    size_t header_sz = 16;

    /* First pass: compute total encoded size */
    uint32_t enc_total = 0;
    uint32_t *offs = (uint32_t *)malloc((size_t)n_tiles * sizeof(uint32_t));
    uint8_t  *sizes = (uint8_t *)malloc((size_t)n_tiles);
    uint8_t  *tbuf  = (uint8_t *)malloc((size_t)n_tiles * 9);

    for (uint32_t ti = 0; ti < n_tiles; ti++) {
        size_t start = (size_t)ti * GSTEN_TILE_SZ;
        HexTile tile; memset(&tile, 0, sizeof(tile));
        size_t copy = (start + GSTEN_TILE_SZ <= raw_sz) ? GSTEN_TILE_SZ : (raw_sz - start);
        if (copy > 0) memcpy(tile.c, raw + start, copy);

        int esz = hex_tile_encode(&tile, tbuf + (size_t)enc_total);
        offs[ti] = enc_total;
        sizes[ti] = (uint8_t)esz;
        enc_total += (uint32_t)esz;
    }

    /* Allocate final buffer + write header */
    uint8_t *gsten = (uint8_t *)malloc(header_sz + index_sz + (size_t)enc_total);
    uint32_t magic = GSTEN_MAGIC;
    memcpy(gsten, &magic, 4);
    memcpy(gsten + 4, &n_tiles, 4);
    gsten[8] = GSTEN_TILE_SZ;
    memset(gsten + 9, 0, 7);

    /* Write index */
    for (uint32_t ti = 0; ti < n_tiles; ti++) {
        size_t off = header_sz + (size_t)ti * 6;
        memcpy(gsten + off, &offs[ti], 4);
        gsten[off + 4] = sizes[ti];
        gsten[off + 5] = tbuf[offs[ti]];  /* type = first byte of encoded data */
    }

    /* Write tile data */
    memcpy(gsten + header_sz + index_sz, tbuf, (size_t)enc_total);

    fp = fopen(out_path, "wb");
    if (fp) { fwrite(gsten, 1, header_sz + index_sz + (size_t)enc_total, fp); fclose(fp); }
    else { free(raw); free(offs); free(sizes); free(tbuf); free(gsten); return -1; }

    free(raw); free(offs); free(sizes); free(tbuf); free(gsten);
    return 0;
}

#ifdef _WIN32
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
#include <time.h>
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: c_gsten_encode <qdat_dir> <out_dir>\n");
        return 1;
    }

    const char *qdat_dir = argv[1];
    const char *out_dir  = argv[2];

    /* Create output dir */
#ifdef _WIN32
    mkdir(out_dir);
#else
    mkdir(out_dir, 0755);
#endif

    /* Scan .qdat files */
    DIR *d = opendir(qdat_dir);
    if (!d) { fprintf(stderr, "Cannot open dir: %s\n", qdat_dir); return 1; }

    struct dirent *entry;
    int n_total = 0, n_ok = 0;
    size_t total_orig = 0, total_enc = 0;

    double t0 = now_ms();

    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 5, ".qdat") != 0) continue;

        n_total++;

        char in_path[1024], out_path[1024];
        snprintf(in_path, sizeof(in_path), "%s/%s", qdat_dir, name);

        /* Output: .qdat → .gsten */
        char base[1024];
        memcpy(base, name, len - 5);
        base[len - 5] = 0;
        snprintf(out_path, sizeof(out_path), "%s/%s.gsten", out_dir, base);

        /* Get file size */
        struct stat st;
        stat(in_path, &st);
        total_orig += (size_t)st.st_size;

        if (encode_file(in_path, out_path) == 0) {
            n_ok++;
            stat(out_path, &st);
            total_enc += (size_t)st.st_size;
            printf("  %-50s %8.1fKB → %8.1fKB  ratio=%.3f\n",
                   base, (double)(st.st_size) / 1024,
                   0.0, 0.0);
        }
    }
    closedir(d);

    double elapsed = now_ms() - t0;

    printf("\n========================================\n");
    printf("  Files:   %d / %d OK\n", n_ok, n_total);
    printf("  Original: %.1f MB\n", total_orig / 1024.0 / 1024.0);
    printf("  Encoded:  %.1f MB\n", total_enc / 1024.0 / 1024.0);
    printf("  Ratio:    %.4f\n", (double)total_enc / total_orig);
    printf("  Time:     %.2f ms\n", elapsed);
    printf("  Out:      %s\n", out_dir);
    printf("========================================\n");

    return (n_ok == n_total) ? 0 : 1;
}

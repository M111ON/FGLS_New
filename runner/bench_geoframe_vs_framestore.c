/*
 * bench_geoframe_vs_framestore.c — head-to-head seek benchmark
 * GeoFrame (bitmap O(1) popcount) vs FrameStore (sorted bsearch O(log n))
 *
 * Build: gcc -O2 -std=c11 -I. -o bench_gf_vs_fs.exe bench_geoframe_vs_framestore.c
 * Run:   bench_gf_vs_fs.exe MODEL.geo [--iters N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pogls_v3_geoframe.h"
#include "pogls_v3_framestore.h"

#ifdef _WIN32
#include <windows.h>
static double now_sec(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
#else
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

#define N_RAND_SEEKS 1000000

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s MODEL.geo [--iters N]\n", argv[0]);
        return 1;
    }
    const char *geof_path = argv[1];
    int n_iters = N_RAND_SEEKS;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
            n_iters = atoi(argv[++i]);
    }

    /* ---- Load GeoFrame file ---- */
    FILE *f = fopen(geof_path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", geof_path); return 1; }

    GeoFHeader hdr;
    if (fread(&hdr, GEOF_HEADER_SZ, 1, f) != 1 || hdr.magic != GEOF_MAGIC) {
        fprintf(stderr, "Bad GeoFrame header\n"); fclose(f); return 1;
    }

    uint8_t *bmap = (uint8_t *)malloc(GEOF_BITMAP_SZ);
    if (fread(bmap, GEOF_BITMAP_SZ, 1, f) != 1) {
        fprintf(stderr, "Cannot read bitmap\n"); free(bmap); fclose(f); return 1;
    }

    /* read offset table (8 bytes per occupied entry) */
    uint64_t *offsets = (uint64_t *)malloc((size_t)hdr.n_occupied * 8);
    if (fread(offsets, 8, hdr.n_occupied, f) != hdr.n_occupied) {
        fprintf(stderr, "Cannot read offsets\n"); free(offsets); free(bmap); fclose(f); return 1;
    }

    /* read frame headers to get addrs and sizes */
    uint32_t *frame_addrs = (uint32_t *)malloc(hdr.n_occupied * sizeof(uint32_t));
    uint32_t *frame_sizes = (uint32_t *)malloc(hdr.n_occupied * sizeof(uint32_t));
    for (uint32_t i = 0; i < hdr.n_occupied; i++) {
        GeoFFrame fr;
        if (fread(&fr, sizeof(GeoFFrame), 1, f) != 1) {
            fprintf(stderr, "Cannot read frame %u\n", i);
            free(frame_sizes); free(frame_addrs); free(offsets); free(bmap); fclose(f); return 1;
        }
        frame_addrs[i] = fr.addr;
        frame_sizes[i] = fr.size;
        /* skip payload */
        if (fr.size > 0) fseek(f, (long)fr.size, SEEK_CUR);
    }
    fclose(f);

    printf("[geoframe] %s: %u occupied, %u slots\n", geof_path, hdr.n_occupied, GEOF_N_SLOTS);

    /* find max addr */
    uint32_t max_addr = 0;
    for (uint32_t i = 0; i < hdr.n_occupied; i++) {
        if (frame_addrs[i] > max_addr) max_addr = frame_addrs[i];
    }

    /* ---- Build FrameStore entries from same data ---- */
    FrameStoreEntry *fs_entries = (FrameStoreEntry *)malloc(hdr.n_occupied * sizeof(FrameStoreEntry));
    for (uint32_t i = 0; i < hdr.n_occupied; i++) {
        fs_entries[i].addr = frame_addrs[i];
        fs_entries[i].nbytes = frame_sizes[i];
        fs_entries[i].file_offset = 0;
    }
    qsort(fs_entries, hdr.n_occupied, sizeof(FrameStoreEntry), framestore_cmp_addr);
    printf("[framestore] %u entries sorted\n", hdr.n_occupied);

    /* ---- Generate random addresses ---- */
    srand((unsigned)time(NULL));
    uint32_t *rand_addrs = (uint32_t *)malloc(n_iters * sizeof(uint32_t));
    for (int i = 0; i < n_iters; i++)
        rand_addrs[i] = (uint32_t)((unsigned)rand() % GEOF_N_SLOTS);

    /* ---- Benchmark: GeoFrame (bitmap popcount) ---- */
    double t0 = now_sec();
    volatile uint32_t geof_hit = 0;
    for (int i = 0; i < n_iters; i++) {
        uint32_t a = rand_addrs[i];
        if (geof_bitmap_get(bmap, a)) {
            uint32_t idx = geof_bitmap_popcount(bmap, a);
            geof_hit += idx;
        }
    }
    double geof_elapsed = now_sec() - t0;

    /* ---- Benchmark: FrameStore (bsearch) ---- */
    t0 = now_sec();
    volatile uint32_t fs_hit = 0;
    for (int i = 0; i < n_iters; i++) {
        const FrameStoreEntry *e = framestore_seek(fs_entries, hdr.n_occupied, rand_addrs[i]);
        if (e) fs_hit += e->nbytes;
    }
    double fs_elapsed = now_sec() - t0;

    double geof_ns = (geof_elapsed / n_iters) * 1e9;
    double fs_ns = (fs_elapsed / n_iters) * 1e9;

    printf("\n=== Benchmark: %d random seeks ===\n", n_iters);
    printf("  GeoFrame   (bitmap O(1)):     %7.2f ns/op  (hits: %u)\n", geof_ns, geof_hit);
    printf("  FrameStore (bsearch O(log n)): %7.2f ns/op  (hits: %u)\n", fs_ns, fs_hit);
    if (fs_ns > geof_ns)
        printf("  Speedup: %.2fx (GeoFrame wins)\n", fs_ns / geof_ns);
    else
        printf("  Speedup: %.2fx (FrameStore wins)\n", geof_ns / fs_ns);

    free(frame_sizes);
    free(frame_addrs);
    free(offsets);
    free(bmap);
    free(fs_entries);
    free(rand_addrs);
    return 0;
}

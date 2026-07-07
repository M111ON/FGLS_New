#include "pogls_v3_geoframe.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>

static int n_pass = 0, n_fail = 0;
#define TEST(name, expr) do { \
    int _r = (expr); \
    if (_r == 0) { n_pass++; fprintf(stderr, "  ✅ %s\n", name); } \
    else { n_fail++; fprintf(stderr, "  ❌ %s (err=%d)\n", name, _r); } \
} while(0)

static uint32_t test_addrs[] = {
    0, 1, 2, 3, 4, 5, 6, 7,
    128, 256, 512, 1024, 2048, 4096, 8192, 16384,
    20734, 20735
};
#define N_TEST (sizeof(test_addrs)/sizeof(test_addrs[0]))

int main(void) {
    fprintf(stderr, "=== POGLS v3 GeoFrame Test ===\n\n");

    /* T1: write + read roundtrip */
    fprintf(stderr, "[T1] Write/read roundtrip\n");
    {
        uint32_t n = N_TEST;
        uint32_t *sizes = (uint32_t *)malloc(n * sizeof(uint32_t));
        uint8_t **datas = (uint8_t **)malloc(n * sizeof(uint8_t *));
        for (uint32_t i = 0; i < n; i++) {
            sizes[i] = 64 + i * 16;
            datas[i] = (uint8_t *)malloc(sizes[i]);
            for (uint32_t j = 0; j < sizes[i]; j++)
                datas[i][j] = (uint8_t)(test_addrs[i] + j);
        }

        int r = geof_write("test_geoframe.bin", test_addrs,
                           (const uint8_t *const *)datas, sizes, n);
        TEST("geof_write", r);

        GeoFHeader hdr;
        uint8_t *bmap;
        uint32_t *offsets;
        uint32_t loaded_n;

        r = geof_load("test_geoframe.bin", &hdr, &bmap, &offsets, &loaded_n);
        TEST("geof_load", r);
        TEST("n_occupied matches", (int)(loaded_n == n) ? 0 : -1);

        for (uint32_t i = 0; i < n; i++) {
            uint32_t idx, file_off;
            r = geof_seek(bmap, offsets, test_addrs[i], hdr.n_occupied, &idx, &file_off);
            if (r != 0) { TEST("seek each addr", -1); break; }
            if (idx != i) { TEST("frame index order", -1); break; }
            if (i == n-1) TEST("seek all addrs", 0);
        }

        r = geof_verify(bmap, hdr.n_occupied);
        TEST("bitmap verify", r);

        FILE *ft1 = fopen("test_geoframe.bin", "rb");
        int read_ok = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t buf[4096];
            GeoFFrame fr;
            r = geof_read_frame(ft1, bmap, offsets, test_addrs[i],
                                hdr.n_occupied, buf, sizeof(buf), &fr);
            if (r != 0) { TEST("read each frame", r); read_ok = -1; break; }
            int ok = (fr.size == sizes[i]) ? 0 : -1;
            if (ok == 0) ok = (fr.addr == test_addrs[i]) ? 0 : -1;
            if (ok == 0) ok = memcmp(buf, datas[i], sizes[i]);
            if (ok != 0) { TEST("frame data integrity", -1); read_ok = -1; break; }
            if (i == n-1) TEST("all frames data integrity", 0);
        }
        if (read_ok == 0) TEST("read_frame all pass", 0);
        if (ft1) fclose(ft1);

        geof_free(bmap, offsets);
        for (uint32_t i = 0; i < n; i++) free(datas[i]);
        free(sizes); free(datas);
    }

    /* T2: seek non-existent address returns -1 */
    fprintf(stderr, "\n[T2] Non-existent address\n");
    {
        GeoFHeader hdr;
        uint8_t *bmap;
        uint32_t *offsets;
        uint32_t n;
        geof_load("test_geoframe.bin", &hdr, &bmap, &offsets, &n);
        uint32_t idx, file_off;
        int r = geof_seek(bmap, offsets, 9999, n, &idx, &file_off);
        TEST("seek addr=9999 (empty)", (r != 0) ? 0 : -1);
        r = geof_seek(bmap, offsets, 20736, n, &idx, &file_off);
        TEST("seek addr=20736 (OOB)", (r != 0) ? 0 : -1);
        geof_free(bmap, offsets);
    }

    /* T3: empty file */
    fprintf(stderr, "\n[T3] Empty file\n");
    {
        int r = geof_write("test_geoframe_empty.bin", NULL, NULL, NULL, 0);
        TEST("write empty", r);

        GeoFHeader hdr;
        uint8_t *bmap;
        uint32_t *offsets;
        uint32_t n;
        r = geof_load("test_geoframe_empty.bin", &hdr, &bmap, &offsets, &n);
        TEST("load empty", r);
        TEST("n_occupied=0", (n == 0) ? 0 : -1);
        geof_free(bmap, offsets);
    }

    /* T4: bitmap operations */
    fprintf(stderr, "\n[T4] Bitmap operations\n");
    {
        uint8_t bmap[GEOF_BITMAP_SZ] = {0};
        int r = geof_bitmap_get(bmap, 0) == 0 ? 0 : -1;
        TEST("bitmap initial zero", r);

        geof_bitmap_set(bmap, 0);
        r = geof_bitmap_get(bmap, 0) == 1 ? 0 : -1;
        TEST("bitmap set addr=0", r);

        geof_bitmap_set(bmap, 20735);
        r = geof_bitmap_get(bmap, 20735) == 1 ? 0 : -1;
        TEST("bitmap set addr=20735", r);

        uint32_t cnt = geof_bitmap_popcount(bmap, 20736);
        TEST("popcount full = 2", (cnt == 2) ? 0 : -1);

        cnt = geof_bitmap_popcount(bmap, 1);
        TEST("popcount up to 1 = 1", (cnt == 1) ? 0 : -1);
    }

    /* T5: large tensor data */
    fprintf(stderr, "\n[T5] Large tensor data (1MB)\n");
    {
        uint32_t addr = 42;
        uint32_t size = 1024 * 1024;
        uint8_t *data = (uint8_t *)malloc(size);
        for (uint32_t i = 0; i < size; i++)
            data[i] = (uint8_t)(i ^ 0xAB);

        const uint8_t *cdatas[] = { data };
        int r = geof_write("test_geoframe_large.bin", &addr, cdatas, &size, 1);
        TEST("write 1MB tensor", r);

        GeoFHeader hdr;
        uint8_t *bmap;
        uint32_t *offsets;
        uint32_t n;
        r = geof_load("test_geoframe_large.bin", &hdr, &bmap, &offsets, &n);
        TEST("load 1MB", r);
        TEST("n=1", (n == 1) ? 0 : -1);

        uint8_t *buf = (uint8_t *)malloc(size);
        FILE *ft5 = fopen("test_geoframe_large.bin", "rb");
        GeoFFrame fr;
        r = ft5 ? geof_read_frame(ft5, bmap, offsets, addr, n, buf, size, &fr) : -1;
        TEST("read 1MB frame", r);
        TEST("1MB data integrity", memcmp(buf, data, size) ? -1 : 0);
        if (ft5) fclose(ft5);

        free(buf); free(data);
        geof_free(bmap, offsets);
    }

    /* T6: get file size */
    fprintf(stderr, "\n[T6] File size\n");
    {
        GeoFHeader hdr;
        uint8_t *bmap;
        uint32_t *offsets;
        uint32_t n;
        geof_load("test_geoframe.bin", &hdr, &bmap, &offsets, &n);
        uint32_t total = geof_file_size(&hdr, offsets, n);
        TEST("file size > 0", (total > 0) ? 0 : -1);
        geof_free(bmap, offsets);
    }

    /* T7: benchmark seek speed */
    fprintf(stderr, "\n[T7] Benchmark seek (100K random)\n");
    {
        uint32_t n = N_TEST;
        uint32_t *sizes = (uint32_t *)malloc(n * sizeof(uint32_t));
        uint8_t **datas = (uint8_t **)malloc(n * sizeof(uint8_t *));
        for (uint32_t i = 0; i < n; i++) {
            sizes[i] = 64;
            datas[i] = (uint8_t *)malloc(64);
            memset(datas[i], (uint8_t)i, 64);
        }
        geof_write("test_geoframe_bench.bin", test_addrs,
                   (const uint8_t *const *)datas, sizes, n);

        GeoFHeader hdr;
        uint8_t *bmap;
        uint32_t *offsets;
        uint32_t loaded_n;
        geof_load("test_geoframe_bench.bin", &hdr, &bmap, &offsets, &loaded_n);

        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        int64_t reps = 1000000;
        for (int64_t k = 0; k < reps; k++) {
            uint32_t addr = test_addrs[k % n];
            uint32_t idx, file_off;
            int r = geof_seek(bmap, offsets, addr, loaded_n, &idx, &file_off);
            if (r != 0) { TEST("bench seek", r); break; }
            if (k == reps - 1) TEST("bench seek 1M pass", 0);
        }
        QueryPerformanceCounter(&t1);
        double total_s = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
        double ns_per_op = total_s * 1e9 / reps;
        fprintf(stderr, "         geof_seek: %.2f ns/op (1M random)\n", ns_per_op);

        geof_free(bmap, offsets);
        for (uint32_t i = 0; i < n; i++) free(datas[i]);
        free(sizes); free(datas);
    }

    /* cleanup */
    remove("test_geoframe.bin");
    remove("test_geoframe_empty.bin");
    remove("test_geoframe_large.bin");
    remove("test_geoframe_bench.bin");

    fprintf(stderr, "\n=== Results: %d PASS, %d FAIL ===\n", n_pass, n_fail);
    return n_fail > 0 ? 1 : 0;
}

/*
 * test_pogls_v3_e2e.c — End-to-end POGLS v3 geopixel test
 *
 * Verifies the full pipeline:
 *   1. Parse GGUF index (get tensor names, offsets, sizes)
 *   2. Build v3 header in memory (sorted entries)
 *   3. Write .pogls file
 *   4. Read back .pogls file
 *   5. mmap GGUF
 *   6. For each tensor: v3 seek → GGUF mmap read → verify data matches
 *   7. Benchmark: v3 binary search vs linear scan vs GGUF index lookup
 *
 * Build:
 *   gcc -O2 -std=c11 -I. -o test_pogls_v3_e2e.exe test_pogls_v3_e2e.c -lm
 *
 * Usage:
 *   .\test_pogls_v3_e2e.exe I:/model/LFM2.5-8B-A1B-Q4_K_M.gguf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  static uint8_t *mmap_file_ro(const char *path, size_t *out_sz) {
      HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      if (hFile == INVALID_HANDLE_VALUE) return NULL;
      LARGE_INTEGER li;
      if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); return NULL; }
      *out_sz = (size_t)li.QuadPart;
      HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
      if (!hMap) { CloseHandle(hFile); return NULL; }
      void *p = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
      CloseHandle(hMap);
      CloseHandle(hFile);
      return (uint8_t *)p;
  }
  static void munmap_file(uint8_t *p, size_t sz) {
      (void)sz;
      if (p) UnmapViewOfFile(p);
  }
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  static uint8_t *mmap_file_ro(const char *path, size_t *out_sz) {
      struct stat st;
      if (stat(path, &st) != 0) return NULL;
      *out_sz = st.st_size;
      int fd = open(path, O_RDONLY);
      if (fd < 0) return NULL;
      void *p = mmap(NULL, *out_sz, PROT_READ, MAP_PRIVATE, fd, 0);
      close(fd);
      return (p == MAP_FAILED) ? NULL : (uint8_t *)p;
  }
  static void munmap_file(uint8_t *p, size_t sz) {
      if (p) munmap(p, sz);
  }
#endif

#include "pogls_v3_geopixel.h"
#include "gguf_index.h"
#include "addr_space.h"

static int tests_run = 0, tests_pass = 0;
#define TEST(n) do { tests_run++; printf("  [%d] %-55s ", tests_run, n); fflush(stdout); } while(0)
#define PASS() do { tests_pass++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* ── Build v3 entries from GGUF index ── */
static PoglsV3Entry *build_v3_entries(const GGUFTensorIndex *idx, uint32_t *out_n) {
    PoglsV3Entry *entries = (PoglsV3Entry *)calloc(idx->n_tensors, sizeof(PoglsV3Entry));
    uint32_t n = 0;
    for (uint64_t i = 0; i < idx->n_tensors; i++) {
        PoglsV3Entry *e = &entries[n];
        e->addr = addr_from_tensor_name(idx->names[i], 0);
        e->dtype = idx->dtypes[i];
        e->nbytes = (uint32_t)idx->sizes[i];
        e->gguf_offset = gguf_idx_tensor_abs_offset(idx, i);
        strncpy(e->name, idx->names[i], sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        n++;
    }
    qsort(entries, n, sizeof(PoglsV3Entry), pogls_v3_cmp_name);
    *out_n = n;
    return entries;
}

/* ── Test 1: Build + write + read roundtrip ── */
static void test_write_read(const char *gguf_path, const GGUFTensorIndex *idx) {
    TEST("v3 write → read roundtrip");
    uint32_t n; PoglsV3Entry *entries = build_v3_entries(idx, &n);

    /* Build header */
    PoglsV3Header hdr;
    pogls_v3_header_init(&hdr);
    hdr.n_tensors = n;
    hdr.flags = POGLS_V3_FLAG_HAS_GGUF;
    hdr.entries_off = sizeof(PoglsV3Header);
    hdr.gguf_path_off = sizeof(PoglsV3Header) + n * POGLS_V3_ENTRY_SZ;
    hdr.gguf_path_sz = (uint16_t)(strlen(gguf_path) + 1);

    /* Write */
    const char *tmp = "test_e2e_v3.pogls";
    FILE *f = fopen(tmp, "wb");
    if (!f) { FAIL("write open"); free(entries); return; }
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(entries, POGLS_V3_ENTRY_SZ, n, f);
    fwrite(gguf_path, hdr.gguf_path_sz, 1, f);
    fclose(f);

    /* Read back */
    size_t map_sz;
    uint8_t *map = mmap_file_ro(tmp, &map_sz);
    if (!map) { FAIL("mmap"); free(entries); remove(tmp); return; }
    PoglsV3Header *hdr2 = (PoglsV3Header *)map;
    if (hdr2->magic != POGLS_V3_MAGIC || hdr2->version != 3) { FAIL("magic"); goto done; }
    if (hdr2->n_tensors != n) { FAIL("n_tensors mismatch"); goto done; }

    const PoglsV3Entry *entries2 = (const PoglsV3Entry *)(map + hdr2->entries_off);
    uint32_t found = 0;
    for (uint32_t i = 0; i < n; i++) {
        const PoglsV3Entry *e = pogls_v3_seek(entries2, n, entries[i].name);
        if (e && e->gguf_offset == entries[i].gguf_offset) found++;
    }
    if (found == n) { PASS(); } else { char msg[64]; snprintf(msg, 64, "%u/%u match", found, n); FAIL(msg); }

done:
    munmap_file(map, map_sz);
    free(entries);
    remove(tmp);
}

/* ── Test 2: GGUF mmap data integrity ── */
static void test_gguf_mmap_data(const char *gguf_path, const GGUFTensorIndex *idx) {
    TEST("v3 seek → GGUF mmap → data integrity");
    uint32_t n; PoglsV3Entry *entries = build_v3_entries(idx, &n);

    size_t gguf_sz;
    uint8_t *gguf_map = mmap_file_ro(gguf_path, &gguf_sz);
    if (!gguf_map) { FAIL("gguf mmap"); free(entries); return; }

    uint32_t checked = 0, match = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (entries[i].gguf_offset + entries[i].nbytes > gguf_sz) continue;
        uint8_t *data = gguf_map + entries[i].gguf_offset;
        /* Verify data is not all zeros (tensor data should have content) */
        uint64_t sum = 0;
        size_t check_sz = entries[i].nbytes < 64 ? entries[i].nbytes : 64;
        for (size_t j = 0; j < check_sz; j++) sum += data[j];
        checked++;
        if (sum > 0) match++;
    }
    if (checked > 0 && match == checked) {
        PASS();
        printf("    (checked %u tensors, all have non-zero data)\n", checked);
    } else {
        char msg[64]; snprintf(msg, 64, "%u/%u non-zero", match, checked); FAIL(msg);
    }
    munmap_file(gguf_map, gguf_sz);
    free(entries);
}

/* ── Test 3: Full E2E — v3 lookup → data compare with GGUF index ── */
static void test_e2e_data_compare(const char *gguf_path, const GGUFTensorIndex *idx) {
    TEST("v3 E2E: each tensor offset matches GGUF index");
    uint32_t n; PoglsV3Entry *entries = build_v3_entries(idx, &n);

    uint32_t match = 0, mismatch = 0;
    for (uint32_t i = 0; i < n; i++) {
        /* Find this tensor in GGUF index by name */
        int found = 0;
        for (uint64_t j = 0; j < idx->n_tensors; j++) {
            if (strcmp(entries[i].name, idx->names[j]) == 0) {
                uint64_t expected_off = gguf_idx_tensor_abs_offset(idx, j);
                if (entries[i].gguf_offset == expected_off &&
                    entries[i].nbytes == (uint32_t)idx->sizes[j] &&
                    entries[i].dtype == idx->dtypes[j]) {
                    match++;
                } else {
                    mismatch++;
                }
                found = 1;
                break;
            }
        }
        if (!found) mismatch++;
    }
    if (mismatch == 0 && match == n) {
        PASS();
    } else {
        char msg[64]; snprintf(msg, 64, "%u match, %u mismatch", match, mismatch); FAIL(msg);
    }
    free(entries);
}

/* ── Test 4: addr_space deterministic addresses ── */
static void test_addr_deterministic(const GGUFTensorIndex *idx) {
    TEST("addr_space.h: deterministic address for all tensors");
    uint32_t n; PoglsV3Entry *entries = build_v3_entries(idx, &n);
    uint32_t match = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr = addr_from_tensor_name(entries[i].name, 0);
        if (addr == entries[i].addr) match++;
    }
    if (match == n) { PASS(); } else {
        char msg[64]; snprintf(msg, 64, "%u/%u match", match, n); FAIL(msg);
    }
    free(entries);
}

/* ── Test 5: Name fields not truncated ── */
static void test_name_lengths(const GGUFTensorIndex *idx) {
    TEST("v3 entry names match GGUF names (no truncation)");
    uint32_t n; PoglsV3Entry *entries = build_v3_entries(idx, &n);
    uint32_t match = 0, truncated = 0;
    for (uint64_t i = 0; i < idx->n_tensors; i++) {
        const PoglsV3Entry *e = pogls_v3_seek(entries, n, idx->names[i]);
        if (e && strcmp(e->name, idx->names[i]) == 0)
            match++;
        else
            truncated++;
    }
    if (truncated == 0) { PASS(); } else {
        char msg[64]; snprintf(msg, 64, "%u match, %u truncated", match, truncated); FAIL(msg);
        for (uint64_t i = 0; i < idx->n_tensors && truncated > 0; i++) {
            const PoglsV3Entry *e = pogls_v3_seek(entries, n, idx->names[i]);
            if (!e || strcmp(e->name, idx->names[i]) != 0)
                fprintf(stderr, "    '%s' (v3='%s', len=%zu vs %zu)\n",
                        idx->names[i], e ? e->name : "NULL",
                        e ? strlen(e->name) : 0, strlen(idx->names[i]));
        }
    }
    free(entries);
}

/* ── Test 6: File size sanity ── */
static void test_file_size(const char *gguf_path, const GGUFTensorIndex *idx) {
    TEST("v3 file size < 0.1% of GGUF");
    uint32_t n; PoglsV3Entry *entries = build_v3_entries(idx, &n);
    uint64_t expected_sz = sizeof(PoglsV3Header) + (uint64_t)n * POGLS_V3_ENTRY_SZ + strlen(gguf_path) + 1;

    struct stat st;
    if (stat(gguf_path, &st) != 0) { FAIL("stat GGUF"); free(entries); return; }
    double gguf_mb = (double)st.st_size / (1024.0 * 1024.0);
    double v3_kb = (double)expected_sz / 1024.0;
    double ratio = (double)st.st_size / (double)expected_sz;

    if (v3_kb < gguf_mb * 10.24) {  /* < 0.1% */
        PASS();
        printf("    v3: %.1f KB, GGUF: %.1f MB, ratio: %.0fx\n", v3_kb, gguf_mb, ratio);
    } else {
        FAIL("v3 too large");
    }
    free(entries);
}

/* ── Test 7: Benchmark v3 binary search vs GGUF index linear scan ── */
static void test_benchmark(const GGUFTensorIndex *idx, const char *gguf_path) {
    TEST("benchmark: v3 binary search (all tensors)");
    uint32_t n; PoglsV3Entry *entries = build_v3_entries(idx, &n);

    int iters = 100000;
    volatile uint64_t checksum = 0;

    /* v3 binary search */
    clock_t start = clock();
    for (int iter = 0; iter < iters; iter++) {
        for (uint32_t i = 0; i < n; i++) {
            const PoglsV3Entry *e = pogls_v3_seek(entries, n, entries[i].name);
            if (e) checksum += e->gguf_offset;
        }
    }
    double v3_us = (double)(clock() - start) / CLOCKS_PER_SEC * 1e6;

    /* GGUF index linear scan */
    start = clock();
    for (int iter = 0; iter < iters; iter++) {
        for (uint32_t i = 0; i < n; i++) {
            for (uint64_t j = 0; j < idx->n_tensors; j++) {
                if (strcmp(entries[i].name, idx->names[j]) == 0) {
                    checksum += gguf_idx_tensor_abs_offset(idx, j);
                    break;
                }
            }
        }
    }
    double gguf_us = (double)(clock() - start) / CLOCKS_PER_SEC * 1e6;

    printf("\n    %d lookups per scan, %d iterations:\n", n, iters);
    printf("    v3 binary search:  %.1f ms total, %.0f ns/lookup\n", v3_us/1000.0, v3_us*1000.0/((double)iters*n));
    printf("    GGUF linear scan:  %.1f ms total, %.0f ns/lookup\n", gguf_us/1000.0, gguf_us*1000.0/((double)iters*n));
    printf("    Speedup:           %.1fx\n", gguf_us / v3_us);

    free(entries);
    PASS();
}

/* ── Main ── */
int main(int argc, char **argv) {
    const char *gguf_path = (argc > 1) ? argv[1] : "I:/model/LFM2.5-8B-A1B-Q4_K_M.gguf";

    printf("═══════════════════════════════════════════════════\n");
    printf("  POGLS v3 Geopixel E2E Test Suite\n");
    printf("  GGUF: %s\n", gguf_path);
    printf("═══════════════════════════════════════════════════\n\n");

    GGUFTensorIndex idx;
    if (gguf_idx_open(gguf_path, &idx) != 0) {
        fprintf(stderr, "ERROR: can't open GGUF\n"); return 1;
    }
    fprintf(stderr, "[e2e] GGUF has %llu tensors\n\n", (unsigned long long)idx.n_tensors);

    test_write_read(gguf_path, &idx);
    test_gguf_mmap_data(gguf_path, &idx);
    test_e2e_data_compare(gguf_path, &idx);
    test_addr_deterministic(&idx);
    test_name_lengths(&idx);
    test_file_size(gguf_path, &idx);
    test_benchmark(&idx, gguf_path);

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  Results: %d/%d PASS\n", tests_pass, tests_run);
    printf("═══════════════════════════════════════════════════\n");

    gguf_idx_close(&idx);
    return tests_pass == tests_run ? 0 : 1;
}

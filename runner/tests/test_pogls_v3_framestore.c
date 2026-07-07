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
      (void)sz; if (p) UnmapViewOfFile(p);
  }
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  static uint8_t *mmap_file_ro(const char *path, size_t *out_sz) {
      struct stat st; if (stat(path, &st) != 0) return NULL;
      *out_sz = st.st_size; int fd = open(path, O_RDONLY);
      if (fd < 0) return NULL;
      void *p = mmap(NULL, *out_sz, PROT_READ, MAP_PRIVATE, fd, 0);
      close(fd); return (p == MAP_FAILED) ? NULL : (uint8_t *)p;
  }
  static void munmap_file(uint8_t *p, size_t sz) { if (p) munmap(p, sz); }
#endif

#include "pogls_v3_framestore.h"
#include "gguf_index.h"
#include "addr_space.h"

static int tests_run = 0, tests_pass = 0;
#define TEST(n) do { tests_run++; printf("  [%d] %-55s ", tests_run, n); fflush(stdout); } while(0)
#define PASS() do { tests_pass++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static int build_entries(const GGUFTensorIndex *idx,
                          FrameStoreEntry **out_entries, uint32_t *out_n)
{
    uint64_t n = idx->n_tensors;
    FrameStoreEntry *e = (FrameStoreEntry *)calloc(n, sizeof(FrameStoreEntry));
    if (!e) return -1;
    for (uint64_t i = 0; i < n; i++) {
        e[i].addr   = addr_from_tensor_name(idx->names[i], 0);
        e[i].nbytes = (uint32_t)idx->sizes[i];
    }
    qsort(e, n, sizeof(FrameStoreEntry), framestore_cmp_addr);
    *out_entries = e;
    *out_n = (uint32_t)n;
    return 0;
}

/* ── Test 1: write/read roundtrip ── */
static void test_roundtrip(const char *gguf_path, const GGUFTensorIndex *idx) {
    TEST("framestore write → read roundtrip");
    uint32_t n; FrameStoreEntry *entries;
    if (build_entries(idx, &entries, &n) != 0) { FAIL("build entries"); return; }

    FrameStoreHeader hdr;
    framestore_header_init(&hdr);
    hdr.n_tensors = n;
    hdr.data_off = FRAMESTORE_HEADER_SZ + n * FRAMESTORE_ENTRY_SZ;

    const char *tmp = "test_fs_tmp.bin";
    FILE *fout = fopen(tmp, "wb");
    if (!fout) { FAIL("write open"); free(entries); return; }
    fwrite(&hdr, FRAMESTORE_HEADER_SZ, 1, fout);
    fwrite(entries, FRAMESTORE_ENTRY_SZ, n, fout);
    uint32_t dummy = 0xDEADBEEF;
    fwrite(&dummy, sizeof(dummy), 1, fout);
    fclose(fout);

    FrameStoreHeader hdr2;
    if (framestore_read_header(tmp, &hdr2) != 0) { FAIL("read header"); remove(tmp); free(entries); return; }
    if (hdr2.n_tensors != n || hdr2.addr_map_off != FRAMESTORE_HEADER_SZ) {
        FAIL("header fields"); remove(tmp); free(entries); return;
    }

    FILE *fin = fopen(tmp, "rb");
    fseek(fin, hdr2.addr_map_off, SEEK_SET);
    FrameStoreEntry *entries2 = (FrameStoreEntry *)malloc(n * FRAMESTORE_ENTRY_SZ);
    fread(entries2, FRAMESTORE_ENTRY_SZ, n, fin);
    fclose(fin);

    uint32_t ok = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (entries[i].addr == entries2[i].addr && entries[i].nbytes == entries2[i].nbytes)
            ok++;
    }
    if (ok == n) { PASS(); } else { char m[64]; snprintf(m,64,"%u/%u",ok,n); FAIL(m); }
    free(entries2);
    free(entries);
    remove(tmp);
}

/* ── Test 2: addr seek correctness ── */
static void test_seek(const GGUFTensorIndex *idx) {
    TEST("framestore_seek: find by addr");
    uint32_t n; FrameStoreEntry *entries;
    if (build_entries(idx, &entries, &n) != 0) { FAIL("build"); return; }

    uint32_t ok = 0;
    for (uint32_t i = 0; i < n; i++) {
        const FrameStoreEntry *e = framestore_seek(entries, n, entries[i].addr);
        if (e != NULL && e->addr == entries[i].addr && e->nbytes == entries[i].nbytes)
            ok++;
    }
    if (ok == n) { PASS(); } else { char m[64]; snprintf(m,64,"%u/%u",ok,n); FAIL(m); }
    free(entries);
}

/* ── Test 3: addr uniqueness ── */
static void test_uniqueness(const GGUFTensorIndex *idx) {
    TEST("no duplicate addresses");
    uint32_t n; FrameStoreEntry *entries;
    if (build_entries(idx, &entries, &n) != 0) { FAIL("build"); return; }

    int dup = 0;
    for (uint32_t i = 1; i < n; i++) {
        if (entries[i].addr == entries[i-1].addr) {
            if (!dup) fprintf(stderr, "\n    duplicate: addr=%u", entries[i].addr);
            dup++;
        }
    }
    if (dup == 0) { PASS(); } else { char m[64]; snprintf(m,64,"%d duplicates",dup); FAIL(m); }
    free(entries);
}

/* ── Test 4: file size sanity ── */
static void test_file_size(const GGUFTensorIndex *idx) {
    TEST("file structure overhead < 0.01%");
    uint32_t n; FrameStoreEntry *entries;
    if (build_entries(idx, &entries, &n) != 0) { FAIL("build"); return; }

    uint64_t total_data = 0;
    for (uint32_t i = 0; i < n; i++) total_data += entries[i].nbytes;
    uint32_t overhead = FRAMESTORE_HEADER_SZ + n * FRAMESTORE_ENTRY_SZ;
    double ratio = (double)total_data / (double)overhead;

    if (ratio > 50000.0) {
        PASS();
        printf("\n    overhead: %u B, data: %.2f GB, ratio: %.0fx", overhead,
               (double)total_data / (1e9), ratio);
    } else {
        char m[64]; snprintf(m,64,"ratio too low: %.0fx",ratio); FAIL(m);
    }
    free(entries);
}

/* ── Test 5: benchmark addr binary search ── */
static void test_benchmark(const GGUFTensorIndex *idx) {
    TEST("benchmark: addr binary search (all tensors)");
    uint32_t n; FrameStoreEntry *entries;
    if (build_entries(idx, &entries, &n) != 0) { FAIL("build"); return; }

    int iters = 100000;
    volatile uint64_t checksum = 0;
    clock_t start = clock();
    for (int iter = 0; iter < iters; iter++) {
        for (uint32_t i = 0; i < n; i++) {
            const FrameStoreEntry *e = framestore_seek(entries, n, entries[i].addr);
            if (e) checksum += e->file_offset;
        }
    }
    double total_us = (double)(clock() - start) / CLOCKS_PER_SEC * 1e6;
    double ns_per_lookup = total_us * 1000.0 / ((double)iters * n);

    printf("\n    %d lookups × %d iterations = %.0f total lookups\n",
           n, iters, (double)n * iters);
    printf("    %.1f ms total, %.0f ns/lookup\n", total_us/1000.0, ns_per_lookup);
    PASS();
    free(entries);
}

/* ── Test 6: deterministic addresses ── */
static void test_addr_deterministic(const GGUFTensorIndex *idx) {
    TEST("addr_space.h: deterministic address (by name)");
    uint32_t n; FrameStoreEntry *entries;
    if (build_entries(idx, &entries, &n) != 0) { FAIL("build"); return; }

    uint32_t ok = 0;
    for (uint64_t i = 0; i < idx->n_tensors; i++) {
        uint32_t addr = addr_from_tensor_name(idx->names[i], 0);
        const FrameStoreEntry *e = framestore_seek(entries, n, addr);
        if (e != NULL && e->addr == addr) ok++;
    }
    if (ok == n) { PASS(); } else { char m[64]; snprintf(m,64,"%u/%llu",ok,(unsigned long long)n); FAIL(m); }
    free(entries);
}

int main(int argc, char **argv) {
    const char *gguf_path = (argc > 1) ? argv[1] : "I:/model/LFM2.5-8B-A1B-Q4_K_M.gguf";

    printf("═══════════════════════════════════════════════════\n");
    printf("  POGLS v3 FrameStore E2E Test Suite\n");
    printf("  GGUF: %s\n", gguf_path);
    printf("═══════════════════════════════════════════════════\n\n");

    GGUFTensorIndex idx;
    if (gguf_idx_open(gguf_path, &idx) != 0) {
        fprintf(stderr, "ERROR: can't open GGUF\n"); return 1;
    }
    fprintf(stderr, "[framestore] GGUF has %llu tensors\n\n", (unsigned long long)idx.n_tensors);

    test_roundtrip(gguf_path, &idx);
    test_seek(&idx);
    test_uniqueness(&idx);
    test_file_size(&idx);
    test_benchmark(&idx);
    test_addr_deterministic(&idx);

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  Results: %d/%d PASS\n", tests_pass, tests_run);
    printf("═══════════════════════════════════════════════════\n");

    gguf_idx_close(&idx);
    return tests_pass == tests_run ? 0 : 1;
}

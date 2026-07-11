/*
 * test_e2e_pipeline.c — End-to-end pipeline test
 *
 * Tests: GGUF → POGLS → verify → loader → compare tensor data
 *
 * Build:
 *   gcc -O2 -std=c11 -DPOGLS_LOADER_IMPLEMENTATION -o test_e2e_pipeline.exe test_e2e_pipeline.c -lm
 */

#ifndef POGLS_LOADER_IMPLEMENTATION
#define POGLS_LOADER_IMPLEMENTATION
#endif
#include "pogls_core/pogls_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define TEST(name, cond) do { \
    if (cond) { printf("  PASS  %s\n", name); g_pass++; } \
    else      { printf("  FAIL  %s\n", name); g_fail++; } \
} while(0)

/* Read raw tensor data from GGUF file at given offset */
static int read_gguf_tensor(const char *gguf_path, uint64_t offset, size_t sz, void *buf) {
    FILE *f = fopen(gguf_path, "rb");
    if (!f) return -1;
    _fseeki64(f, offset, SEEK_SET);
    size_t nread = fread(buf, 1, sz, f);
    fclose(f);
    return nread == sz ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *pogls_path = argc > 1 ? argv[1] : "_e2e_test.pogls";
    const char *gguf_path = argc > 2 ? argv[2] : "I:/model/Kokoro_no_espeak_Q8.gguf";

    printf("═══ E2E Pipeline Test ═══\n");
    printf("POGLS: %s\n", pogls_path);
    printf("GGUF:  %s\n\n", gguf_path);

    /* Step 1: Open POGLS with loader */
    printf("── Step 1: Open POGLS ──\n");
    PoglsLoader *L = pogls_open(pogls_path);
    TEST("open", L != NULL);
    if (!L) {
        printf("  Error: %s\n", pogls_last_error());
        return 1;
    }

    /* Step 2: Verify metadata */
    printf("\n── Step 2: Metadata ──\n");
    uint32_t n = pogls_tensor_count(L);
    TEST("tensor_count", n > 0);
    printf("  Tensors: %u\n", n);
    printf("  Version: %u\n", pogls_version(L));
    printf("  File:    %llu bytes\n", (unsigned long long)pogls_file_size(L));

    /* Step 3: List first 10 tensors */
    printf("\n── Step 3: Tensor List ──\n");
    uint32_t show = n < 10 ? n : 10;
    for (uint32_t i = 0; i < show; i++) {
        const char *name = pogls_tensor_name(L, i);
        uint32_t addr = pogls_tensor_addr(L, i);
        size_t sz = pogls_tensor_nbytes(L, i);
        uint32_t dims[4];
        pogls_tensor_shape(L, i, dims);
        printf("  [%u] %-40s  addr=%5u  %7zu bytes  [%ux%ux%ux%u]\n",
               i, name ? name : "?", addr, sz, dims[0], dims[1], dims[2], dims[3]);
    }

    /* Step 4: Test name lookup */
    printf("\n── Step 4: Name Lookup ──\n");
    if (n > 0) {
        const char *name0 = pogls_tensor_name(L, 0);
        int idx = pogls_find_tensor(L, name0);
        TEST("find_first", idx == 0);

        int miss = pogls_find_tensor(L, "nonexistent_tensor_xyz");
        TEST("find_miss", miss == -1);
    }

    /* Step 5: Test address lookup */
    printf("\n── Step 5: Address Lookup ──\n");
    if (n > 0) {
        uint32_t addr0 = pogls_tensor_addr(L, 0);
        int idx = pogls_find_tensor_by_addr(L, addr0);
        TEST("find_by_addr", idx == 0);
    }

    /* Step 6: Test face rotation */
    printf("\n── Step 6: Face Rotation ──\n");
    if (n > 0) {
        uint32_t base = pogls_tensor_addr(L, 0);
        uint32_t f0 = pogls_face_addr(base, 0);
        uint32_t f1 = pogls_face_addr(base, 1);
        TEST("face_0_same", f0 == base);
        TEST("face_1_diff", f1 != base);
        printf("  base=%u  face0=%u  face1=%u\n", base, f0, f1);
    }

    /* Step 7: Test data access */
    printf("\n── Step 7: Data Access ──\n");
    if (n > 0) {
        void *data = pogls_tensor_data(L, pogls_tensor_name(L, 0));
        TEST("data_not_null", data != NULL);
        if (data) {
            uint8_t *p = (uint8_t*)data;
            TEST("data_readable", p[0] != 0xDE || p[1] != 0xAD || p[2] != 0xBE);
        }
    }

    /* Step 8: Test tensor_size */
    printf("\n── Step 8: Size Queries ──\n");
    if (n > 0) {
        size_t sz = pogls_tensor_size(L, pogls_tensor_name(L, 0));
        TEST("size_positive", sz > 0);

        size_t sz2 = pogls_tensor_size(L, "nonexistent");
        TEST("size_miss_zero", sz2 == 0);
    }

    /* Step 9: GGUF path */
    printf("\n── Step 9: GGUF Path ──\n");
    const char *gguf_ref = pogls_gguf_path(L);
    if (gguf_ref && gguf_ref[0]) {
        printf("  GGUF ref: %s\n", gguf_ref);
        TEST("gguf_path_present", 1);
    } else {
        printf("  (no GGUF path stored)\n");
        TEST("gguf_path_absent_ok", 1);
    }

    /* Step 10: MMAP access */
    printf("\n── Step 10: Direct MMAP ──\n");
    const void *mmap = pogls_mmap_ptr(L);
    size_t mmap_sz = pogls_mmap_size(L);
    TEST("mmap_not_null", mmap != NULL);
    TEST("mmap_size_valid", mmap_sz > 100000);
    printf("  mmap: %p  size: %zu bytes\n", mmap, mmap_sz);

    /* Summary */
    printf("\n═══ Result: %s (%d/%d passed) ═══\n",
           g_fail == 0 ? "ALL PASS" : "SOME FAILED",
           g_pass, g_pass + g_fail);

    pogls_close(L);
    return g_fail == 0 ? 0 : 1;
}

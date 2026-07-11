/*
 * test_pogls_loader.c — Test suite for pogls_loader.h
 *
 * Build:
 *   gcc -O2 -std=c11 -o test_pogls_loader.exe test_pogls_loader.c -lm
 *
 * Or link against libpogls_loader.a.
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

/* ── Test: Open invalid file ── */
static void test_open_invalid(void) {
    printf("\n── Open Invalid ──\n");
    PoglsLoader *L = pogls_open("nonexistent.pogls");
    TEST("open_nonexistent", L == NULL);
    TEST("error_message", pogls_last_error() != NULL);
}

/* ── Test: Open real POGLS file ── */
static void test_open_real(void) {
    printf("\n── Open Real POGLS ──\n");
    fflush(stdout);

    /* Create a minimal POGLS file for testing */
    const char *pogls = "_test_loader.pogls";
    printf("  Creating test file...\n");
    fflush(stdout);

    /* Header using the actual struct layout */
    PoglsLHeader hdr = {0};
    hdr.magic = 0x53474F50u;
    hdr.version = 2;
    hdr.n_tensors = 3;
    hdr.flags = 0x0001u; /* HAS_TMETA */
    hdr.tensor_meta_off = 128 + 331776;
    hdr.tensor_meta_count = 3;

    printf("  Header: magic=0x%08X ver=%u n_tensors=%u meta_off=%llu meta_count=%u\n",
           hdr.magic, hdr.version, hdr.n_tensors,
           (unsigned long long)hdr.tensor_meta_off, hdr.tensor_meta_count);
    fflush(stdout);

    /* Write to file */
    FILE *pf = fopen(pogls, "wb");
    if (!pf) { printf("  FAIL  cannot create file\n"); return; }
    fwrite(&hdr, 1, 128, pf);
    printf("  Wrote header: 128 bytes\n");
    fflush(stdout);

    /* Write index (20736 × 16B = 331776B) — all zeros */
    uint8_t chunk[8192] = {0};
    size_t remaining = 331776;
    while (remaining > 0) {
        size_t to_write = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        fwrite(chunk, 1, to_write, pf);
        remaining -= to_write;
    }
    printf("  Wrote index: 331776 bytes\n");
    fflush(stdout);

    /* Write 3 tensor meta entries */
    typedef struct {
        uint32_t addr, dtype, ndim, nbytes_orig, comp_type, comp_nbytes;
        uint32_t dims[4];
        char name[64];
    } TMeta;

    TMeta tm[3] = {0};
    tm[0].addr = 11; tm[0].dtype = 0; tm[0].ndim = 2;
    tm[0].nbytes_orig = 256; tm[0].comp_type = 0; tm[0].comp_nbytes = 0;
    tm[0].dims[0] = 16; tm[0].dims[1] = 16;
    strncpy(tm[0].name, "tensor_a", 64);

    tm[1].addr = 1739; tm[1].dtype = 1; tm[1].ndim = 1;
    tm[1].nbytes_orig = 128; tm[1].comp_type = 0; tm[1].comp_nbytes = 0;
    tm[1].dims[0] = 128;
    strncpy(tm[1].name, "tensor_b", 64);

    tm[2].addr = 3467; tm[2].dtype = 8; tm[2].ndim = 3;
    tm[2].nbytes_orig = 512; tm[2].comp_type = 0; tm[2].comp_nbytes = 0;
    tm[2].dims[0] = 8; tm[2].dims[1] = 8; tm[2].dims[2] = 8;
    strncpy(tm[2].name, "tensor_c", 64);

    fwrite(tm, sizeof(TMeta), 3, pf);
    printf("  Wrote tensor meta: %zu bytes\n", sizeof(TMeta) * 3);
    fflush(stdout);

    /* Write tensor data */
    uint8_t data_a[256]; for (int i = 0; i < 256; i++) data_a[i] = (uint8_t)i;
    uint8_t data_b[128]; for (int i = 0; i < 128; i++) data_b[i] = (uint8_t)(i + 100);
    uint8_t data_c[512]; for (int i = 0; i < 512; i++) data_c[i] = (uint8_t)(i + 200);
    fwrite(data_a, 1, 256, pf);
    fwrite(data_b, 1, 128, pf);
    fwrite(data_c, 1, 512, pf);
    printf("  Wrote tensor data: 896 bytes\n");
    fflush(stdout);

    long file_end = ftell(pf);
    printf("  Total file size: %ld bytes\n", file_end);
    fclose(pf);

    /* Now test the loader */
    printf("  Opening with loader...\n");
    fflush(stdout);
    PoglsLoader *L = pogls_open(pogls);
    printf("  pogls_open returned: %p\n", (void*)L);
    fflush(stdout);
    if (!L) {
        printf("  FAIL  open: %s\n", pogls_last_error() ? pogls_last_error() : "unknown");
        g_fail++;
        return;
    }

    /* Metadata */
    printf("  Checking version...\n");
    fflush(stdout);
    TEST("version", pogls_version(L) == 2);

    printf("  Checking tensor_count...\n");
    fflush(stdout);
    TEST("tensor_count", pogls_tensor_count(L) == 3);

    printf("  Checking tensor_0_name...\n");
    fflush(stdout);
    const char *n0 = pogls_tensor_name(L, 0);
    printf("    name: %s\n", n0 ? n0 : "NULL");
    fflush(stdout);
    TEST("tensor_0_name", n0 && strcmp(n0, "tensor_a") == 0);
    TEST("tensor_0_addr", pogls_tensor_addr(L, 0) == 11);
    TEST("tensor_0_ndim", pogls_tensor_ndim(L, 0) == 2);
    TEST("tensor_0_nbytes", pogls_tensor_nbytes(L, 0) == 256);

    TEST("tensor_1_name", strcmp(pogls_tensor_name(L, 1), "tensor_b") == 0);
    TEST("tensor_1_addr", pogls_tensor_addr(L, 1) == 1739);

    TEST("tensor_2_name", strcmp(pogls_tensor_name(L, 2), "tensor_c") == 0);
    TEST("tensor_2_addr", pogls_tensor_addr(L, 2) == 3467);

    /* Tensor by name */
    int idx = pogls_find_tensor(L, "tensor_b");
    TEST("find_by_name", idx == 1);

    int idx_missing = pogls_find_tensor(L, "nonexistent");
    TEST("find_missing", idx_missing == -1);

    /* Tensor by address */
    int aidx = pogls_find_tensor_by_addr(L, 3467);
    TEST("find_by_addr", aidx == 2);

    /* Data pointers */
    void *d0 = pogls_tensor_data(L, "tensor_a");
    TEST("data_tensor_a", d0 != NULL);
    if (d0) {
        uint8_t *p = (uint8_t*)d0;
        TEST("data_tensor_a_content", p[0] == 0 && p[1] == 1 && p[255] == 255);
    }

    void *d1 = pogls_tensor_data(L, "tensor_b");
    TEST("data_tensor_b", d1 != NULL);
    if (d1) {
        uint8_t *p = (uint8_t*)d1;
        TEST("data_tensor_b_content", p[0] == 100 && p[1] == 101);
    }

    void *d2 = pogls_tensor_data_by_addr(L, 3467);
    TEST("data_by_addr", d2 != NULL);

    /* Size queries */
    TEST("tensor_size", pogls_tensor_size(L, "tensor_a") == 256);
    TEST("tensor_size_missing", pogls_tensor_size(L, "nope") == 0);

    /* File size */
    TEST("file_size", pogls_file_size(L) > 332000);

    pogls_close(L);
    remove(pogls);
}

/* ── Main ── */
int main(void) {
    printf("═══ pogls_loader Test Suite ═══\n");

    test_open_invalid();
    test_open_real();

    printf("\n═══ Result: %s (%d/%d passed) ═══\n",
           g_fail == 0 ? "ALL PASS" : "SOME FAILED",
           g_pass, g_pass + g_fail);

    return g_fail == 0 ? 0 : 1;
}

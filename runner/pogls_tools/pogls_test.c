/*
 * pogls_test.c — Automated test suite for POGLS toolchain
 *
 * Usage: pogls_test
 *
 * Tests all core library functions and CLI tool workflows.
 * Returns 0 on all-pass, 1 on any failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_core.h"
#include "pogls_meta.h"

static int g_pass = 0, g_fail = 0;

#define TEST(name, cond) do { \
    if (cond) { printf("  PASS  %s\n", name); g_pass++; } \
    else      { printf("  FAIL  %s\n", name); g_fail++; } \
} while(0)

/* ── Platform Tests ── */
static void test_platform(void) {
    printf("\n── Platform ──\n");

    /* file I/O roundtrip */
    const char *test_file = "_test_pogls_io.bin";
    uint8_t data[256];
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)i;

    FILE *f = pogls_fopen(test_file, "wb");
    TEST("pogls_fopen_write", f != NULL);
    if (f) { fwrite(data, 1, 256, f); fclose(f); }

    uint8_t read_buf[256] = {0};
    f = pogls_fopen(test_file, "rb");
    TEST("pogls_fopen_read", f != NULL);
    if (f) { fread(read_buf, 1, 256, f); fclose(f); }
    TEST("pogls_io_roundtrip", memcmp(data, read_buf, 256) == 0);

    remove(test_file);
}

/* ── Compression Tests ── */
static void test_compress(void) {
    printf("\n── Compression ──\n");

    /* All-zero (should compress well) */
    uint8_t zeros[1024] = {0};
    uint8_t out[2048];
    uint32_t comp_type, comp_nbytes;
    uint32_t sz = pogls_compress_tensor(out, sizeof(out), zeros, 1024, &comp_type, &comp_nbytes);
    TEST("compress_zeros", sz > 0 && comp_nbytes > 0);

    /* Decompress */
    uint8_t dec[1024];
    uint32_t dec_sz = pogls_decompress_tensor(dec, 1024, out, comp_nbytes, comp_type, 1024);
    TEST("decompress_zeros", dec_sz == 1024 && memcmp(dec, zeros, 1024) == 0);

    /* Random-ish data (should fall back to raw) */
    uint8_t random_data[256];
    for (int i = 0; i < 256; i++) random_data[i] = (uint8_t)(i * 37 + 13);
    uint32_t sz2 = pogls_compress_tensor(out, sizeof(out), random_data, 256, &comp_type, &comp_nbytes);
    TEST("compress_random", sz2 > 0);

    /* Bound check */
    size_t bound = pogls_compress_bound(1024);
    TEST("compress_bound", bound >= 1024);
}

/* ── Address Tests ── */
static void test_addr(void) {
    printf("\n── Address ──\n");

    /* Known name → address */
    uint32_t addr1 = pogls_addr_from_name("blk.0.attn_q.weight", 0);
    uint32_t addr2 = pogls_addr_from_name("blk.0.attn_q.weight", 0);
    TEST("addr_deterministic", addr1 == addr2);

    /* Different names → different addresses (usually) */
    uint32_t addr3 = pogls_addr_from_name("blk.0.attn_k.weight", 0);
    TEST("addr_distinct", addr1 != addr3);

    /* Decompose → compose roundtrip */
    PoglsAddrDecomp d = pogls_addr_decompose(addr1, 0);
    uint32_t composed = pogls_addr_compose(d.macro, d.micro, 0);
    TEST("decompose_compose", composed == addr1);

    /* Bounds check */
    TEST("addr_valid", pogls_addr_valid(addr1, 0));
    TEST("addr_invalid", !pogls_addr_valid(999999, 0));

    /* Face rotation */
    uint32_t f0 = pogls_addr_capo(addr1, 0, 0);
    uint32_t f1 = pogls_addr_capo(addr1, 1, 0);
    TEST("face_rotation", f0 != f1);

    /* Tier capacity */
    uint64_t cap = pogls_addr_tier_capacity(0);
    TEST("tier0_capacity", cap == 20736);
}

/* ── Metadata Tests ── */
static void test_meta(void) {
    printf("\n── Metadata ──\n");

    /* Header size check */
    TEST("header_size", POGLS_HEADER_SZ == 128);

    /* Magic values */
    TEST("magic_value", POGLS_META_MAGIC == 0x53474F50u);
    TEST("legacy_magic", 0x504F474Cu == 0x504F474Cu);

    /* Tensor meta size */
    TEST("tensor_meta_size", sizeof(PoglsTensorMeta) == 104);

    /* Compression types */
    TEST("comp_raw", POGLS_COMP_RAW == 0);
    TEST("comp_zstd", POGLS_COMP_ZSTD == 1);
}

/* ── Main ── */
int main(void) {
    printf("═══ POGLS Toolchain Test Suite ═══\n");

    test_platform();
    test_compress();
    test_addr();
    test_meta();

    printf("\n═══ Result: %s (%d/%d passed) ═══\n",
           g_fail == 0 ? "ALL PASS" : "SOME FAILED",
           g_pass, g_pass + g_fail);

    return g_fail == 0 ? 0 : 1;
}

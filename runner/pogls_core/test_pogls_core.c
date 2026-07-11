#include "pogls_core.h"
#include <string.h>
#include <assert.h>

static int pass = 0, fail = 0;

#define TEST(name) do { printf("  [%s]\n", name); } while(0)
#define PASS(msg) do { printf("    PASS: %s\n", msg); pass++; } while(0)
#define FAIL(msg) do { printf("    FAIL: %s\n", msg); fail++; } while(0)
#define CHECK(cond, msg) do { if (cond) PASS(msg); else FAIL(msg); } while(0)

static void test_platform(void) {
    TEST("Platform layer");
    int64_t sz = pogls_fsize("pogls_core/pogls_core.h");
    CHECK(sz > 0, "pogls_fsize returns > 0");
    void *p = pogls_alloc_large(65536);
    CHECK(p != NULL, "pogls_alloc_large(64KB)");
    if (p) {
        memset(p, 0xAB, 65536);
        pogls_free_large(p, 65536);
        PASS("write + free large memory");
    }
    FILE *f = pogls_fopen("pogls_core/pogls_core.h", "rb");
    CHECK(f != NULL, "pogls_fopen exists");
    if (f) {
        CHECK(pogls_fseek(f, 0, SEEK_END) == 0, "pogls_fseek end");
        int64_t pos = pogls_ftell(f);
        CHECK(pos > 0, "pogls_ftell > 0");
        fclose(f);
    }
}

static void test_compress(void) {
    TEST("Compression API");
    uint8_t src[] = "Hello POGLS! This is a test of the compression system.";
    size_t src_sz = sizeof(src) - 1;
    uint8_t dst[256];
    uint8_t out[256];
    PoglsCompMeta meta;
    meta.nbytes_orig = (uint32_t)src_sz;

    uint32_t csz = pogls_compress(dst, sizeof(dst), src, src_sz, &meta);
    CHECK(csz > 0 && csz <= src_sz, "compress returned valid size");
    CHECK(meta.comp_type == POGLS_COMP_RAW, "small data stored as RAW (no zstd)");

    uint32_t dsz = pogls_decompress(out, sizeof(out), dst, &meta);
    CHECK(dsz == src_sz, "decompress returned original size");
    CHECK(memcmp(out, src, src_sz) == 0, "decompress data matches original");
}

static void test_addr(void) {
    TEST("Address space");
    CHECK(POGLS_BASE == 20736, "POGLS_BASE = 20736");
    CHECK(POGLS_DIM_A == 128, "POGLS_DIM_A = 128");
    CHECK(POGLS_DIM_B == 162, "POGLS_DIM_B = 162");

    uint8_t t = pogls_select_tier(100, 128);
    CHECK(t == 0, "100 tensors → tier 0");

    uint32_t a = pogls_from_name("blk.0.attn_q.weight", 0);
    CHECK(a < POGLS_BASE, "tensor name maps to valid address");

    PoglsAddrDecomp d = pogls_decompose(a, 0);
    CHECK(d.macro < 128 && d.micro < 256, "decompose macro/micro in range");

    uint32_t composed = pogls_compose(d.macro, d.micro, 0);
    CHECK(composed == a, "compose roundtrip matches original");

    uint32_t capo1 = pogls_capo(a, 1, 0);
    uint32_t capo0 = pogls_capo(a, 0, 0);
    CHECK(capo1 != a, "capo face 1 changes address");
    CHECK(capo0 == a, "capo face 0 returns original");

    PoglsGeoDecomp g = pogls_to_geo(a, 0);
    CHECK(g.slot < 128, "geo decomposition slot < 128");

    CHECK(pogls_addr_valid(a, 0), "address valid in tier 0");
    CHECK(!pogls_addr_valid(POGLS_BASE, 0), "address >= BASE invalid");
}

static void test_meta(void) {
    TEST("Metadata format");
    PoglsStoreHeader hdr;
    pogls_meta_header_init(&hdr);
    CHECK(hdr.magic == POGLS_META_MAGIC, "header init sets magic");
    CHECK(hdr.version == POGLS_META_VERSION, "header init sets version");

    uint64_t data_off = pogls_meta_data_off(&hdr);
    CHECK(data_off == sizeof(hdr) + POGLS_INDEX_SZ, "data offset for header-only");

    PoglsTensorMeta tm;
    pogls_meta_entry_init(&tm);
    CHECK(tm.addr == 0 && tm.name[0] == '\0', "entry init zeroed");

    const char *tmp_path = "_test_pogls_meta.bin";
    hdr.tensor_meta_count = 2;
    hdr.tensor_meta_off = sizeof(hdr) + POGLS_INDEX_SZ;

    PoglsTensorMeta meta[2];
    pogls_meta_entry_init(&meta[0]);
    pogls_meta_entry_init(&meta[1]);
    meta[0].addr = 42;
    strcpy(meta[0].name, "test.tensor.0");
    meta[0].nbytes_orig = 128;
    meta[1].addr = 99;
    strcpy(meta[1].name, "test.tensor.1");
    meta[1].nbytes_orig = 256;

    uint8_t data[384];
    memset(data, 0xAA, 128);
    memset(data + 128, 0xBB, 256);

    int ok = pogls_meta_write(tmp_path, &hdr, NULL, meta, NULL, data, 384);
    CHECK(ok == 0, "write v2 file");

    PoglsStoreHeader hdr2;
    uint8_t idx2[POGLS_INDEX_SZ];
    uint8_t meta_buf[2 * POGLS_META_ENTRY_SZ];
    uint64_t data_off2 = 0;
    ok = pogls_meta_read(tmp_path, &hdr2, idx2, meta_buf, &data_off2);
    CHECK(ok == 0, "read back v2 file");
    CHECK(hdr2.magic == POGLS_META_MAGIC, "readback magic matches");
    CHECK(hdr2.n_tensors == 0, "v2 header n_tensors (no index data)");
    CHECK(data_off2 > 0, "data offset > 0");

    const PoglsTensorMeta *found = pogls_meta_find_name(
        (PoglsTensorMeta*)meta_buf, 2, "test.tensor.1");
    CHECK(found != NULL, "find_name found tensor.1");
    if (found) CHECK(found->addr == 99, "found tensor has addr=99");

    found = pogls_meta_find((PoglsTensorMeta*)meta_buf, 2, 42);
    CHECK(found != NULL, "find addr 42 found");
    if (found) CHECK(strcmp(found->name, "test.tensor.0") == 0, "found tensor has correct name");

    remove(tmp_path);
}

static void test_store_contract(void) {
    TEST("Store contract (API exists)");
    CHECK(pogls_store_open != NULL, "pogls_store_open declared");
    CHECK(pogls_store_put != NULL, "pogls_store_put declared");
    CHECK(pogls_store_get != NULL, "pogls_store_get declared");
}

int main(void) {
    printf("═══ POGLS Core Library Test ═══\n\n");
    test_platform();
    test_compress();
    test_addr();
    test_meta();
    test_store_contract();
    printf("\n═══ Results: %d pass, %d fail ═══\n", pass, fail);
    return fail > 0 ? 1 : 0;
}

#include <stdio.h>
#include <string.h>
#include "pogls_dram.h"

static int g_tests = 0;
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) do { \
    g_tests++; \
    printf("  test %d: %s ... ", g_tests, name); \
    fflush(stdout); \
} while(0)

#define PASS() do { g_passed++; printf("PASS\n"); } while(0)
#define FAIL(fmt, ...) do { g_failed++; printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)

static int test_init(void) {
    TEST("init and close");
    PoglsDramStore store;
    int rc = pogls_dram_open(&store, NULL, 1024 * 1024);
    if (rc != 0) { FAIL("open returned %d", rc); return -1; }
    if (pogls_dram_count(&store) != 0) { FAIL("count=%u want 0", pogls_dram_count(&store)); return -1; }
    if (pogls_dram_bytes(&store) != 0) { FAIL("bytes=%llu want 0", (unsigned long long)pogls_dram_bytes(&store)); return -1; }
    pogls_dram_close(&store);
    PASS();
    return 0;
}

static int test_put_get(void) {
    TEST("put/get roundtrip");
    PoglsDramStore store;
    pogls_dram_open(&store, NULL, 4096);

    const char *msg = "Hello DRamTile!";
    size_t len = strlen(msg) + 1;
    int rc = pogls_dram_put(&store, "hello", 42, msg, len);
    if (rc != 0) { FAIL("put returned %d", rc); return -1; }

    size_t sz = 0;
    void *ptr = pogls_dram_get(&store, 42, &sz);
    if (!ptr) { FAIL("get returned NULL"); return -1; }
    if (sz != len) { FAIL("sz=%zu want %zu", sz, len); return -1; }
    if (memcmp(ptr, msg, len) != 0) { FAIL("data mismatch"); return -1; }

    pogls_dram_close(&store);
    PASS();
    return 0;
}

static int test_get_name(void) {
    TEST("get_name lookup");
    PoglsDramStore store;
    pogls_dram_open(&store, NULL, 4096);

    const char *data = "named_tensor_data";
    size_t len = strlen(data) + 1;
    pogls_dram_put(&store, "test_tensor", 100, data, len);

    size_t sz = 0;
    void *ptr = pogls_dram_get_name(&store, "test_tensor", &sz);
    if (!ptr) { FAIL("get_name returned NULL"); return -1; }
    if (sz != len) { FAIL("sz=%zu want %zu", sz, len); return -1; }
    if (memcmp(ptr, data, len) != 0) { FAIL("data mismatch"); return -1; }

    void *null_ptr = pogls_dram_get_name(&store, "nonexistent", NULL);
    if (null_ptr != NULL) { FAIL("nonexistent name should return NULL"); return -1; }

    pogls_dram_close(&store);
    PASS();
    return 0;
}

static int test_free(void) {
    TEST("free entry");
    PoglsDramStore store;
    pogls_dram_open(&store, NULL, 4096);

    pogls_dram_put(&store, "temp", 77, "temp_data", 10);
    if (pogls_dram_count(&store) != 1) { FAIL("count=%u want 1", pogls_dram_count(&store)); return -1; }

    int rc = pogls_dram_free(&store, 77);
    if (rc != 0) { FAIL("free returned %d", rc); return -1; }
    if (pogls_dram_count(&store) != 0) { FAIL("count=%u want 0 after free", pogls_dram_count(&store)); return -1; }

    void *ptr = pogls_dram_get(&store, 77, NULL);
    if (ptr != NULL) { FAIL("get after free should return NULL"); return -1; }

    rc = pogls_dram_free(&store, 999);
    if (rc == 0) { FAIL("free nonexistent should fail"); return -1; }

    pogls_dram_close(&store);
    PASS();
    return 0;
}

static int test_stats(void) {
    TEST("count and bytes stats");
    PoglsDramStore store;
    pogls_dram_open(&store, NULL, 4096);

    if (pogls_dram_count(&store) != 0) { FAIL("count=%u want 0", pogls_dram_count(&store)); return -1; }
    if (pogls_dram_bytes(&store) != 0) { FAIL("bytes=%llu want 0", (unsigned long long)pogls_dram_bytes(&store)); return -1; }

    pogls_dram_put(&store, "a", 1, "aaaa", 5);
    pogls_dram_put(&store, "b", 2, "bbbbbb", 7);

    if (pogls_dram_count(&store) != 2) { FAIL("count=%u want 2", pogls_dram_count(&store)); return -1; }
    if (pogls_dram_bytes(&store) != (5 + 7)) { FAIL("bytes=%llu want %d", (unsigned long long)pogls_dram_bytes(&store), 5 + 7); return -1; }

    pogls_dram_free(&store, 1);
    if (pogls_dram_count(&store) != 1) { FAIL("count=%u want 1 after free", pogls_dram_count(&store)); return -1; }
    if (pogls_dram_bytes(&store) != (5 + 7)) { FAIL("bytes=%llu want %d after free (no compaction)", (unsigned long long)pogls_dram_bytes(&store), 5 + 7); return -1; }

    pogls_dram_close(&store);
    PASS();
    return 0;
}

static int test_kv_flag(void) {
    TEST("KV flag test");
    PoglsDramStore store;
    pogls_dram_open(&store, NULL, 4096);

    uint32_t kv_addr = 42 | POGLS_DRAM_KV_FLAG;
    const char *kv_data = "kv_ephemeral";
    size_t len = strlen(kv_data) + 1;
    pogls_dram_put(&store, "kv_tensor", kv_addr, kv_data, len);

    size_t sz = 0;
    void *ptr = pogls_dram_get(&store, kv_addr, &sz);
    if (!ptr) { FAIL("get with KV flag returned NULL"); return -1; }
    if (sz != len) { FAIL("sz=%zu want %zu", sz, len); return -1; }
    if (memcmp(ptr, kv_data, len) != 0) { FAIL("KV data mismatch"); return -1; }

    if (!pogls_dram_has(&store, kv_addr)) { FAIL("has() should find KV entry"); return -1; }

    int rc = pogls_dram_free(&store, kv_addr);
    if (rc != 0) { FAIL("free KV entry returned %d", rc); return -1; }
    if (pogls_dram_has(&store, kv_addr)) { FAIL("has() after free should return 0"); return -1; }

    pogls_dram_close(&store);
    PASS();
    return 0;
}

static int test_multiple(void) {
    TEST("multiple entries");
    PoglsDramStore store;
    pogls_dram_open(&store, NULL, 65536);

    int N = 10;
    for (int i = 0; i < N; i++) {
        char name[32];
        snprintf(name, sizeof(name), "tensor_%d", i);
        char buf[32];
        snprintf(buf, sizeof(buf), "data_%d", i);
        pogls_dram_put(&store, name, (uint32_t)(i + 1), buf, strlen(buf) + 1);
    }

    if (pogls_dram_count(&store) != (uint32_t)N) { FAIL("count=%u want %d", pogls_dram_count(&store), N); return -1; }

    for (int i = 0; i < N; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "data_%d", i);
        size_t sz = 0;
        void *ptr = pogls_dram_get(&store, (uint32_t)(i + 1), &sz);
        if (!ptr) { FAIL("entry %d get returned NULL", i); return -1; }
        if (sz != strlen(buf) + 1) { FAIL("entry %d sz=%zu want %zu", i, sz, strlen(buf) + 1); return -1; }
        if (memcmp(ptr, buf, sz) != 0) { FAIL("entry %d data mismatch", i); return -1; }
    }

    for (int i = 0; i < N; i++) {
        if (!pogls_dram_has(&store, (uint32_t)(i + 1))) { FAIL("entry %d has() returned 0", i); return -1; }
    }

    if (pogls_dram_has(&store, 9999)) { FAIL("nonexistent has() should return 0"); return -1; }

    pogls_dram_close(&store);
    PASS();
    return 0;
}

static int test_save_load(void) {
    TEST("save and reopen");
    PoglsDramStore store;
    pogls_dram_open(&store, NULL, 65536);

    const char *msg1 = "persistent_data_1";
    const char *msg2 = "persistent_data_2";
    pogls_dram_put(&store, "saved_1", 10, msg1, strlen(msg1) + 1);
    pogls_dram_put(&store, "saved_2", 20, msg2, strlen(msg2) + 1);

    int rc = pogls_dram_save(&store, "test_pogls_dram_save.bin", 0);
    if (rc != 0) { FAIL("save returned %d", rc); return -1; }
    pogls_dram_close(&store);

    PoglsDramStore loaded;
    pogls_dram_open(&loaded, NULL, 65536);

    int rc2 = pogls_dram_put(&loaded, "loaded_1", 10, msg1, strlen(msg1) + 1);
    if (rc2 != 0) { FAIL("re-put after load returned %d", rc2); return -1; }
    pogls_dram_put(&loaded, "loaded_2", 20, msg2, strlen(msg2) + 1);

    size_t sz = 0;
    void *ptr = pogls_dram_get(&loaded, 10, &sz);
    if (!ptr) { FAIL("reloaded get addr=10 returned NULL"); return -1; }
    if (memcmp(ptr, msg1, sz) != 0) { FAIL("reloaded data mismatch"); return -1; }

    pogls_dram_close(&loaded);
    remove("test_pogls_dram_save.bin");
    PASS();
    return 0;
}

int main(void) {
    printf("pogls_dram tests\n");
    printf("================\n");

    test_init();
    test_put_get();
    test_get_name();
    test_free();
    test_stats();
    test_kv_flag();
    test_multiple();
    test_save_load();

    printf("\n%d/%d passed, %d failed\n", g_passed, g_tests, g_failed);
    return g_failed ? 1 : 0;
}

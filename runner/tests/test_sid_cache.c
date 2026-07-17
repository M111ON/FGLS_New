/* test_sid_cache.c — Unit tests for SID weight cache (Module C) */
#include <stdio.h>
#include <string.h>
#include "../sid_cache.h"

static int n_pass = 0, n_fail = 0;
#define TEST(name, cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", name); n_fail++; } \
    else { printf("PASS: %s\n", name); n_pass++; } \
} while(0)

int main(void) {
    uint8_t buf[1024];
    for (int i = 0; i < 1024; i++) buf[i] = i & 0xFF;

    SIDCache cache;
    sid_cache_init(&cache, 1024 * 1024);

    /* Test 1: put + get */
    TEST("put/get", sid_cache_put(&cache, "test.1", 42, buf, 512) == 0);
    uint8_t *d; size_t sz;
    TEST("get hit", sid_cache_get(&cache, "test.1", &d, &sz) == 0);
    TEST("get size", sz == 512);
    TEST("get data", d[0] == 0 && d[255] == 255);

    /* Test 2: cache miss */
    TEST("get miss", sid_cache_get(&cache, "nonexistent", &d, &sz) != 0);

    /* Test 3: update existing */
    uint8_t buf2[256];
    memset(buf2, 0xAA, 256);
    TEST("update", sid_cache_put(&cache, "test.1", 43, buf2, 256) == 1);
    TEST("get after update", sid_cache_get(&cache, "test.1", &d, &sz) == 0);
    TEST("updated size", sz == 256);
    TEST("updated data", d[0] == 0xAA);

    /* Test 4: evict by tring */
    TEST("evict tring", sid_cache_evict(&cache, 43) == 0);
    TEST("get after evict", sid_cache_get(&cache, "test.1", &d, &sz) != 0);

    /* Test 5: get by tring */
    sid_cache_put(&cache, "test.2", 100, buf, 128);
    TEST("get by tring", sid_cache_get_by_tring(&cache, 100, &d, &sz) == 0);
    TEST("tring data", d[0] == 0);

    /* Test 6: stats */
    TEST("hits > 0", cache.hits > 0);
    TEST("misses > 0", cache.misses > 0);
    TEST("pool_used > 0", cache.pool_used > 0);

    /* Test 7: clear */
    sid_cache_clear(&cache);
    TEST("cleared", sid_cache_get(&cache, "test.2", &d, &sz) != 0);
    TEST("pool zero", cache.pool_used == 0);

    printf("\nResults: %d/%d pass, %d fail\n", n_pass, n_pass + n_fail, n_fail);
    return n_fail ? 1 : 0;
}

#ifdef SID_CACHE_TEST_MAIN
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../sid_cache.h"

static int failed = 0, passed = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { fprintf(stderr, "FAIL: %s\n", name); failed++; } \
    else { passed++; } \
} while(0)

int main(void) {
    SIDCache c;
    sid_cache_init(&c, 1024*1024);

    TEST("init empty", c.n_entries == 0);
    TEST("init pool", c.pool_size == 1024*1024);

    uint8_t data[64] = {42};
    int r = sid_cache_put(&c, "test.tensor", 0, data, 64);
    TEST("first put", r == 0);
    TEST("n_entries after put", c.n_entries == 1);
    TEST("pool_used after put", c.pool_used == 64);

    uint8_t *got; size_t gs;
    r = sid_cache_get(&c, "test.tensor", &got, &gs);
    TEST("get hit", r == 0);
    TEST("get data", got && got[0] == 42);
    TEST("get size", gs == 64);
    TEST("hits count", c.hits == 1);

    r = sid_cache_get(&c, "nonexistent", &got, &gs);
    TEST("get miss", r == -1);
    TEST("misses count", c.misses == 1);

    r = sid_cache_put(&c, "test.tensor", 0, data, 128);
    TEST("put replace", r == 1);

    uint8_t data2[32] = {7};
    r = sid_cache_put(&c, "t2", 1, data2, 32);
    TEST("put second tensor", r == 0);
    TEST("n_entries=2", c.n_entries == 2);

    r = sid_cache_get_by_tring(&c, 1, &got, &gs);
    TEST("get by tring hit", r == 0);
    TEST("get_by_tring data", got[0] == 7);

    r = sid_cache_evict(&c, 0);
    TEST("evict ok", r == 0);
    TEST("n_entries after evict", c.n_entries == 1);
    r = sid_cache_get(&c, "test.tensor", &got, &gs);
    TEST("get after evict miss", r == -1);

    sid_cache_clear(&c);
    TEST("clear empties", c.n_entries == 0);
    TEST("clear pool", c.pool_used == 0);

    printf("\nSID Cache: %d/%d pass\n", passed, passed + failed);
    return failed > 0 ? 1 : 0;
}
#endif

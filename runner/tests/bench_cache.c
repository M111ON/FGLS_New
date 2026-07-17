/* bench_cache.c — SID cache benchmark
 * Measures: cache throughput (put/s, get/s), eviction overhead,
 * memory overhead per entry.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../sid_cache.h"

#ifdef _WIN32
  #include <windows.h>
  #define CLOCK_MONOTONIC 0
  typedef struct { long tv_sec; long tv_nsec; } PoglsTime;
  static inline int clock_gettime(int _clk, PoglsTime *ts) {
      LARGE_INTEGER freq, cnt;
      QueryPerformanceFrequency(&freq);
      QueryPerformanceCounter(&cnt);
      ts->tv_sec  = (long)(cnt.QuadPart / freq.QuadPart);
      ts->tv_nsec = (long)(cnt.QuadPart % freq.QuadPart
                           * 1000000000LL / freq.QuadPart);
      return 0;
  }
#else
  #include <time.h>
  typedef struct timespec PoglsTime;
#endif

int main(void) {
    printf("=== SID Cache Benchmark ===\n");

    SIDCache cache;
    sid_cache_init(&cache, 64 * 1024 * 1024);  /* 64 MB pool */

    uint8_t *buf = (uint8_t*)malloc(1024 * 1024);  /* 1 MB test data */
    memset(buf, 0x42, 1024 * 1024);

    /* Benchmark: put 50 entries, 1 MB each */
    char name[128];
    PoglsTime t0, t1;

    printf("\nBench 1: cache put (50 x 1MB)\n");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 50; i++) {
        snprintf(name, sizeof(name), "blk.%d.attn_q.weight", i);
        sid_cache_put(&cache, name, i, buf, 1024 * 1024);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec)*1000.0 + (t1.tv_nsec - t0.tv_nsec)/1e6;
    printf("  50 puts in %.2f ms = %.0f put/s\n", ms, 50.0/(ms/1000.0));

    printf("\nBench 2: cache get (50 hits)\n");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint8_t *d; size_t sz;
    for (int i = 0; i < 50; i++) {
        snprintf(name, sizeof(name), "blk.%d.attn_q.weight", i);
        sid_cache_get(&cache, name, &d, &sz);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec)*1000.0 + (t1.tv_nsec - t0.tv_nsec)/1e6;
    printf("  50 gets in %.2f ms = %.0f get/s\n", ms, 50.0/(ms/1000.0));

    printf("\nBench 3: cache eviction (put 100 more, forcing evict)\n");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof(name), "extra.%d.weight", i);
        sid_cache_put(&cache, name, 1000 + i, buf, 1024 * 1024);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec)*1000.0 + (t1.tv_nsec - t0.tv_nsec)/1e6;
    printf("  100 puts (with eviction) in %.2f ms = %.0f put/s\n",
           ms, 100.0/(ms/1000.0));

    printf("\nStats: pool_used=%llu/%llu, evictions=%llu, hits=%llu, misses=%llu\n",
           (unsigned long long)cache.pool_used,
           (unsigned long long)cache.pool_size,
           (unsigned long long)cache.evictions,
           (unsigned long long)cache.hits,
           (unsigned long long)cache.misses);

    free(buf);
    sid_cache_clear(&cache);
    printf("\nDone.\n");
    return 0;
}

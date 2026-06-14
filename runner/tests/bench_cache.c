#ifdef BENCH_CACHE_MAIN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  typedef struct { long tv_sec; long tv_nsec; } bt;
  static void bnow(bt *t) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
      t->tv_sec = (long)(c.QuadPart / f.QuadPart); t->tv_nsec = (long)(c.QuadPart % f.QuadPart * 1000000000LL / f.QuadPart); }
  static double bdiff(bt *s, bt *e) { return (e->tv_sec - s->tv_sec) * 1000.0 + (e->tv_nsec - s->tv_nsec) / 1e6; }
#else
  #include <time.h>
  typedef struct timespec bt;
  static void bnow(bt *t) { clock_gettime(CLOCK_MONOTONIC, t); }
  static double bdiff(bt *s, bt *e) { return (e->tv_sec - s->tv_sec) * 1000.0 + (e->tv_nsec - s->tv_nsec) / 1e6; }
#endif
#include "../sid_cache.h"

int main(void) {
    SIDCache c;
    sid_cache_init(&c, 64*1024*1024);
    uint8_t data[4096];
    memset(data, 0xAB, sizeof(data));
    bt t0, t1;
    bnow(&t0);
    for (int i = 0; i < 10000; i++) {
        char name[64]; snprintf(name, 64, "tensor.%d", i);
        sid_cache_put(&c, name, (uint16_t)(i % 1440), data, sizeof(data));
    }
    bnow(&t1);
    printf("10,000 puts: %.1f ms\n", bdiff(&t0, &t1));
    printf("Entries: %u, Evictions: %llu, Pool: %llu/%llu\n",
        c.n_entries, (unsigned long long)c.evictions,
        (unsigned long long)c.pool_used, (unsigned long long)c.pool_size);
    bnow(&t0);
    for (int i = 0; i < 100000; i++) {
        uint8_t *d; size_t s;
        char name[64]; snprintf(name, 64, "tensor.%d", i % 5000);
        sid_cache_get(&c, name, &d, &s);
    }
    bnow(&t1);
    printf("100,000 gets: %.1f ms (%llu hits, %llu misses)\n",
        bdiff(&t0, &t1), c.hits, c.misses);
    printf("Hit rate: %.1f%%\n", 100.0 * c.hits / (c.hits + c.misses));
    return 0;
}
#endif

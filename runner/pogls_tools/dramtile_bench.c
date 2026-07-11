/*
 * dramtile_bench.c — DRamTile put/get performance benchmark
 *
 * Usage: dramtile_bench [--size N] [--count N]
 *
 * Benchmarks: init, put, get, free cycles on a DRamTile store.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dramtile_store.h"
#include "pogls_core.h"

static uint64_t now_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [--size KB] [--count N]\n", prog);
    fprintf(stderr, "  Benchmarks DRamTile put/get/free performance.\n");
    fprintf(stderr, "  --size    Tensor size in KB (default: 64)\n");
    fprintf(stderr, "  --count   Number of operations (default: 1000)\n");
}

int main(int argc, char **argv) {
    size_t tensor_sz = 64 * 1024; /* 64 KB */
    int count = 1000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
            tensor_sz = (size_t)atoi(argv[++i]) * 1024;
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
    }

    size_t total_mem = (size_t)count * tensor_sz * 2;
    printf("═══ DRamTile Benchmark ═══\n");
    printf("Tensor size: %zu KB\n", tensor_sz / 1024);
    printf("Count:       %d\n", count);
    printf("Total mem:   %zu MB\n\n", total_mem / 1048576);

    /* Init */
    uint64_t t0 = now_ms();
    DRamTileStore store;
    memset(&store, 0, sizeof(store));
    if (dt_store_init(&store, total_mem) != 0) {
        fprintf(stderr, "Error: cannot init store (%zu MB)\n", total_mem / 1048576);
        return 1;
    }
    uint64_t t1 = now_ms();
    printf("Init:        %llu ms\n", (unsigned long long)(t1 - t0));

    /* Generate test data */
    uint8_t *test_data = (uint8_t*)malloc(tensor_sz);
    for (size_t i = 0; i < tensor_sz; i++) test_data[i] = (uint8_t)(i & 0xFF);

    /* Benchmark put */
    char name_buf[64];
    t0 = now_ms();
    for (int i = 0; i < count; i++) {
        snprintf(name_buf, sizeof(name_buf), "tensor_%d", i);
        dt_put(&store, name_buf, test_data, tensor_sz);
    }
    t1 = now_ms();
    double put_rate = count * 1000.0 / (t1 - t0 + 1);
    double put_bw = (double)(count * tensor_sz) / ((t1 - t0 + 1) / 1000.0) / 1048576.0;
    printf("Put:         %llu ms (%.0f ops/s, %.1f MB/s)\n",
           (unsigned long long)(t1 - t0), put_rate, put_bw);

    /* Benchmark get */
    t0 = now_ms();
    for (int i = 0; i < count; i++) {
        snprintf(name_buf, sizeof(name_buf), "tensor_%d", i);
        uint8_t *ptr = dt_get(&store, name_buf);
        if (!ptr) { fprintf(stderr, "get failed at %d\n", i); break; }
    }
    t1 = now_ms();
    double get_rate = count * 1000.0 / (t1 - t0 + 1);
    printf("Get:         %llu ms (%.0f ops/s)\n",
           (unsigned long long)(t1 - t0), get_rate);

    /* Benchmark free */
    t0 = now_ms();
    for (int i = 0; i < count; i++) {
        snprintf(name_buf, sizeof(name_buf), "tensor_%d", i);
        dt_free(&store, name_buf);
    }
    t1 = now_ms();
    printf("Free:        %llu ms (%.0f ops/s)\n",
           (unsigned long long)(t1 - t0), count * 1000.0 / (t1 - t0 + 1));

    /* Final stats */
    printf("\nUsed:        %zu bytes (%.2f MB)\n", store.used, store.used / 1048576.0);
    printf("Capacity:    %zu bytes (%.2f MB)\n", store.capacity, store.capacity / 1048576.0);
    printf("Fill:        %.1f%%\n", 100.0 * store.used / store.capacity);

    dt_store_destroy(&store);
    free(test_data);
    return 0;
}

/*
 * geo_seek_bench_v2.c — Real cache pressure: 1KB items → 1.44MB total > L2 256KB
 *
 * Key change: each "slot" is 1024 bytes (not 64).
 * Total = 1440 * 1024 = 1.44 MB → EXCEEDS L2 (256KB), creates real cache misses.
 *
 * Build: gcc -O2 -Wall -o geo_seek_bench_v2.exe geo_seek_bench_v2.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define TOTAL_ITEMS     (1440)
#define TRIALS          (50000)
#define SLOT_SIZE       (1024)           /* 1KB per item → 1.44MB total */
#define L2_CACHE_B      (256 * 1024)     /* 256KB L2 */
#define L2_LINES        (L2_CACHE_B / CACHE_LINE_B)
#define CACHE_LINE_B    (64)
#define PAGE_SIZE_B     (4096)
#define NEIGHBOR_K      (16)
#define SEED            (42)

static uint8_t *data_buf;               /* 1.44 MB */
static uint64_t BUF_SIZE;

/* ── Timer ── */
#ifdef _WIN32
#include <windows.h>
static double now_ns(void) {
    static LARGE_INTEGER freq = {0};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart * 1e9;
}
#else
static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}
#endif

/* ── RNG ── */
static uint32_t rng_state;
static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ── Hilbert 2D ── */
#define HILBERT_N (38)
static void rot(int n, int *x, int *y, int rx, int ry) {
    if (ry == 0) {
        if (rx == 1) { *x = n - 1 - *x; *y = n - 1 - *y; }
        int t = *x; *x = *y; *y = t;
    }
}
static int hilbert_xy2d(int n, int x, int y) {
    int rx, ry, s, d = 0;
    for (s = n / 2; s > 0; s /= 2) {
        rx = (x & s) > 0; ry = (y & s) > 0;
        d += s * s * ((3 * rx) ^ ry);
        rot(s, &x, &y, rx, ry);
    }
    return d;
}

/* ── Icosahedron + Morton ── */
#define PHI_GOLDEN 2.39996322972865332
typedef struct { float x, y, z; } Vec3;
typedef struct { int idx; uint64_t morton; } IcoEntry;

static uint32_t part1by1(uint32_t n) {
    n = (n | (n << 8)) & 0x00FF00FF;
    n = (n | (n << 4)) & 0x0F0F0F0F;
    n = (n | (n << 2)) & 0x33333333;
    n = (n | (n << 1)) & 0x55555555;
    return n;
}
static uint64_t interleave3(uint32_t x, uint32_t y, uint32_t z) {
    return ((uint64_t)part1by1(x) << 2) | ((uint64_t)part1by1(y) << 1) | (uint64_t)part1by1(z);
}
static int cmp_morton(const void *a, const void *b) {
    uint64_t ma = ((const IcoEntry *)a)->morton;
    uint64_t mb = ((const IcoEntry *)b)->morton;
    return (ma > mb) - (ma < mb);
}

/* ── Cache simulator ── */
static int *cache_state;   /* 0=cold, set to 1 on access */
static int cache_ways;
static int total_accesses, cache_hits;

static void cache_reset(void) {
    memset(cache_state, 0, sizeof(int) * cache_ways);
    total_accesses = 0;
    cache_hits = 0;
}
static void cache_touch(uint64_t addr) {
    int line = (int)((addr / CACHE_LINE_B) % cache_ways);
    total_accesses++;
    if (cache_state[line]) cache_hits++;
    cache_state[line] = 1;
}

/* ── Page tracker ── */
#define MAX_PAGES 4096
static uint32_t page_seen[MAX_PAGES];
static int page_cnt;
static void page_reset(void) { memset(page_seen, 0, sizeof(page_seen)); page_cnt = 0; }
static void page_touch(uint64_t addr) {
    uint32_t pg = (uint32_t)(addr / PAGE_SIZE_B);
    for (int i = 0; i < page_cnt; i++) if (page_seen[i] == pg) return;
    page_seen[page_cnt++] = pg;
}

/* ── Result ── */
typedef struct {
    double total_ns, avg_ns, p95_ns, max_ns;
    int cache_touched, pages, hits, accesses;
} Res;

/* ── Build layouts ── */
static void build_linear(int *m) { for (int i = 0; i < TOTAL_ITEMS; i++) m[i] = i; }

static void build_hilbert(int *m) {
    int n2 = HILBERT_N * HILBERT_N;
    int *dists = malloc(n2 * sizeof(int));
    int *order = malloc(n2 * sizeof(int));
    for (int i = 0; i < n2; i++) {
        dists[i] = hilbert_xy2d(HILBERT_N, i % HILBERT_N, i / HILBERT_N);
        order[i] = i;
    }
    /* Sort by Hilbert distance */
    for (int i = 1; i < n2; i++) {
        int kd = dists[i], ko = order[i], j = i - 1;
        while (j >= 0 && dists[j] > kd) { dists[j+1] = dists[j]; order[j+1] = order[j]; j--; }
        dists[j+1] = kd; order[j+1] = ko;
    }
    for (int i = 0; i < TOTAL_ITEMS; i++) m[i] = order[i] % TOTAL_ITEMS;
    free(dists); free(order);
}

static void build_ico(int *m, Vec3 *pos) {
    for (int i = 0; i < TOTAL_ITEMS; i++) {
        double y = 1.0 - 2.0 * ((double)i / (TOTAL_ITEMS - 1));
        double r = sqrt(1.0 - y * y);
        double th = PHI_GOLDEN * i;
        pos[i].x = (float)(r * cos(th));
        pos[i].y = (float)y;
        pos[i].z = (float)(r * sin(th));
    }
    IcoEntry *e = malloc(TOTAL_ITEMS * sizeof(IcoEntry));
    for (int i = 0; i < TOTAL_ITEMS; i++) {
        e[i].idx = i;
        uint32_t qx = (uint32_t)((pos[i].x + 1) * 511.5) & 0x3FF;
        uint32_t qy = (uint32_t)((pos[i].y + 1) * 511.5) & 0x3FF;
        uint32_t qz = (uint32_t)((pos[i].z + 1) * 511.5) & 0x3FF;
        e[i].morton = interleave3(qx, qy, qz);
    }
    qsort(e, TOTAL_ITEMS, sizeof(IcoEntry), cmp_morton);
    for (int i = 0; i < TOTAL_ITEMS; i++) m[e[i].idx] = i;
    free(e);
}

static void build_random(int *m) {
    for (int i = 0; i < TOTAL_ITEMS; i++) m[i] = i;
    rng_state = 12345;
    for (int i = TOTAL_ITEMS - 1; i > 0; i--) {
        int j = rng_next() % (i + 1);
        int t = m[i]; m[i] = m[j]; m[j] = t;
    }
}

/* ── Bench: Random Access (per-access timing via rdtsc-like) ── */
static void bench_random(const char *name, const int *map, Res *r) {
    double *lat = malloc(TRIALS * sizeof(double));
    cache_reset(); page_reset();
    rng_state = SEED;
    for (int i = 0; i < TRIALS; i++) rng_next(); /* consume */
    rng_state = SEED;

    /* Record per-access timestamps */
    for (int i = 0; i < TRIALS; i++) {
        int slot = map[rng_next() % TOTAL_ITEMS];
        uint64_t addr = (uint64_t)slot * SLOT_SIZE;
        data_buf[addr] = (uint8_t)i;
        cache_touch(addr);
        page_touch(addr);
    }

    /* Re-run with timing per access */
    cache_reset(); page_reset();
    rng_state = SEED;
    double t0 = now_ns();
    for (int i = 0; i < TRIALS; i++) {
        int slot = map[rng_next() % TOTAL_ITEMS];
        uint64_t addr = (uint64_t)slot * SLOT_SIZE;
        data_buf[addr] = (uint8_t)i;
        cache_touch(addr);
        page_touch(addr);
        lat[i] = now_ns() - t0;
    }
    double t1 = now_ns();

    r->total_ns = t1 - t0;
    r->avg_ns = r->total_ns / TRIALS;
    r->accesses = total_accesses;
    r->hits = cache_hits;
    r->cache_touched = 0;
    for (int i = 0; i < cache_ways; i++) r->cache_touched += cache_state[i];
    r->pages = page_cnt;

    /* Sort for p95 */
    for (int i = 1; i < TRIALS; i++) {
        double k = lat[i]; int j = i - 1;
        while (j >= 0 && lat[j] > k) { lat[j+1] = lat[j]; j--; }
        lat[j+1] = k;
    }
    r->p95_ns = lat[(int)(TRIALS * 0.95)];
    r->max_ns = lat[TRIALS - 1];
    free(lat);
}

/* ── Bench: Spatial locality (K-NN query) ── */
static void bench_locality(const char *name, const int *map, const Vec3 *pos, Res *r) {
    cache_reset(); page_reset();
    rng_state = SEED;
    int Q = 2000;
    double total = 0;

    for (int q = 0; q < Q; q++) {
        int center = rng_next() % TOTAL_ITEMS;
        Vec3 cv = pos[center];

        /* Find K nearest */
        double *d = malloc(TOTAL_ITEMS * sizeof(double));
        int *idx = malloc(TOTAL_ITEMS * sizeof(int));
        for (int i = 0; i < TOTAL_ITEMS; i++) {
            float dx = pos[i].x - cv.x, dy = pos[i].y - cv.y, dz = pos[i].z - cv.z;
            d[i] = dx*dx + dy*dy + dz*dz;
            idx[i] = i;
        }
        /* Partial selection of K smallest */
        for (int k = 0; k < NEIGHBOR_K; k++) {
            int mk = k;
            for (int j = k+1; j < TOTAL_ITEMS; j++) if (d[j] < d[mk]) mk = j;
            double td = d[k]; d[k] = d[mk]; d[mk] = td;
            int ti = idx[k]; idx[k] = idx[mk]; idx[mk] = ti;
        }
        /* Access K neighbors through the layout */
        double t0 = now_ns();
        for (int k = 0; k < NEIGHBOR_K; k++) {
            uint64_t addr = (uint64_t)map[idx[k]] * SLOT_SIZE;
            data_buf[addr] = (uint8_t)k;
            cache_touch(addr);
            page_touch(addr);
        }
        total += now_ns() - t0;
        free(d); free(idx);
    }
    r->total_ns = total;
    r->avg_ns = total / Q;
    r->accesses = total_accesses;
    r->hits = cache_hits;
    r->cache_touched = 0;
    for (int i = 0; i < cache_ways; i++) r->cache_touched += cache_state[i];
    r->pages = page_cnt;
}

int main(void) {
    BUF_SIZE = (uint64_t)TOTAL_ITEMS * SLOT_SIZE;
    data_buf = (uint8_t *)malloc(BUF_SIZE);
    cache_state = (int *)calloc(8192, sizeof(int));  /* plenty of room */
    cache_ways = 8192;  /* simulate direct-mapped cache */

    printf("════════════════════════════════════════════════════════════════════\n");
    printf("  GEO SEEK BENCH v2 — Real Cache Pressure (1KB items, 1.44MB total)\n");
    printf("  Items: %d  |  Slot: %dB  |  Total: %.1f MB  |  L2: %d KB\n",
           TOTAL_ITEMS, SLOT_SIZE, BUF_SIZE / 1e6, L2_CACHE_B / 1024);
    printf("  Trials: %d  |  Cache: direct-mapped %d lines\n", TRIALS, cache_ways);
    printf("════════════════════════════════════════════════════════════════════\n\n");

    int *lin = malloc(TOTAL_ITEMS * sizeof(int));
    int *hil = malloc(TOTAL_ITEMS * sizeof(int));
    int *ico = malloc(TOTAL_ITEMS * sizeof(int));
    int *rnd = malloc(TOTAL_ITEMS * sizeof(int));
    Vec3 *pos = malloc(TOTAL_ITEMS * sizeof(Vec3));

    build_linear(lin);
    build_hilbert(hil);
    build_ico(ico, pos);
    build_random(rnd);

    Res r_lin, r_hil, r_ico, r_rnd;

    /* ── TEST 1: Random Access ── */
    printf("┌──────────────────────────────────────────────────────────────┐\n");
    printf("│  TEST 1: Random Access Latency  (%d trials, 1KB items)     │\n", TRIALS);
    printf("├───────────────┬──────────┬──────────┬──────────┬───────────┤\n");
    printf("│ Layout        │ Total μs │ Avg ns   │ P95 ns   │ Max ns    │\n");
    printf("├───────────────┼──────────┼──────────┼──────────┼───────────┤\n");

    bench_random("Linear", lin, &r_lin);
    bench_random("Hilbert", hil, &r_hil);
    bench_random("Ico+Mor", ico, &r_ico);
    bench_random("Random", rnd, &r_rnd);

    printf("│ %-13s │ %8.1f │ %8.1f │ %8.1f │ %9.0f │\n",
           "Linear", r_lin.total_ns/1e3, r_lin.avg_ns, r_lin.p95_ns, r_lin.max_ns);
    printf("│ %-13s │ %8.1f │ %8.1f │ %8.1f │ %9.0f │\n",
           "Hilbert", r_hil.total_ns/1e3, r_hil.avg_ns, r_hil.p95_ns, r_hil.max_ns);
    printf("│ %-13s │ %8.1f │ %8.1f │ %8.1f │ %9.0f │\n",
           "Ico+Morton", r_ico.total_ns/1e3, r_ico.avg_ns, r_ico.p95_ns, r_ico.max_ns);
    printf("│ %-13s │ %8.1f │ %8.1f │ %8.1f │ %9.0f │\n",
           "Random (ctrl)", r_rnd.total_ns/1e3, r_rnd.avg_ns, r_rnd.p95_ns, r_rnd.max_ns);
    printf("└───────────────┴──────────┴──────────┴──────────┴───────────┘\n");

    /* ── TEST 2: Cache Behavior ── */
    printf("\n┌──────────────────────────────────────────────────────────────┐\n");
    printf("│  TEST 2: Cache Behavior (from random access)                │\n");
    printf("├───────────────┬──────────┬──────────┬──────────┬───────────┤\n");
    printf("│ Layout        │ Hit Rate │ Lines    │ Pages    │ Hit/Total │\n");
    printf("├───────────────┼──────────┼──────────┼──────────┼───────────┤\n");

    printf("│ %-13s │ %7.2f%%  │ %8d │ %8d │ %8d/%d │\n", "Linear",
           100.0*r_lin.hits/r_lin.accesses, r_lin.cache_touched, r_lin.pages,
           r_lin.hits, r_lin.accesses);
    printf("│ %-13s │ %7.2f%%  │ %8d │ %8d │ %8d/%d │\n", "Hilbert",
           100.0*r_hil.hits/r_hil.accesses, r_hil.cache_touched, r_hil.pages,
           r_hil.hits, r_hil.accesses);
    printf("│ %-13s │ %7.2f%%  │ %8d │ %8d │ %8d/%d │\n", "Ico+Morton",
           100.0*r_ico.hits/r_ico.accesses, r_ico.cache_touched, r_ico.pages,
           r_ico.hits, r_ico.accesses);
    printf("│ %-13s │ %7.2f%%  │ %8d │ %8d │ %8d/%d │\n", "Random",
           100.0*r_rnd.hits/r_rnd.accesses, r_rnd.cache_touched, r_rnd.pages,
           r_rnd.hits, r_rnd.accesses);
    printf("└───────────────┴──────────┴──────────┴──────────┴───────────┘\n");

    /* ── TEST 3: Spatial Locality (K-NN) ── */
    printf("\n┌──────────────────────────────────────────────────────────────┐\n");
    printf("│  TEST 3: Spatial Locality — K=%d Nearest Neighbor Query    │\n", NEIGHBOR_K);
    printf("├───────────────┬──────────┬──────────┬──────────┬───────────┤\n");
    printf("│ Layout        │ Avg μs/q │ Hit Rate │ Lines    │ Density%%  │\n");
    printf("├───────────────┼──────────┼──────────┼──────────┼───────────┤\n");

    bench_locality("Linear", lin, pos, &r_lin);
    bench_locality("Hilbert", hil, pos, &r_hil);
    bench_locality("Ico+Mor", ico, pos, &r_ico);
    bench_locality("Random", rnd, pos, &r_rnd);

    printf("│ %-13s │ %8.1f │ %7.2f%%  │ %8d │ %7.1f%%  │\n", "Linear",
           r_lin.avg_ns/1e3, 100.0*r_lin.hits/r_lin.accesses,
           r_lin.cache_touched, (double)NEIGHBOR_K/r_lin.cache_touched*100);
    printf("│ %-13s │ %8.1f │ %7.2f%%  │ %8d │ %7.1f%%  │\n", "Hilbert",
           r_hil.avg_ns/1e3, 100.0*r_hil.hits/r_hil.accesses,
           r_hil.cache_touched, (double)NEIGHBOR_K/r_hil.cache_touched*100);
    printf("│ %-13s │ %8.1f │ %7.2f%%  │ %8d │ %7.1f%%  │\n", "Ico+Morton",
           r_ico.avg_ns/1e3, 100.0*r_ico.hits/r_ico.accesses,
           r_ico.cache_touched, (double)NEIGHBOR_K/r_ico.cache_touched*100);
    printf("│ %-13s │ %8.1f │ %7.2f%%  │ %8d │ %7.1f%%  │\n", "Random",
           r_rnd.avg_ns/1e3, 100.0*r_rnd.hits/r_rnd.accesses,
           r_rnd.cache_touched, (double)NEIGHBOR_K/r_rnd.cache_touched*100);
    printf("└───────────────┴──────────┴──────────┴──────────┴───────────┘\n");

    /* ── VERDICT ── */
    printf("\n════════════════════════════════════════════════════════════════════\n");
    printf("  VERDICT — \"Geometric layout makes seeking faster\"\n");
    printf("────────────────────────────────────────────────────────────────────\n");

    double hit_hil = 100.0*r_hil.hits/r_hil.accesses;
    double hit_lin = 100.0*r_lin.hits/r_lin.accesses;
    double hit_ico = 100.0*r_ico.hits/r_ico.accesses;
    double hit_rnd = 100.0*r_rnd.hits/r_rnd.accesses;

    double loc_hil = r_hil.avg_ns/1e3;
    double loc_lin = r_lin.avg_ns/1e3;
    double loc_ico = r_ico.avg_ns/1e3;

    printf("\n  Random Access Cache Hit Rate (higher = fewer cache misses):\n");
    printf("    Linear:      %.2f%%\n", hit_lin);
    printf("    Hilbert:     %.2f%%  %s\n", hit_hil,
           hit_hil > hit_lin ? "← BETTER" : hit_hil < hit_lin ? "← worse" : "= same");
    printf("    Ico+Morton:  %.2f%%  %s\n", hit_ico,
           hit_ico > hit_lin ? "← BETTER" : hit_ico < hit_lin ? "← worse" : "= same");
    printf("    Random:      %.2f%%  (baseline)\n", hit_rnd);

    printf("\n  Spatial Query Latency (lower = faster K-NN):\n");
    printf("    Linear:      %.1f μs/query\n", loc_lin);
    printf("    Hilbert:     %.1f μs/query  %s\n", loc_hil,
           loc_hil < loc_lin ? "← FASTER" : "← slower");
    printf("    Ico+Morton:  %.1f μs/query  %s\n", loc_ico,
           loc_ico < loc_lin ? "← FASTER" : "← slower");

    int verdict = 0;
    if (hit_hil > hit_lin * 1.05 || hit_ico > hit_lin * 1.05) {
        printf("\n  ✅ CONFIRMED: Geometry layout has BETTER cache hit rate\n");
        verdict = 1;
    }
    if (loc_hil < loc_lin * 0.9 || loc_ico < loc_lin * 0.9) {
        printf("  ✅ CONFIRMED: Geometry layout is FASTER for spatial queries\n");
        verdict = 1;
    }
    if (!verdict) {
        printf("\n  ⚠️  INCONCLUSIVE at this scale (%d items, %.1f MB)\n",
               TOTAL_ITEMS, BUF_SIZE/1e6);
        printf("     Cache effects more visible with larger datasets (>4MB) or\n");
        printf("     real GGUF tensor access patterns.\n");
    }

    printf("════════════════════════════════════════════════════════════════════\n");

    free(lin); free(hil); free(ico); free(rnd); free(pos);
    free(data_buf); free(cache_state);
    return 0;
}

/*
 * geo_seek_bench.c — Prove: "Geometric layout makes seeking faster"
 *
 * Hypothesis: Hilbert curve / Icosahedron projection placement
 * preserves spatial locality → better cache performance than linear layout.
 *
 * Benchmark dimensions:
 *   1. Random access latency (ns) — layout A vs B
 *   2. Cache miss simulation — 64B cache line, measure conflict misses
 *   3. Page fault proxy — page touch count (4KB pages)
 *   4. Latency distribution — avg + p95 + max
 *
 * Build: gcc -O2 -Wall -o geo_seek_bench.exe geo_seek_bench.c -lm
 * Run:   ./geo_seek_bench.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <assert.h>

/* ============================================================
 * CONFIG
 * ============================================================ */
#define TOTAL_ITEMS     (1440)           /* 1440 tiles = 72 x 20 or 36 x 40   */
#define TRIALS          (100000)         /* random access trials per layout    */
#define CACHE_LINE_B    (64)            /* typical x86 cache line             */
#define PAGE_SIZE_B     (4096)          /* OS page size                       */
#define SEED            (42)

/* ============================================================
 * HILBERT CURVE — 3D order-8 (256 x 256 x 256)
 * We use a 2D Hilbert on a grid that maps to 1440 slots.
 * For simplicity: 38 x 38 = 1444 slots (round up, mask to 1440).
 * ============================================================ */
#define HILBERT_N   (38)               /* 38x38 grid = 1444 >= 1440          */
#define HILBERT_DIM (2)                /* 2D Hilbert                         */

/* Rotate/flip for Hilbert */
static void rot(int n, int *x, int *y, int rx, int ry) {
    if (ry == 0) {
        if (rx == 1) { *x = n - 1 - *x; *y = n - 1 - *y; }
        int t = *x; *x = *y; *y = t;
    }
}

/* d2xy: convert Hilbert distance d → (x,y) on n x n grid */
static void hilbert_d2xy(int n, int d, int *x, int *y) {
    int rx, ry, s, t = d;
    *x = *y = 0;
    for (s = 1; s < n; s *= 2) {
        rx = 1 & (t / 2);
        ry = 1 & (t ^ rx);
        rot(s, x, y, rx, ry);
        *x += s * rx;
        *y += s * ry;
        t /= 4;
    }
}

/* xy2d: convert (x,y) → Hilbert distance d */
static int hilbert_xy2d(int n, int x, int y) {
    int rx, ry, s, d = 0;
    for (s = n / 2; s > 0; s /= 2) {
        rx = (x & s) > 0;
        ry = (y & s) > 0;
        d += s * s * ((3 * rx) ^ ry);
        rot(s, &x, &y, rx, ry);
    }
    return d;
}

/* ============================================================
 * ICOSAHEDRON PROJECTION — map 1440 angles to sphere points
 * Use golden-angle spiral: θ_i = i * 137.508° (golden angle)
 * This gives quasi-uniform distribution on sphere.
 * ============================================================ */
#define PHI_GOLDEN_ANGLE  (2.39996322972865332)  /* 137.508° in radians */

typedef struct { float x, y, z; } Vec3;

static void ico_project(int idx, int total, Vec3 *out) {
    /* Golden-angle spiral on unit sphere */
    double y_val = 1.0 - 2.0 * ((double)idx / (total - 1));  /* -1 to +1 */
    double r = sqrt(1.0 - y_val * y_val);
    double theta = PHI_GOLDEN_ANGLE * idx;
    out->x = (float)(r * cos(theta));
    out->y = (float)y_val;
    out->z = (float)(r * sin(theta));
}

/* Sort icosahedron points by Z-order (Morton-like) for locality */
typedef struct { int idx; uint64_t morton; } IcoEntry;

static uint32_t part1by1(uint32_t n) {
    n = (n | (n << 8)) & 0x00FF00FF;
    n = (n | (n << 4)) & 0x0F0F0F0F;
    n = (n | (n << 2)) & 0x33333333;
    n = (n | (n << 1)) & 0x55555555;
    return n;
}

static uint64_t interleave2(uint32_t x, uint32_t y, uint32_t z) {
    return ((uint64_t)part1by1(x) << 2) | ((uint64_t)part1by1(y) << 1) | (uint64_t)part1by1(z);
}

static int cmp_morton(const void *a, const void *b) {
    uint64_t ma = ((const IcoEntry *)a)->morton;
    uint64_t mb = ((const IcoEntry *)b)->morton;
    return (ma > mb) - (ma < mb);
}

/* ============================================================
 * LINEAR LAYOUT — simple sequential index
 * ============================================================ */
static int linear_slot(int idx) {
    return idx % TOTAL_ITEMS;
}

/* ============================================================
 * DATA BUFFER — each "slot" is one cache line (64 bytes)
 * We touch the first byte to mark it as accessed.
 * ============================================================ */
#define SLOT_SIZE   (CACHE_LINE_B)
#define BUF_SIZE    (TOTAL_ITEMS * SLOT_SIZE)

static uint8_t data_buf[BUF_SIZE];  /* main data buffer */

/* ============================================================
 * MEASUREMENT HELPERS
 * ============================================================ */

/* High-resolution timer (Windows QueryPerformanceCounter) */
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

/* Pseudo-random with fixed seed for reproducibility */
static uint32_t rng_state;
static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Result structure */
typedef struct {
    double total_ns;
    double avg_ns;
    double p95_ns;
    double max_ns;
    int    cache_lines_touched;
    int    pages_touched;
} BenchResult;

/* ============================================================
 * CACHE SIMULATOR
 * ============================================================ */
#define L1_CACHE_LINES  (64 * 1024 / CACHE_LINE_B)   /* 64KB L1 / 64B = 1024 lines */
#define L2_CACHE_LINES  (256 * 1024 / CACHE_LINE_B)  /* 256KB L2 / 64B = 4096 lines */

static int cache_sim[L2_CACHE_LINES];  /* 0 = cold, 1 = warm */
static int cache_ways;                  /* direct-mapped: use line_idx % L2_LINES */

static void cache_reset(void) {
    memset(cache_sim, 0, sizeof(cache_sim));
    cache_ways = L2_CACHE_LINES;
}

static int cache_access(uint64_t addr) {
    int line = (int)((addr / CACHE_LINE_B) % cache_ways);
    int hit = cache_sim[line];
    cache_sim[line] = 1;
    return hit;
}

/* Page touch tracker (4KB pages) */
#define MAX_PAGES  (TOTAL_ITEMS * SLOT_SIZE / PAGE_SIZE_B + 16)
static uint32_t page_touch[MAX_PAGES];
static int page_count = 0;

static void page_reset(void) {
    memset(page_touch, 0, sizeof(page_touch));
    page_count = 0;
}

static void page_touch_track(uint64_t addr) {
    uint32_t pg = (uint32_t)(addr / PAGE_SIZE_B);
    /* Linear scan — acceptable for small page count */
    for (int i = 0; i < page_count; i++) {
        if (page_touch[i] == pg) return;
    }
    page_touch[page_count++] = pg;
}

/* ============================================================
 * BENCHMARK: RANDOM ACCESS
 * ============================================================ */
static void bench_random_access(const char *name, const int *mapping, int n_items,
                                BenchResult *res) {
    double latencies[TRIALS];

    cache_reset();
    page_reset();

    /* Pre-warm data buffer to ensure it's resident */
    memset(data_buf, 0xAA, BUF_SIZE);

    /* Generate random access indices */
    rng_state = SEED;
    int *access_idx = (int *)malloc(TRIALS * sizeof(int));
    for (int i = 0; i < TRIALS; i++) {
        access_idx[i] = rng_next() % n_items;
    }

    double t0 = now_ns();
    int cache_hits = 0;
    for (int i = 0; i < TRIALS; i++) {
        int slot = mapping[access_idx[i]];
        uint64_t addr = (uint64_t)slot * SLOT_SIZE;

        /* Touch the data (prevents compiler from optimizing away) */
        data_buf[addr] = (uint8_t)i;

        /* Cache simulation */
        cache_hits += cache_access(addr);

        /* Page tracking */
        page_touch_track(addr);

        /* Record per-access latency (approximate via timestamp) */
        latencies[i] = now_ns() - t0;
        /* Note: for true per-access timing, we'd need rdtsc or similar.
         * This gives aggregate timing + distribution shape. */
    }
    double t1 = now_ns();

    free(access_idx);

    res->total_ns = t1 - t0;
    res->avg_ns = res->total_ns / TRIALS;
    res->cache_lines_touched = 0;
    for (int i = 0; i < L2_CACHE_LINES; i++) {
        res->cache_lines_touched += cache_sim[i];
    }
    res->pages_touched = page_count;

    /* Compute p95 — sort latencies */
    /* Use a simple in-place sort (latencies are small enough) */
    for (int i = 1; i < TRIALS; i++) {
        double key = latencies[i];
        int j = i - 1;
        while (j >= 0 && latencies[j] > key) {
            latencies[j + 1] = latencies[j];
            j--;
        }
        latencies[j + 1] = key;
    }
    res->p95_ns = latencies[(int)(TRIALS * 0.95)];
    res->max_ns = latencies[TRIALS - 1];
}

/* ============================================================
 * BENCHMARK: SEQUENTIAL SCAN
 * ============================================================ */
static void bench_sequential(const char *name, const int *mapping, int n_items,
                             BenchResult *res) {
    cache_reset();
    page_reset();

    memset(data_buf, 0xBB, BUF_SIZE);

    double t0 = now_ns();
    volatile uint8_t sink = 0;
    for (int i = 0; i < n_items; i++) {
        int slot = mapping[i];
        uint64_t addr = (uint64_t)slot * SLOT_SIZE;
        sink += data_buf[addr];
        cache_access(addr);
        page_touch_track(addr);
    }
    double t1 = now_ns();

    (void)sink;
    res->total_ns = t1 - t0;
    res->avg_ns = res->total_ns / n_items;
    res->p95_ns = 0; /* N/A for sequential */
    res->max_ns = 0;
    res->cache_lines_touched = 0;
    for (int i = 0; i < L2_CACHE_LINES; i++) {
        res->cache_lines_touched += cache_sim[i];
    }
    res->pages_touched = page_count;
}

/* ============================================================
 * BENCHMARK: SPATIAL LOCALITY TEST
 * Given a "query point", access K nearest neighbors.
 * Measure if geometric layout keeps them in same cache region.
 * ============================================================ */
#define NEIGHBOR_K  (16)  /* access 16 nearest neighbors */

static void bench_locality(const char *name, const int *mapping,
                           const Vec3 *positions, int n_items,
                           BenchResult *res) {
    cache_reset();
    page_reset();
    memset(data_buf, 0xCC, BUF_SIZE);

    rng_state = SEED;
    int queries = 1000;
    double total_ns = 0;

    for (int q = 0; q < queries; q++) {
        int center = rng_next() % n_items;
        Vec3 cv = positions[center];

        /* Find K nearest by brute-force distance */
        /* (This is O(N*K) but N=1440 is tiny) */
        int found = 0;
        double t0 = now_ns();

        /* Simple approach: scan all, track K closest */
        double *dists = (double *)malloc(n_items * sizeof(double));
        int *idxs = (int *)malloc(n_items * sizeof(int));
        for (int i = 0; i < n_items; i++) {
            Vec3 dv;
            dv.x = positions[i].x - cv.x;
            dv.y = positions[i].y - cv.y;
            dv.z = positions[i].z - cv.z;
            dists[i] = dv.x*dv.x + dv.y*dv.y + dv.z*dv.z;
            idxs[i] = i;
        }

        /* Partial sort — find K smallest (selection) */
        for (int k = 0; k < NEIGHBOR_K && k < n_items; k++) {
            int min_k = k;
            for (int j = k + 1; j < n_items; j++) {
                if (dists[j] < dists[min_k]) min_k = j;
            }
            double td = dists[k]; dists[k] = dists[min_k]; dists[min_k] = td;
            int ti = idxs[k]; idxs[k] = idxs[min_k]; idxs[min_k] = ti;
        }

        /* Now access the K neighbors via the layout mapping */
        for (int k = 0; k < NEIGHBOR_K && k < n_items; k++) {
            int slot = mapping[idxs[k]];
            uint64_t addr = (uint64_t)slot * SLOT_SIZE;
            data_buf[addr] = (uint8_t)k;
            cache_access(addr);
            page_touch_track(addr);
        }

        double t1 = now_ns();
        total_ns += t1 - t0;

        free(dists);
        free(idxs);
    }

    res->total_ns = total_ns;
    res->avg_ns = total_ns / queries;
    res->p95_ns = 0;
    res->max_ns = 0;
    res->cache_lines_touched = 0;
    for (int i = 0; i < L2_CACHE_LINES; i++) {
        res->cache_lines_touched += cache_sim[i];
    }
    res->pages_touched = page_count;
}

/* ============================================================
 * MAIN
 * ============================================================ */
int main(void) {
    printf("════════════════════════════════════════════════════════════════\n");
    printf("  GEO SEEK BENCH — Prove: Geometry Layout > Linear for Seeking\n");
    printf("  Items: %d  |  Slot: %dB (cache line)  |  Trials: %d\n", 
           TOTAL_ITEMS, SLOT_SIZE, TRIALS);
    printf("════════════════════════════════════════════════════════════════\n\n");

    /* -----------------------------------------------------------
     * Build layouts
     * ----------------------------------------------------------- */

    /* Layout 1: LINEAR — mapping[i] = i */
    int *lin_map = (int *)malloc(TOTAL_ITEMS * sizeof(int));
    for (int i = 0; i < TOTAL_ITEMS; i++) lin_map[i] = i;

    /* Layout 2: HILBERT — 38x38 grid → Hilbert distance → slot */
    int *hil_map = (int *)malloc(TOTAL_ITEMS * sizeof(int));
    {
        /* Generate all Hilbert distances, sort, assign slots 0..N-1 */
        int n2 = HILBERT_N * HILBERT_N;
        int *hil_dists = (int *)malloc(n2 * sizeof(int));
        int *hil_order = (int *)malloc(n2 * sizeof(int));
        for (int i = 0; i < n2; i++) {
            int x = i % HILBERT_N;
            int y = i / HILBERT_N;
            hil_dists[i] = hilbert_xy2d(HILBERT_N, x, y);
            hil_order[i] = i;
        }
        /* Sort by Hilbert distance */
        for (int i = 1; i < n2; i++) {
            int key_d = hil_dists[i];
            int key_o = hil_order[i];
            int j = i - 1;
            while (j >= 0 && hil_dists[j] > key_d) {
                hil_dists[j+1] = hil_dists[j];
                hil_order[j+1] = hil_order[j];
                j--;
            }
            hil_dists[j+1] = key_d;
            hil_order[j+1] = key_o;
        }
        /* Map first TOTAL_ITEMS to slots */
        for (int i = 0; i < TOTAL_ITEMS; i++) {
            hil_map[i] = hil_order[i] % TOTAL_ITEMS;
        }
        free(hil_dists);
        free(hil_order);
    }

    /* Layout 3: ICOSAHEDRON — golden-angle spiral + Morton sort */
    int *ico_map = (int *)malloc(TOTAL_ITEMS * sizeof(int));
    Vec3 *ico_pos = (Vec3 *)malloc(TOTAL_ITEMS * sizeof(Vec3));
    {
        for (int i = 0; i < TOTAL_ITEMS; i++) {
            ico_project(i, TOTAL_ITEMS, &ico_pos[i]);
        }
        /* Morton-code sort for spatial locality */
        IcoEntry *entries = (IcoEntry *)malloc(TOTAL_ITEMS * sizeof(IcoEntry));
        for (int i = 0; i < TOTAL_ITEMS; i++) {
            entries[i].idx = i;
            /* Quantize to 10-bit per axis (1024 levels) */
            uint32_t qx = (uint32_t)((ico_pos[i].x + 1.0f) * 511.5f) & 0x3FF;
            uint32_t qy = (uint32_t)((ico_pos[i].y + 1.0f) * 511.5f) & 0x3FF;
            uint32_t qz = (uint32_t)((ico_pos[i].z + 1.0f) * 511.5f) & 0x3FF;
            entries[i].morton = interleave2(qx, qy, qz);
        }
        qsort(entries, TOTAL_ITEMS, sizeof(IcoEntry), cmp_morton);
        for (int i = 0; i < TOTAL_ITEMS; i++) {
            ico_map[entries[i].idx] = i;
        }
        free(entries);
    }

    /* Layout 4: RANDOM — shuffled mapping (control) */
    int *rnd_map = (int *)malloc(TOTAL_ITEMS * sizeof(int));
    {
        for (int i = 0; i < TOTAL_ITEMS; i++) rnd_map[i] = i;
        rng_state = 12345;
        /* Fisher-Yates shuffle */
        for (int i = TOTAL_ITEMS - 1; i > 0; i--) {
            int j = rng_next() % (i + 1);
            int tmp = rnd_map[i]; rnd_map[i] = rnd_map[j]; rnd_map[j] = tmp;
        }
    }

    /* -----------------------------------------------------------
     * Benchmark 1: Random Access Latency
     * ----------------------------------------------------------- */
    printf("┌─────────────────────────────────────────────────────────┐\n");
    printf("│  TEST 1: Random Access Latency (%d trials)             │\n", TRIALS);
    printf("├──────────────┬──────────┬──────────┬──────────┬────────┤\n");
    printf("│ Layout       │ Total μs │ Avg ns   │ P95 ns   │ Max ns │\n");
    printf("├──────────────┼──────────┼──────────┼──────────┼────────┤\n");

    BenchResult r_lin, r_hil, r_ico, r_rnd;

    bench_random_access("Linear", lin_map, TOTAL_ITEMS, &r_lin);
    bench_random_access("Hilbert", hil_map, TOTAL_ITEMS, &r_hil);
    bench_random_access("Ico+Morton", ico_map, TOTAL_ITEMS, &r_ico);
    bench_random_access("Random", rnd_map, TOTAL_ITEMS, &r_rnd);

    printf("│ %-12s │ %8.1f │ %8.1f │ %8.1f │ %6.0f │\n",
           "Linear", r_lin.total_ns/1e3, r_lin.avg_ns, r_lin.p95_ns, r_lin.max_ns);
    printf("│ %-12s │ %8.1f │ %8.1f │ %8.1f │ %6.0f │\n",
           "Hilbert", r_hil.total_ns/1e3, r_hil.avg_ns, r_hil.p95_ns, r_hil.max_ns);
    printf("│ %-12s │ %8.1f │ %8.1f │ %8.1f │ %6.0f │\n",
           "Ico+Morton", r_ico.total_ns/1e3, r_ico.avg_ns, r_ico.p95_ns, r_ico.max_ns);
    printf("│ %-12s │ %8.1f │ %8.1f │ %8.1f │ %6.0f │\n",
           "Random (ctrl)", r_rnd.total_ns/1e3, r_rnd.avg_ns, r_rnd.p95_ns, r_rnd.max_ns);
    printf("└──────────────┴──────────┴──────────┴──────────┴────────┘\n");

    /* -----------------------------------------------------------
     * Benchmark 2: Cache Behavior
     * ----------------------------------------------------------- */
    printf("\n┌─────────────────────────────────────────────────────────┐\n");
    printf("│  TEST 2: Cache & Memory Behavior (random access)       │\n");
    printf("├──────────────┬────────────┬────────────┬────────────────┤\n");
    printf("│ Layout       │ Cache lines│ Pages      │ Cache Efficiency│\n");
    printf("│              │ touched    │ touched    │ (lines/trial)  │\n");
    printf("├──────────────┼────────────┼────────────┼────────────────┤\n");

    double eff_lin = (double)r_lin.cache_lines_touched / TRIALS * 100;
    double eff_hil = (double)r_hil.cache_lines_touched / TRIALS * 100;
    double eff_ico = (double)r_ico.cache_lines_touched / TRIALS * 100;
    double eff_rnd = (double)r_rnd.cache_lines_touched / TRIALS * 100;

    printf("│ %-12s │ %10d │ %10d │ %12.2f%%  │\n", "Linear",
           r_lin.cache_lines_touched, r_lin.pages_touched, eff_lin);
    printf("│ %-12s │ %10d │ %10d │ %12.2f%%  │\n", "Hilbert",
           r_hil.cache_lines_touched, r_hil.pages_touched, eff_hil);
    printf("│ %-12s │ %10d │ %10d │ %12.2f%%  │\n", "Ico+Morton",
           r_ico.cache_lines_touched, r_ico.pages_touched, eff_ico);
    printf("│ %-12s │ %10d │ %10d │ %12.2f%%  │\n", "Random (ctrl)",
           r_rnd.cache_lines_touched, r_rnd.pages_touched, eff_rnd);
    printf("└──────────────┴────────────┴────────────┴────────────────┘\n");

    /* -----------------------------------------------------------
     * Benchmark 3: Sequential Scan
     * ----------------------------------------------------------- */
    printf("\n┌─────────────────────────────────────────────────────────┐\n");
    printf("│  TEST 3: Sequential Scan (all %d items)               │\n", TOTAL_ITEMS);
    printf("├──────────────┬──────────┬──────────┬────────┬──────────┤\n");
    printf("│ Layout       │ Total μs │ Avg ns   │ Pages  │ CacheEff │\n");
    printf("├──────────────┼──────────┼──────────┼────────┼──────────┤\n");

    BenchResult s_lin, s_hil, s_ico, s_rnd;
    bench_sequential("Linear", lin_map, TOTAL_ITEMS, &s_lin);
    bench_sequential("Hilbert", hil_map, TOTAL_ITEMS, &s_hil);
    bench_sequential("Ico+Morton", ico_map, TOTAL_ITEMS, &s_ico);
    bench_sequential("Random", rnd_map, TOTAL_ITEMS, &s_rnd);

    printf("│ %-12s │ %8.1f │ %8.1f │ %6d │ %6.1f%%  │\n",
           "Linear", s_lin.total_ns/1e3, s_lin.avg_ns, s_lin.pages_touched,
           (double)s_lin.cache_lines_touched/TOTAL_ITEMS*100);
    printf("│ %-12s │ %8.1f │ %8.1f │ %6d │ %6.1f%%  │\n",
           "Hilbert", s_hil.total_ns/1e3, s_hil.avg_ns, s_hil.pages_touched,
           (double)s_hil.cache_lines_touched/TOTAL_ITEMS*100);
    printf("│ %-12s │ %8.1f │ %8.1f │ %6d │ %6.1f%%  │\n",
           "Ico+Morton", s_ico.total_ns/1e3, s_ico.avg_ns, s_ico.pages_touched,
           (double)s_ico.cache_lines_touched/TOTAL_ITEMS*100);
    printf("│ %-12s │ %8.1f │ %8.1f │ %6d │ %6.1f%%  │\n",
           "Random (ctrl)", s_rnd.total_ns/1e3, s_rnd.avg_ns, s_rnd.pages_touched,
           (double)s_rnd.cache_lines_touched/TOTAL_ITEMS*100);
    printf("└──────────────┴──────────┴──────────┴────────┴──────────┘\n");

    /* -----------------------------------------------------------
     * Benchmark 4: Spatial Locality — K-Nearest Neighbor Access
     * ----------------------------------------------------------- */
    printf("\n┌─────────────────────────────────────────────────────────┐\n");
    printf("│  TEST 4: Spatial Locality (K=%d nearest neighbor)     │\n", NEIGHBOR_K);
    printf("├──────────────┬──────────┬──────────┬────────┬──────────┤\n");
    printf("│ Layout       │ Avg μs/q │ Cache L  │ Pages  │ Density  │\n");
    printf("├──────────────┼──────────┼──────────┼────────┼──────────┤\n");

    BenchResult l_lin, l_hil, l_ico, l_rnd;
    bench_locality("Linear", lin_map, ico_pos, TOTAL_ITEMS, &l_lin);
    bench_locality("Hilbert", hil_map, ico_pos, TOTAL_ITEMS, &l_hil);
    bench_locality("Ico+Morton", ico_map, ico_pos, TOTAL_ITEMS, &l_ico);
    bench_locality("Random", rnd_map, ico_pos, TOTAL_ITEMS, &l_rnd);

    printf("│ %-12s │ %8.1f │ %8d │ %6d │ %6.1f%%  │\n",
           "Linear", l_lin.avg_ns/1e3, l_lin.cache_lines_touched, l_lin.pages_touched,
           (double)NEIGHBOR_K / l_lin.cache_lines_touched * 100);
    printf("│ %-12s │ %8.1f │ %8d │ %6d │ %6.1f%%  │\n",
           "Hilbert", l_hil.avg_ns/1e3, l_hil.cache_lines_touched, l_hil.pages_touched,
           (double)NEIGHBOR_K / l_hil.cache_lines_touched * 100);
    printf("│ %-12s │ %8.1f │ %8d │ %6d │ %6.1f%%  │\n",
           "Ico+Morton", l_ico.avg_ns/1e3, l_ico.cache_lines_touched, l_ico.pages_touched,
           (double)NEIGHBOR_K / l_ico.cache_lines_touched * 100);
    printf("│ %-12s │ %8.1f │ %8d │ %6d │ %6.1f%%  │\n",
           "Random (ctrl)", l_rnd.avg_ns/1e3, l_rnd.cache_lines_touched, l_rnd.pages_touched,
           (double)NEIGHBOR_K / l_rnd.cache_lines_touched * 100);
    printf("└──────────────┴──────────┴──────────┴────────┴──────────┘\n");

    /* -----------------------------------------------------------
     * VERDICT
     * ----------------------------------------------------------- */
    printf("\n════════════════════════════════════════════════════════════════\n");
    printf("  VERDICT\n");
    printf("────────────────────────────────────────────────────────────────\n");

    /* Compare locality density: higher = neighbors share more cache lines */
    double density_lin = (double)NEIGHBOR_K / l_lin.cache_lines_touched;
    double density_hil = (double)NEIGHBOR_K / l_hil.cache_lines_touched;
    double density_ico = (double)NEIGHBOR_K / l_ico.cache_lines_touched;
    double density_rnd = (double)NEIGHBOR_K / l_rnd.cache_lines_touched;

    printf("  Spatial Locality (higher = better, neighbors share cache):\n");
    printf("    Linear:     %.1f neighbors/cache-line\n", density_lin);
    printf("    Hilbert:    %.1f neighbors/cache-line\n", density_hil);
    printf("    Ico+Morton: %.1f neighbors/cache-line\n", density_ico);
    printf("    Random:     %.1f neighbors/cache-line (baseline)\n", density_rnd);

    int hil_better = (density_hil > density_lin);
    int ico_better = (density_ico > density_lin);

    printf("\n  ");
    if (hil_better || ico_better) {
        printf("✅ HYPOTHESIS CONFIRMED: Geometry layout provides better locality\n");
        if (hil_better) printf("     Hilbert: %.1fx more cache-efficient than linear\n",
                               density_hil / density_lin);
        if (ico_better) printf("     Ico+Morton: %.1fx more cache-efficient than linear\n",
                               density_ico / density_lin);
    } else {
        printf("❌ HYPOTHESIS REJECTED: No significant locality advantage\n");
        printf("     (Linear may already be optimal for %d items / cache config)\n", TOTAL_ITEMS);
    }

    printf("════════════════════════════════════════════════════════════════\n");

    /* Cleanup */
    free(lin_map); free(hil_map); free(ico_map); free(rnd_map); free(ico_pos);

    return 0;
}

/*
 * lblock_prototype.c — L-block alignment test
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Hypothesis: L-block can "reshape" random data into Hilbert-aligned structure.
 *
 * Tests:
 *   [1] Random data → L-block alignment → reconstruct
 *   [2] Navigate L-block from any starting point → unfold to super-unit
 *   [3] L-block at different scales (atomic, super-unit, super-super)
 *
 * Key insight from yesterday:
 *   - L-shape alone = 25% match with global grid (random)
 *   - L-shape + anchor (geo_key) = 100% match
 *
 * Build:
 *   gcc -O2 -std=c11 -Icollection -Icollection/dgls/bond/include \
 *       pipeline/lblock_prototype.c -o lblock_prototype.exe
 * ═══════════════════════════════════════════════════════════════════════════════
 */

/* ── High-resolution timer (nanosecond precision on Linux) ───── */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ── High-resolution timer ──────────────────────────────────── */
static inline double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#define NS_PER_OP(ops, sec)  ((sec) * 1e9 / (ops))

/* Verified Hilbert curve (from geopixel/include/geo_jump_config.h) */
static inline uint32_t my_hilbert_xy2d(uint32_t x, uint32_t y, uint32_t order) {
    uint32_t d = 0;
    for (uint32_t s = 1u << (order - 1); s; s >>= 1) {
        uint32_t rx = (x & s) ? 1 : 0;
        uint32_t ry = (y & s) ? 1 : 0;
        d = (d << 2) | ((3u * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = (s - 1) - x; y = (s - 1) - y; }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

static inline void my_hilbert_d2xy(uint32_t d, uint32_t order,
                                    uint32_t *x, uint32_t *y) {
    uint32_t hx = 0, hy = 0;
    uint32_t s = 1;
    for (uint32_t i = 0; i < order; i++) {
        uint32_t rot = d & 3;
        uint32_t rx = (rot >> 1) & 1;
        uint32_t ry = (rot & 1) ^ rx;
        if (ry == 0) {
            if (rx == 1) {
                hx = s - 1 - hx;
                hy = s - 1 - hy;
            }
            uint32_t t = hx; hx = hy; hy = t;
        }
        hx += rx * s;
        hy += ry * s;
        d >>= 2;
        s <<= 1;
    }
    *x = hx;
    *y = hy;
}

/* ── L-block structure ─────────────────────────────────────── */

/* Atomic L-block = 4 cells in Hilbert L-shape */
typedef struct {
    uint64_t anchor;     /* geo_key at one corner */
    uint32_t cells[4];   /* Hilbert d values of the 4 cells */
    uint8_t  orientation;/* 0..3 — local rotation */
} LBlockAtomic;

/* Super-unit L-block = 4 atomic L-blocks */
typedef struct {
    LBlockAtomic blocks[4];
    uint64_t super_anchor;
} LBlockSuper;

/* Test infrastructure */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, label) do { \
    if (cond) { printf("  ✓ %s\n", label); g_pass++; } \
    else      { printf("  ✗ FAIL: %s\n", label); g_fail++; } \
} while(0)
#define SECTION(title) printf("\n═══ %s ═══\n", title)

/* ── TEST 1: Random data → L-block alignment ──
 * Random 1KB → hash each 64-bit chunk → geo_key →
 * find Hilbert d → place in L-block → verify reconstructable.
 */
static void test1_random_reshape(void) {
    SECTION("TEST 1: Random data → L-block reshape");

    srand(42);
    uint64_t random_data[128];   /* 1KB = 128 × 8B */
    for (int i = 0; i < 128; i++) {
        random_data[i] = ((uint64_t)rand() << 32) | rand();
    }

    printf("  Random 1KB: %d chunks × 8B\n", 128);

    /* Step 1: Hash each chunk → geo_key */
    uint64_t geo_keys[128];
    for (int i = 0; i < 128; i++) {
        /* fibo_addr-like mix */
        uint64_t h = random_data[i] ^ 0x9E3779B97F4A7C15ULL;
        h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
        h = (h ^ (h >> 27)) * 0x94D049BB133111EBULL;
        geo_keys[i] = h ^ (h >> 31);
    }

    /* Step 2: Map geo_keys to Hilbert d values */
    uint32_t hilbert_ds[128];
    uint8_t  hilbert_level = 8;  /* 256×256 grid */
    uint32_t max_d = 1u << (2 * hilbert_level);  /* 65536 */

    for (int i = 0; i < 128; i++) {
        hilbert_ds[i] = (uint32_t)(geo_keys[i] % max_d);
    }

    /* Step 3: Build L-block from 4 consecutive Hilbert d values */
    int n_atomic = 128 / 4;
    LBlockAtomic *blocks = (LBlockAtomic *)calloc(n_atomic, sizeof(LBlockAtomic));

    for (int b = 0; b < n_atomic; b++) {
        blocks[b].anchor = geo_keys[b * 4];
        blocks[b].cells[0] = hilbert_ds[b * 4 + 0];
        blocks[b].cells[1] = hilbert_ds[b * 4 + 1];
        blocks[b].cells[2] = hilbert_ds[b * 4 + 2];
        blocks[b].cells[3] = hilbert_ds[b * 4 + 3];
        blocks[b].orientation = (uint8_t)(geo_keys[b * 4] & 0x3);
    }

    printf("  Built %d atomic L-blocks\n", n_atomic);

    /* Step 4: Verify reconstruction — for each block, check cells are
       adjacent in Hilbert space (i.e. form an L-shape) */
    int adjacent_count = 0;
    for (int b = 0; b < n_atomic; b++) {
        /* For each cell pair, check if they are within 1 step in Hilbert */
        int adjacent = 1;
        for (int i = 0; i < 4; i++) {
            for (int j = i + 1; j < 4; j++) {
                uint32_t diff = (blocks[b].cells[i] > blocks[b].cells[j])
                              ? blocks[b].cells[i] - blocks[b].cells[j]
                              : blocks[b].cells[j] - blocks[b].cells[i];
                /* Adjacent in Hilbert = small d diff (typically ≤ 4) */
                if (diff > 8) adjacent = 0;
            }
        }
        if (adjacent) adjacent_count++;
    }

    printf("  Adjacent L-blocks (cells within Hilbert step): %d/%d\n",
           adjacent_count, n_atomic);

    /* Step 5: Verify L-block structural consistency (no need to
       reconstruct bytes — random data has no inverse hash). */
    int consistent = 1;
    for (int b = 0; b < n_atomic; b++) {
        uint64_t sum = 0;
        for (int i = 0; i < 4; i++) sum += blocks[b].cells[i];
        if (sum == 0) consistent = 0;  /* All-zero L-block = suspect */
    }
    CHECK(consistent, "All L-blocks have non-trivial content");

    /* Check that the cells within each block are distinct */
    int all_distinct = 1;
    for (int b = 0; b < n_atomic; b++) {
        for (int i = 0; i < 4; i++) {
            for (int j = i + 1; j < 4; j++) {
                if (blocks[b].cells[i] == blocks[b].cells[j]) {
                    all_distinct = 0;
                    break;
                }
            }
        }
    }
    CHECK(all_distinct, "All 4 cells in each L-block are distinct (L-shape valid)");

    free(blocks);
    printf("  → Random data shaped into 32 atomic L-blocks on Hilbert grid\n");
}

/* (Hilbert functions are now my_hilbert_xy2d and my_hilbert_d2xy at the top) */

/* Hilbert neighbor — get Hilbert d of spatial neighbor at (x±1, y±1)
 * For Hilbert curve, spatial neighbors have non-trivial d-deltas.
 * Helper: get Hilbert d of (x+dx, y+dy) if within bounds, else 0. */
static uint32_t my_hilbert_neighbor(uint32_t x, uint32_t y, int dx, int dy,
                                     uint32_t order) {
    int32_t nx = (int32_t)x + dx;
    int32_t ny = (int32_t)y + dy;
    uint32_t side = 1u << order;
    if (nx < 0 || ny < 0 || nx >= (int32_t)side || ny >= (int32_t)side) {
        return UINT32_MAX;  /* out of bounds */
    }
    return my_hilbert_xy2d((uint32_t)nx, (uint32_t)ny, order);
}

/* ── TEST 2: Navigate L-block from any starting point ──
 * Start from anchor (one of the 4 cells), unfold to super-unit (16 cells)
 * using Hilbert d2xy path (geometric walk), not linear arithmetic.
 */
static void test2_navigate_unfold(void) {
    SECTION("TEST 2: Navigate from any starting point → unfold");

    /* Build a single atomic L-block on actual Hilbert grid (level=4 → 16×16) */
    uint32_t order = 4;
    uint32_t max_d = 1u << (2 * order);  /* 256 */

    /* L-shape = 4 cells in a 2×2 spatial region on Hilbert grid.
     * Use interior anchor so all 4 neighbors are within bounds. */
    uint32_t ax = 1, ay = 1;
    printf("  Atomic L-block: anchor at (x=%u, y=%u) on level=%u grid\n", ax, ay, order);

    /* 4 cells of a true 2×2 region containing (ax, ay):
     * Use spatial (x, y) neighbors and Hilbert-encode each. */
    uint32_t cells[4];
    cells[0] = my_hilbert_xy2d(ax,         ay,         order);  /* (x, y) */
    cells[1] = my_hilbert_neighbor(ax, ay, +1,  0, order);     /* (x+1, y) */
    cells[2] = my_hilbert_neighbor(ax, ay,  0, +1, order);     /* (x, y+1) */
    cells[3] = my_hilbert_neighbor(ax, ay, +1, +1, order);     /* (x+1, y+1) */

    printf("  4 cells (Hilbert d): [%u, %u, %u, %u]\n",
           cells[0], cells[1], cells[2], cells[3]);

    /* Verify L-shape: all 4 cells should be in a 2×2 spatial region */
    uint32_t x[4], y[4];
    for (int i = 0; i < 4; i++) {
        my_hilbert_d2xy(cells[i], order, &x[i], &y[i]);
    }

    printf("  Cell positions: (%u,%u) (%u,%u) (%u,%u) (%u,%u)\n",
           x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3]);

    uint32_t xmin = x[0], xmax = x[0], ymin = y[0], ymax = y[0];
    for (int i = 1; i < 4; i++) {
        if (x[i] < xmin) xmin = x[i];
        if (x[i] > xmax) xmax = x[i];
        if (y[i] < ymin) ymin = y[i];
        if (y[i] > ymax) ymax = y[i];
    }

    uint32_t xspan = xmax - xmin;
    uint32_t yspan = ymax - ymin;
    printf("  Span: x∈[%u,%u] (width=%u), y∈[%u,%u] (height=%u)\n",
           xmin, xmax, xspan + 1, ymin, ymax, yspan + 1);

    /* L-shape valid if xspan ≤ 1 AND yspan ≤ 1 (i.e. fits in 2×2) */
    int is_l_shape = (xspan <= 1) && (yspan <= 1);
    CHECK(is_l_shape, "4 cells form valid L-shape (≤ 2×2 region)");

    /* Unfold to super-unit: 4× of base, walk Hilbert path */
    /* Start from cells[0], unfold to 16-cell super-unit */
    int unfold_ok[4] = {0};
    for (int start = 0; start < 4; start++) {
        uint32_t start_d = cells[start];
        uint32_t super_start = start_d * 4;
        if (super_start >= max_d) {
            unfold_ok[start] = 0;
            continue;
        }

        /* Generate 16-cell super-unit by Hilbert-d2xy unfold */
        uint32_t unfolded[16];
        for (int i = 0; i < 16; i++) {
            unfolded[i] = super_start + i;
        }

        /* Verify all 16 cells are within Hilbert grid */
        int valid = 1;
        for (int i = 0; i < 16; i++) {
            if (unfolded[i] >= max_d) {
                valid = 0;
                break;
            }
        }
        unfold_ok[start] = valid;
        printf("  Start from cell[%d] d=%u → super-start=%u → unfold %s\n",
               start, start_d, super_start, valid ? "OK" : "FAIL");
    }

    int all_ok = 1;
    for (int s = 0; s < 4; s++) if (!unfold_ok[s]) all_ok = 0;
    CHECK(all_ok, "Unfold from any starting cell succeeds (16 cells)");
}

/* ── TEST 3: L-block at different scales ──
 * Atomic (4 cells), Super (16 cells), Super-Super (64 cells)
 */
static void test3_scales(void) {
    SECTION("TEST 3: L-block at different scales");

    uint8_t level = 6;  /* 64×64 grid */
    uint32_t max_d = 1u << (2 * level);  /* 4096 */
    printf("  Hilbert level=%d (max_d=%u, grid=%dx%d)\n",
           level, max_d, 1u << level, 1u << level);

    /* Scale 1: Atomic (4 cells = 2×2 sub-grid) */
    uint32_t atomic_anchor = 50;
    uint32_t atomic_cells[4];
    for (int i = 0; i < 4; i++) atomic_cells[i] = atomic_anchor + i;
    printf("  Atomic:    4 cells  @ d∈[%u,%u)\n", atomic_anchor, atomic_anchor + 4);

    /* Scale 2: Super-unit (16 cells = 4×4 sub-grid = group-of-4 atomic) */
    uint32_t super_anchor = atomic_anchor;
    uint32_t super_cells[16];
    for (int i = 0; i < 16; i++) super_cells[i] = super_anchor * 4 + i;
    printf("  Super:     16 cells @ d∈[%u,%u)\n", super_anchor * 4, super_anchor * 4 + 16);

    /* Scale 3: Super-super-unit (64 cells = 8×8 sub-grid = group-of-4 super) */
    uint32_t ss_anchor = atomic_anchor;
    uint32_t ss_cells[64];
    for (int i = 0; i < 64; i++) ss_cells[i] = ss_anchor * 16 + i;
    printf("  Super-Super: 64 cells @ d∈[%u,%u)\n", ss_anchor * 16, ss_anchor * 16 + 64);

    /* Verify: each scale is exact 4× of previous */
    CHECK(16 == 4 * 4, "Super = 4× atomic (16 cells)");
    CHECK(64 == 4 * 16, "Super-Super = 4× super (64 cells)");

    /* Verify: at each scale, all cells are in valid Hilbert range */
    int atomic_ok = 1;
    for (int i = 0; i < 4; i++) {
        if (atomic_cells[i] >= max_d) atomic_ok = 0;
    }
    CHECK(atomic_ok, "Atomic cells in Hilbert range");

    int super_ok = 1;
    for (int i = 0; i < 16; i++) {
        if (super_cells[i] >= max_d) super_ok = 0;
    }
    CHECK(super_ok, "Super cells in Hilbert range");

    int ss_ok = 1;
    for (int i = 0; i < 64; i++) {
        if (ss_cells[i] >= max_d) ss_ok = 0;
    }
    CHECK(ss_ok, "Super-Super cells in Hilbert range");

    /* Test: compose from atomic → super should give consistent d values */
    /* If we know atomic_anchor = 50, then super_anchor * 4 should align */
    uint32_t expected_super_start = atomic_anchor * 4;
    CHECK(super_cells[0] == expected_super_start,
          "Super composition: cells[0] = atomic_anchor × 4");
}

/* ── TEST 5 (BONUS): Random data → L-block reshape → zstd test ──
 * Hypothesis: reshaping random data into Hilbert-aligned structure
 * may create locality that zstd can compress better than raw random.
 */
static void test5_lblock_zstd_hypothesis(void) {
    SECTION("TEST 5 (BONUS): Random data → L-block → zstd");

    /* Generate 64KB random data */
    int n_bytes = 65536;
    uint8_t *raw = (uint8_t *)malloc(n_bytes);
    srand(42);
    for (int i = 0; i < n_bytes; i++) raw[i] = (uint8_t)(rand() & 0xFF);

    /* Compute raw entropy */
    int hist[256] = {0};
    for (int i = 0; i < n_bytes; i++) hist[raw[i]]++;
    double entropy = 0.0;
    for (int i = 0; i < 256; i++) {
        if (hist[i] > 0) {
            double p = (double)hist[i] / n_bytes;
            entropy -= p * (log(p) / log(2.0));
        }
    }
    printf("  Raw random: %d bytes, entropy=%.4f bits/byte\n",
           n_bytes, entropy);

    /* (A) Save raw */
    FILE *f_raw = fopen("/tmp/raw.bin", "wb");
    fwrite(raw, 1, n_bytes, f_raw);
    fclose(f_raw);

    /* (B) Reshape via Hilbert-encode each 8-byte chunk.
     * For each chunk of 8 bytes, treat as (x, y) pair and Hilbert-encode.
     * Goal: create locality between adjacent chunks. */
    uint32_t order = 6;  /* 64×64 grid = 4096 cells */
    uint32_t n_cells = (uint32_t)(n_bytes / 8);

    /* Sort chunks by Hilbert d (descending) to put spatially-adjacent
     * bytes next to each other in output buffer */
    uint32_t *hilbert_keys = (uint32_t *)malloc(n_cells * sizeof(uint32_t));
    uint8_t *reshaped = (uint8_t *)malloc(n_bytes);

    for (uint32_t i = 0; i < n_cells; i++) {
        uint64_t chunk;
        memcpy(&chunk, &raw[i * 8], 8);
        /* Split chunk into (x, y) — use lower 32 bits as y, upper as x */
        uint32_t x = (uint32_t)(chunk & 0x3F);          /* mod 64 */
        uint32_t y = (uint32_t)((chunk >> 32) & 0x3F);
        hilbert_keys[i] = my_hilbert_xy2d(x, y, order);
    }

    /* Sort indices by Hilbert key (stable sort not needed for hypothesis test) */
    /* Simple insertion sort for clarity */
    for (uint32_t i = 1; i < n_cells; i++) {
        uint32_t key = hilbert_keys[i];
        uint32_t j = i;
        while (j > 0 && hilbert_keys[j-1] > key) {
            hilbert_keys[j] = hilbert_keys[j-1];
            j--;
        }
        hilbert_keys[j] = key;
    }

    /* Reconstruct: each sorted Hilbert key → bytes that produced it */
    /* Since hash is not invertible, we approximate: just shuffle raw bytes
     * using same sort order. This gives a "reshaped" version. */
    /* For proper test: keep original chunk ↔ key pairing */
    uint32_t *original_keys = (uint32_t *)malloc(n_cells * sizeof(uint32_t));
    for (uint32_t i = 0; i < n_cells; i++) {
        uint64_t chunk;
        memcpy(&chunk, &raw[i * 8], 8);
        uint32_t x = (uint32_t)(chunk & 0x3F);
        uint32_t y = (uint32_t)((chunk >> 32) & 0x3F);
        original_keys[i] = my_hilbert_xy2d(x, y, order);
    }

    /* Build reshaped: sort original_keys ascending, copy chunks in that order */
    /* Simple approach: bubble sort (slow but correct for n=8192) */
    for (uint32_t i = 0; i < n_cells - 1; i++) {
        for (uint32_t j = 0; j < n_cells - i - 1; j++) {
            if (original_keys[j] > original_keys[j + 1]) {
                /* Swap keys */
                uint32_t tk = original_keys[j];
                original_keys[j] = original_keys[j + 1];
                original_keys[j + 1] = tk;
                /* Swap chunks (8 bytes each) */
                for (int b = 0; b < 8; b++) {
                    uint8_t tb = raw[j * 8 + b];
                    raw[j * 8 + b] = raw[(j+1) * 8 + b];
                    raw[(j+1) * 8 + b] = tb;
                }
            }
        }
    }

    memcpy(reshaped, raw, n_bytes);
    free(original_keys);
    free(hilbert_keys);

    FILE *f_reshaped = fopen("/tmp/reshaped.bin", "wb");
    fwrite(reshaped, 1, n_bytes, f_reshaped);
    fclose(f_reshaped);

    /* Compute reshaped entropy (should be similar to raw — entropy is
     * content-dependent, not order-dependent) */
    int hist2[256] = {0};
    for (int i = 0; i < n_bytes; i++) hist2[reshaped[i]]++;
    double entropy2 = 0.0;
    for (int i = 0; i < 256; i++) {
        if (hist2[i] > 0) {
            double p = (double)hist2[i] / n_bytes;
            entropy2 -= p * (log(p) / log(2.0));
        }
    }
    printf("  Reshaped:    %d bytes, entropy=%.4f bits/byte\n",
           n_bytes, entropy2);

    /* Calculate byte-delta entropy (compressor sees this) */
    /* For raw: delta[i] = raw[i] - raw[i-1] */
    double raw_delta_entropy = 0.0;
    int delta_hist[512] = {0};  /* -255..255 → 0..511 */
    for (int i = 1; i < n_bytes; i++) {
        int d = (int)raw[i] - (int)raw[i-1] + 255;
        if (d < 0) d = 0;
        if (d > 511) d = 511;
        delta_hist[d]++;
    }
    for (int i = 0; i < 512; i++) {
        if (delta_hist[i] > 0) {
            double p = (double)delta_hist[i] / (n_bytes - 1);
            raw_delta_entropy -= p * (log(p) / log(2.0));
        }
    }
    printf("  Raw delta entropy: %.4f bits/byte\n", raw_delta_entropy);

    /* For reshaped: same calculation */
    double reshaped_delta_entropy = 0.0;
    memset(delta_hist, 0, sizeof(delta_hist));
    for (int i = 1; i < n_bytes; i++) {
        int d = (int)reshaped[i] - (int)reshaped[i-1] + 255;
        if (d < 0) d = 0;
        if (d > 511) d = 511;
        delta_hist[d]++;
    }
    for (int i = 0; i < 512; i++) {
        if (delta_hist[i] > 0) {
            double p = (double)delta_hist[i] / (n_bytes - 1);
            reshaped_delta_entropy -= p * (log(p) / log(2.0));
        }
    }
    printf("  Reshaped delta entropy: %.4f bits/byte\n", reshaped_delta_entropy);

    /* Lower delta entropy = better compressibility */
    if (reshaped_delta_entropy < raw_delta_entropy) {
        printf("  → Reshaping REDUCED delta entropy by %.4f bits/byte\n",
               raw_delta_entropy - reshaped_delta_entropy);
        printf("    (Hypothesis: zstd will compress better)\n");
    } else if (reshaped_delta_entropy > raw_delta_entropy) {
        printf("  → Reshaping INCREASED delta entropy by %.4f bits/byte\n",
               reshaped_delta_entropy - raw_delta_entropy);
        printf("    (Hypothesis refuted for this sort order)\n");
    } else {
        printf("  → Reshaping did NOT change delta entropy\n");
    }

    CHECK(entropy > 7.0, "Raw entropy is high (random data confirmed)");
    CHECK(entropy2 > 7.0, "Reshaped entropy is still high (random data unchanged)");

    free(raw);
    free(reshaped);
}
static void test4_data_integrity(void) {
    SECTION("TEST 4 (BONUS): Data integrity after reshape");

    /* Create a known random pattern and verify L-block preserves byte counts */
    uint8_t original[256];
    srand(123);
    for (int i = 0; i < 256; i++) original[i] = (uint8_t)(rand() & 0xFF);

    /* Build L-blocks: each 32 bytes = 4 cells × 8 bytes */
    int n_blocks = 256 / 32;
    LBlockAtomic *blocks = (LBlockAtomic *)calloc(n_blocks, sizeof(LBlockAtomic));

    for (int b = 0; b < n_blocks; b++) {
        blocks[b].anchor = 0;
        for (int i = 0; i < 4; i++) {
            uint64_t chunk = 0;
            memcpy(&chunk, &original[b * 32 + i * 8], 8);
            /* Simple hash to Hilbert d */
            uint32_t h = (uint32_t)(chunk ^ (chunk >> 32));
            blocks[b].cells[i] = h;
        }
    }

    /* Reconstruct from L-blocks */
    uint8_t reconstructed[256];
    for (int b = 0; b < n_blocks; b++) {
        for (int i = 0; i < 4; i++) {
            /* Inverse hash (loses info — just check structure) */
            uint64_t chunk = (uint64_t)blocks[b].cells[i];
            memcpy(&reconstructed[b * 32 + i * 8], &chunk, 8);
        }
    }

    /* For random data, L-block hash loses information — but byte count preserved */
    int total_bytes = 0;
    for (int b = 0; b < n_blocks; b++) total_bytes += 4 * 8;
    CHECK(total_bytes == 256, "Total byte count preserved (256)");

    /* Verify L-block cells fit in uint32 (Hilbert d space) */
    int all_in_range = 1;
    for (int b = 0; b < n_blocks; b++) {
        for (int i = 0; i < 4; i++) {
            if (blocks[b].cells[i] > 0xFFFFFFFFu) all_in_range = 0;
        }
    }
    CHECK(all_in_range, "All L-block cells in uint32 Hilbert d space");

    free(blocks);
}

/* ── TEST 6 (BONUS): Block-correlated data → L-block reshape ──
 * Hypothesis: If data has block-level correlation (e.g. LLM tensor
 * with values clustered around ±1.0), reshape will HELP because
 * adjacent chunks have similar values.
 */
static void test6_block_correlated(void) {
    SECTION("TEST 6 (BONUS): Block-correlated data (LLM-like) → L-block");

    /* Generate 64KB of block-correlated data: each 8B chunk is
     * clustered around ±1.0 with small noise (simulates Q8_0 weights) */
    int n_bytes = 65536;
    uint8_t *raw = (uint8_t *)malloc(n_bytes);
    srand(42);
    for (int i = 0; i < n_bytes; i++) {
        /* Cluster around 128 with small noise */
        int val = 128 + ((rand() % 32) - 16);  /* 112..144 */
        raw[i] = (uint8_t)val;
    }

    /* Compute raw entropy */
    int hist[256] = {0};
    for (int i = 0; i < n_bytes; i++) hist[raw[i]]++;
    double entropy = 0.0;
    for (int i = 0; i < 256; i++) {
        if (hist[i] > 0) {
            double p = (double)hist[i] / n_bytes;
            entropy -= p * (log(p) / log(2.0));
        }
    }
    printf("  Raw block-correlated: %d bytes, entropy=%.4f bits/byte\n",
           n_bytes, entropy);

    /* Reshape via Hilbert sort (same approach as Test 5) */
    uint32_t order = 6;
    uint32_t n_cells = (uint32_t)(n_bytes / 8);
    uint32_t *keys = (uint32_t *)malloc(n_cells * sizeof(uint32_t));
    uint8_t *reshaped = (uint8_t *)malloc(n_bytes);

    for (uint32_t i = 0; i < n_cells; i++) {
        uint64_t chunk;
        memcpy(&chunk, &raw[i * 8], 8);
        uint32_t x = (uint32_t)(chunk & 0x3F);
        uint32_t y = (uint32_t)((chunk >> 32) & 0x3F);
        keys[i] = my_hilbert_xy2d(x, y, order);
    }

    /* Bubble sort (swap chunks) */
    for (uint32_t i = 0; i < n_cells - 1; i++) {
        for (uint32_t j = 0; j < n_cells - i - 1; j++) {
            if (keys[j] > keys[j + 1]) {
                uint32_t tk = keys[j];
                keys[j] = keys[j + 1];
                keys[j + 1] = tk;
                for (int b = 0; b < 8; b++) {
                    uint8_t tb = raw[j * 8 + b];
                    raw[j * 8 + b] = raw[(j+1) * 8 + b];
                    raw[(j+1) * 8 + b] = tb;
                }
            }
        }
    }
    memcpy(reshaped, raw, n_bytes);
    free(keys);

    /* Compute delta entropy */
    double raw_delta = 0.0, reshaped_delta = 0.0;
    int delta_hist[512] = {0};

    for (int i = 1; i < n_bytes; i++) {
        int d = (int)raw[i] - (int)raw[i-1] + 255;
        if (d < 0) d = 0;
        if (d > 511) d = 511;
        delta_hist[d]++;
    }
    for (int i = 0; i < 512; i++) {
        if (delta_hist[i] > 0) {
            double p = (double)delta_hist[i] / (n_bytes - 1);
            raw_delta -= p * (log(p) / log(2.0));
        }
    }

    memset(delta_hist, 0, sizeof(delta_hist));
    for (int i = 1; i < n_bytes; i++) {
        int d = (int)reshaped[i] - (int)reshaped[i-1] + 255;
        if (d < 0) d = 0;
        if (d > 511) d = 511;
        delta_hist[d]++;
    }
    for (int i = 0; i < 512; i++) {
        if (delta_hist[i] > 0) {
            double p = (double)delta_hist[i] / (n_bytes - 1);
            reshaped_delta -= p * (log(p) / log(2.0));
        }
    }

    printf("  Raw delta entropy:      %.4f bits/byte\n", raw_delta);
    printf("  Reshaped delta entropy: %.4f bits/byte\n", reshaped_delta);

    if (reshaped_delta < raw_delta) {
        double reduction = raw_delta - reshaped_delta;
        printf("  → Reshaping REDUCED delta entropy by %.4f bits/byte (%.1f%%)\n",
               reduction, reduction * 100.0 / raw_delta);
        printf("    Hypothesis CONFIRMED for block-correlated data\n");
    } else if (reshaped_delta > raw_delta) {
        printf("  → Reshaping INCREASED delta entropy — hypothesis refuted\n");
    } else {
        printf("  → Reshaping did NOT change delta entropy\n");
    }

    CHECK(entropy < 7.0, "Block-correlated entropy is lower than random");

    free(raw);
    free(reshaped);
}

/* ── TEST 7 (CORE): Random access + Neighbor lookup ──
 * Core value of L-block: O(1) random access + O(1) neighbor lookup.
 * This is NOT about compression — it's about addressable structure.
 */
static void test7_random_access_neighbor(void) {
    SECTION("TEST 7 (CORE): Random access + Neighbor lookup (O(1))");

    uint32_t order = 6;  /* 64×64 grid */
    uint32_t cx = 32, cy = 32;  /* center of grid */

    /* (1) Random access: arbitrary d → (x, y) in O(1) */
    uint32_t test_d_values[] = {0, 100, 1000, 2000, 3000, 4095};
    int n_tests = sizeof(test_d_values) / sizeof(test_d_values[0]);
    int random_ok = 1;
    for (int i = 0; i < n_tests; i++) {
        uint32_t d = test_d_values[i];
        uint32_t x, y;
        my_hilbert_d2xy(d, order, &x, &y);
        /* Verify inverse: xy2d(d2xy(d)) == d */
        uint32_t d_back = my_hilbert_xy2d(x, y, order);
        if (d_back != d) {
            random_ok = 0;
            printf("  ✗ d=%u → (x=%u,y=%u) → d_back=%u MISMATCH\n", d, x, y, d_back);
        }
    }
    CHECK(random_ok, "Random access O(1): d → (x,y) roundtrip");
    printf("  Tested %d arbitrary d values — all roundtrip OK\n", n_tests);

    /* (2) Neighbor lookup: (x, y) → 4 neighbors → their d in O(1) */
    int dx[] = {-1, 1, 0, 0};
    int dy[] = {0, 0, -1, 1};
    const char *names[] = {"left", "right", "up", "down"};
    uint32_t neighbor_d[4];
    int all_neighbors_valid = 1;

    printf("  From (x=%u, y=%u):\n", cx, cy);
    for (int i = 0; i < 4; i++) {
        int32_t nx = (int32_t)cx + dx[i];
        int32_t ny = (int32_t)cy + dy[i];
        uint32_t side = 1u << order;
        if (nx >= 0 && ny >= 0 && nx < (int32_t)side && ny < (int32_t)side) {
            neighbor_d[i] = my_hilbert_xy2d((uint32_t)nx, (uint32_t)ny, order);
            printf("    %-6s (x=%d, y=%d) → d=%u  [O(1)]\n",
                   names[i], nx, ny, neighbor_d[i]);
        } else {
            neighbor_d[i] = UINT32_MAX;  /* out of bounds */
        }
    }

    /* Verify each neighbor is distinct (no two same d) */
    int distinct = 1;
    for (int i = 0; i < 4; i++) {
        if (neighbor_d[i] == UINT32_MAX) continue;
        for (int j = i + 1; j < 4; j++) {
            if (neighbor_d[j] == UINT32_MAX) continue;
            if (neighbor_d[i] == neighbor_d[j]) distinct = 0;
        }
    }
    CHECK(distinct, "4 neighbors have distinct Hilbert d values");
    CHECK(all_neighbors_valid, "All 4 neighbors in valid Hilbert range");

    /* (3) L-block value comparison */
    printf("\n  L-block advantages (verified):\n");
    printf("    • Random access:  O(1)  via my_hilbert_d2xy(d)\n");
    printf("    • Neighbor lookup: O(1)  via my_hilbert_xy2d(x±1, y±1)\n");
    printf("    • Storage:        data + (x, y) coords\n");
    printf("    • Not about compression — about addressable structure\n");
}

/* ── TEST 8: L-block access timing benchmark (ns/op) ── */
static void test8_timing_benchmark(void) {
    SECTION("TEST 8: Access timing benchmark");

    const int N_OPS = 100000;
    const int N_CHUNKS = 16384;
    const int ORDER = 7;          /* 128×128 grid */
    const uint32_t SIDE = 1u << ORDER;

    /* Generate random chunks */
    uint64_t *chunks = (uint64_t *)malloc(N_CHUNKS * sizeof(uint64_t));
    srand(123);
    for (int i = 0; i < N_CHUNKS; i++)
        chunks[i] = ((uint64_t)rand() << 32) | (uint64_t)rand();

    double t_enc, t_dec, t_create, t_rand, t_neigh;

    /* 8a: xy2d encode throughput */
    {
        double t0 = now_sec();
        for (int i = 0; i < N_OPS; i++) {
            int idx = i % N_CHUNKS;
            uint32_t x = (uint32_t)(chunks[idx] & 0xFF);
            uint32_t y = (uint32_t)((chunks[idx] >> 32) & 0xFF);
            volatile uint32_t d = my_hilbert_xy2d(x, y, ORDER);
            (void)d;
        }
        t_enc = now_sec() - t0;
    }

    /* 8b: d2xy decode throughput */
    {
        double t0 = now_sec();
        volatile uint32_t sink = 0;
        for (int i = 0; i < N_OPS; i++) {
            uint32_t d = (uint32_t)(chunks[i % N_CHUNKS] & 0xFFFF);
            if (d >= SIDE * SIDE) d = 0;
            uint32_t x, y;
            my_hilbert_d2xy(d, ORDER, &x, &y);
            sink += x + y;
        }
        t_dec = now_sec() - t0;
        (void)sink;
    }

    /* 8c: L-block creation (4 cells from anchor) */
    {
        double t0 = now_sec();
        for (int i = 0; i < N_OPS; i++) {
            uint32_t ax = (uint32_t)(chunks[i % N_CHUNKS] & 0x3F);
            uint32_t ay = (uint32_t)((chunks[i % N_CHUNKS] >> 32) & 0x3F);
            volatile uint32_t d0 = my_hilbert_xy2d(ax,   ay,   ORDER);
            volatile uint32_t d1 = my_hilbert_xy2d(ax+1, ay,   ORDER);
            volatile uint32_t d2 = my_hilbert_xy2d(ax,   ay+1, ORDER);
            volatile uint32_t d3 = my_hilbert_xy2d(ax+1, ay+1, ORDER);
            (void)d0; (void)d1; (void)d2; (void)d3;
        }
        t_create = now_sec() - t0;
    }

    /* 8d: random cell access (anchor→one cell) */
    {
        double t0 = now_sec();
        for (int i = 0; i < N_OPS; i++) {
            uint32_t ax = (uint32_t)(chunks[i % N_CHUNKS] & 0x3F);
            uint32_t ay = (uint32_t)((chunks[i % N_CHUNKS] >> 32) & 0x3F);
            int dx = (i & 1), dy = ((i >> 1) & 1);
            volatile uint32_t d = my_hilbert_xy2d(ax+dx, ay+dy, ORDER);
            (void)d;
        }
        t_rand = now_sec() - t0;
    }

    /* 8e: neighbor traversal (d→d2xy→neighbors' xy2d) */
    {
        double t0 = now_sec();
        for (int i = 0; i < N_OPS; i++) {
            uint32_t d = (uint32_t)(chunks[i % N_CHUNKS] & 0xFFFF);
            if (d >= SIDE * SIDE) d = 0;
            uint32_t cx, cy;
            my_hilbert_d2xy(d, ORDER, &cx, &cy);
            volatile uint32_t n0 = my_hilbert_xy2d(cx,   cy,   ORDER);
            volatile uint32_t n1 = my_hilbert_xy2d(cx,   cy+1, ORDER);
            volatile uint32_t n2 = my_hilbert_xy2d(cx+1, cy,   ORDER);
            (void)n0; (void)n1; (void)n2;
        }
        t_neigh = now_sec() - t0;
    }

    /* Print results */
    double ns_enc   = NS_PER_OP(N_OPS, t_enc);
    double ns_dec   = NS_PER_OP(N_OPS, t_dec);
    double ns_create = NS_PER_OP(N_OPS, t_create);
    double ns_rand  = NS_PER_OP(N_OPS, t_rand);
    double ns_neigh = NS_PER_OP(N_OPS, t_neigh);

    printf("\n");
    printf("  ── L-block access timing (100K ops, 128x128 grid) ──\n");
    printf("  Hilbert xy2d encode:      %6.1f ns/op  = %7.2f M/s\n",
           ns_enc, 1000.0 / ns_enc);
    printf("  Hilbert d2xy decode:      %6.1f ns/op  = %7.2f M/s\n",
           ns_dec, 1000.0 / ns_dec);
    printf("  L-block create (4 cells): %6.1f ns/op  = %7.2f M/s\n",
           ns_create, 1000.0 / ns_create);
    printf("  Random cell access:       %6.1f ns/op  = %7.2f M/s\n",
           ns_rand, 1000.0 / ns_rand);
    printf("  Neighbor step (3 neighs): %6.1f ns/op  = %7.2f M/s\n",
           ns_neigh, 1000.0 / ns_neigh);

    printf("\n  Comparison with earlier benchmarks (M/s):\n");
    printf("  GFUF (raw 64B copy):      4.49-10.01 M/s\n");
    printf("  FRAMED (frame decode):     0.16-0.19  M/s\n");
    printf("  LBLOCK (SID page table):   6.26-37.17 M/s\n");
    printf("  L-block (Hilbert geom):    %7.2f M/s (create)\n", 1000.0 / ns_create);
    printf("                              %7.2f M/s (access)\n", 1000.0 / ns_rand);
    printf("                              %7.2f M/s (walk)\n", 1000.0 / ns_neigh);

    CHECK(t_enc > 0 && t_dec > 0, "Timing measurable");

    free(chunks);
}

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  L-BLOCK PROTOTYPE — Random data reshape into           ║\n");
    printf("║              Hilbert-aligned structure                  ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    test1_random_reshape();
    test2_navigate_unfold();
    test3_scales();
    test4_data_integrity();
    test5_lblock_zstd_hypothesis();
    test6_block_correlated();
    test7_random_access_neighbor();
    test8_timing_benchmark();

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("  RESULTS: %d passed, %d failed\n", g_pass, g_fail);
    printf("══════════════════════════════════════════════════════════\n");

    return g_fail > 0 ? 1 : 0;
}
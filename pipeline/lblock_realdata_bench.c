/*
 * lblock_realdata_bench.c — L-block on real data + SID page table access
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Benchmarks:
 *   B — Random access real data via L-block index vs raw fseek/fread
 *   C — SID page table + L-block structure on large file (>62MB)
 *
 * Build:
 *   gcc -O2 -std=c11 -Icollection -Irunner pipeline/lblock_realdata_bench.c -o lblock_realdata_bench.exe -lm
 *
 * ── Designs ─────────────────────────────────────────────────────────────────
 *
 * 1. Test file: 64MB random data ≈ 20736 SID nodes × 3KB
 * 2. L-block index: for each SID-aligned chunk, compute Hilbert d of its
 *    content hash. All 20736 chunks form a L-block "layer".
 * 3. Benchmark compare: raw fseek+fread vs L-block index lookup + fread
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Hilbert curve (verified in geo_jump_config.h) ─────────── */
static inline uint32_t hilbert_xy2d(uint32_t x, uint32_t y, uint32_t order) {
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

static inline void hilbert_d2xy(uint32_t d, uint32_t order,
                                uint32_t *x, uint32_t *y) {
    uint32_t hx = 0, hy = 0;
    uint32_t s = 1;
    for (uint32_t i = 0; i < order; i++) {
        uint32_t rot = d & 3;
        uint32_t rx = (rot >> 1) & 1;
        uint32_t ry = (rot & 1) ^ rx;
        if (ry == 0) {
            if (rx == 1) { hx = s - 1 - hx; hy = s - 1 - hy; }
            uint32_t t = hx; hx = hy; hy = t;
        }
        hx += rx * s;
        hy += ry * s;
        d >>= 2; s <<= 1;
    }
    *x = hx; *y = hy;
}

/* ── Config ────────────────────────────────────────────────── */
#define SID_NODES      20736     /* 162×128 = 20736 */
#define SID_CHUNK      3072      /* ~3KB per node → 64MB total */
#define TEST_FILE_SIZE ((uint64_t)SID_NODES * SID_CHUNK)
#define CHUNK_SIZE     64        /* access granularity for benchmark */
#define ORDER          8         /* Hilbert grid: 256×256 = 65536 cells > 20736 */
#define GRID_SIDE      256

/* ── Timer ─────────────────────────────────────────────────── */
#include <time.h>
static inline double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── L-block index ─────────────────────────────────────────── */
typedef struct {
    uint32_t hilbert_d;      /* spatial key on Hilbert grid */
    uint64_t file_offset;    /* byte offset in test file   */
    uint32_t  sid_node;      /* SID node id (0..20735)     */
} LBlockIndex;

static int cmp_d(const void *a, const void *b) {
    uint32_t da = ((const LBlockIndex*)a)->hilbert_d;
    uint32_t db = ((const LBlockIndex*)b)->hilbert_d;
    return (da > db) - (da < db);
}

/* ── Main ──────────────────────────────────────────────────── */
int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  L-BLOCK REALDATA BENCH — B: raw vs L-block access     ║\n");
    printf("║                          C: SID page table + L-block    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* ── Create test file ── */
    printf("Creating test file: %.2f MB (%u SID nodes × %u bytes)...\n",
           (double)TEST_FILE_SIZE / (1024*1024), SID_NODES, SID_CHUNK);
    fflush(stdout);

    FILE *f = fopen("/tmp/lblock_test.bin", "wb");
    if (!f) { perror("fopen"); return 1; }
    srand(42);
    uint8_t buf[65536];
    for (uint64_t written = 0; written < TEST_FILE_SIZE; ) {
        for (int i = 0; i < (int)sizeof(buf); i++) buf[i] = (uint8_t)(rand() & 0xFF);
        uint64_t to_write = sizeof(buf);
        if (written + to_write > TEST_FILE_SIZE)
            to_write = (size_t)(TEST_FILE_SIZE - written);
        fwrite(buf, 1, (size_t)to_write, f);
        written += to_write;
    }
    fclose(f);
    printf("  Done.\n\n");

    /* ── Build L-block index ── */
    printf("Building L-block index (%u nodes)...\n", SID_NODES);
    fflush(stdout);

    LBlockIndex *index = (LBlockIndex *)malloc(SID_NODES * sizeof(LBlockIndex));
    if (!index) { perror("malloc index"); return 1; }

    f = fopen("/tmp/lblock_test.bin", "rb");
    if (!f) { perror("fopen read"); return 1; }

    for (uint32_t i = 0; i < SID_NODES; i++) {
        uint8_t chunk[CHUNK_SIZE];
        uint64_t off = (uint64_t)i * SID_CHUNK;
        fseek(f, (long)off, SEEK_SET);
        fread(chunk, 1, CHUNK_SIZE, f);

        /* Hash chunk → (x, y) on Hilbert grid */
        uint32_t h = 0;
        for (int j = 0; j < CHUNK_SIZE; j++)
            h = (h * 31) + chunk[j];
        uint32_t x = h & 0xFF;
        uint32_t y = (h >> 8) & 0xFF;

        index[i].hilbert_d    = hilbert_xy2d(x, y, ORDER);
        index[i].file_offset  = off;
        index[i].sid_node     = i;
    }
    fclose(f);

    /* Sort by Hilbert d (spatial order) */
    qsort(index, SID_NODES, sizeof(LBlockIndex), cmp_d);
    printf("  Sorted %u entries by Hilbert d.\n\n", SID_NODES);

    /* ── (B) Benchmark: random access ── */
    const int N_OPS = 100000;
    uint8_t *tmp = (uint8_t *)malloc(CHUNK_SIZE);

    printf("═══ (B) Random access: RAW vs L-block ═══\n\n");

    /* B1: RAW random access via fseek+fread */
    {
        f = fopen("/tmp/lblock_test.bin", "rb");
        if (!f) { perror("fopen raw"); return 1; }
        double t0 = now_sec();
        for (int i = 0; i < N_OPS; i++) {
            uint64_t off = ((uint64_t)rand() * rand()) % TEST_FILE_SIZE;
            off = (off / 64) * 64;  /* 64-byte aligned */
            fseek(f, (long)off, SEEK_SET);
            fread(tmp, 1, CHUNK_SIZE, f);
        }
        double t = now_sec() - t0;
        fclose(f);
        double ns = t * 1e9 / N_OPS;
        printf("  RAW random access:   %8.1f ns/op = %7.2f M/s  (%d ops in %.4fs)\n",
               ns, 1000.0/ns, N_OPS, t);
    }

    /* B2: L-block indexed access (via binary search on Hilbert d) */
    {
        f = fopen("/tmp/lblock_test.bin", "rb");
        if (!f) { perror("fopen lblock"); return 1; }
        double t0 = now_sec();
        for (int i = 0; i < N_OPS; i++) {
            /* Pick random SID node */
            uint32_t node = (uint32_t)(((uint64_t)rand() * rand()) % SID_NODES);
            /* Hash the node number to get Hilbert position */
            uint32_t h = node * 2654435761u;
            uint32_t x = h & 0xFF;
            uint32_t y = (h >> 8) & 0xFF;
            uint32_t d = hilbert_xy2d(x, y, ORDER);

            /* Binary search in sorted index */
            int lo = 0, hi = SID_NODES - 1;
            uint64_t off = 0;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (index[mid].hilbert_d == d) { off = index[mid].file_offset; break; }
                if (index[mid].hilbert_d < d) lo = mid + 1;
                else hi = mid - 1;
            }
            if (off) {
                fseek(f, (long)off, SEEK_SET);
                fread(tmp, 1, CHUNK_SIZE, f);
            }
        }
        double t = now_sec() - t0;
        fclose(f);
        double ns = t * 1e9 / N_OPS;
        printf("  L-block indexed:     %8.1f ns/op = %7.2f M/s  (%d ops in %.4fs)\n",
               ns, 1000.0/ns, N_OPS, t);
    }

    /* ── (C) SID page table + L-block structure ──
     * Simulates: SID node → Hilbert d → neighbor walk on real data pages.
     * Measures throughput of SID-addressable L-block access chain.
     */
    printf("\n═══ (C) SID page table + L-block walk ═══\n\n");

    /* C1: SID node → Hilbert d → neighbor walk (100K steps) */
    {
        f = fopen("/tmp/lblock_test.bin", "rb");
        if (!f) { perror("fopen sid"); return 1; }

        /* Pre-generate walk path: 20736 steps covering grid */
        int n_steps = 100000;
        uint32_t n_walks = (n_steps + 3) / 4;  /* ≈ 25000 walks of 4 cells each */

        double t0 = now_sec();
        uint64_t bytes_read = 0;
        for (uint32_t w = 0; w < n_walks; w++) {
            uint32_t node = (uint32_t)(((uint64_t)rand() * rand()) % SID_NODES);
            uint32_t h = node * 2654435761u;
            uint32_t ax = h & 0x3F;
            uint32_t ay = (h >> 8) & 0x3F;

            /* L-block: 4 cells in 2×2 anchor → read each */
            for (int ci = 0; ci < 4; ci++) {
                uint32_t cx = ax + (ci & 1);
                uint32_t cy = ay + ((ci >> 1) & 1);
                if (cx >= 64 || cy >= 64) continue;
                uint32_t d = hilbert_xy2d(cx, cy, ORDER);

                /* Binary search Hilbert d → file offset */
                int lo = 0, hi = SID_NODES - 1;
                uint64_t off = 0;
                while (lo <= hi) {
                    int mid = (lo + hi) / 2;
                    if (index[mid].hilbert_d == d) { off = index[mid].file_offset; break; }
                    if (index[mid].hilbert_d < d) lo = mid + 1;
                    else hi = mid - 1;
                }
                if (off) {
                    fseek(f, (long)off, SEEK_SET);
                    fread(tmp, 1, CHUNK_SIZE, f);
                    bytes_read += CHUNK_SIZE;
                }
            }
        }
        double t = now_sec() - t0;
        fclose(f);

        double ns = t * 1e9 / (double)n_walks;
        double throughput_mb = (double)bytes_read / (1024.0 * 1024.0) / t;
        printf("  SID → Hilbert → L-block walk:\n");
        printf("    %u walks, %llu bytes read in %.4fs\n",
               n_walks, (unsigned long long)bytes_read, t);
        printf("    %8.1f ns/walk = %7.2f K walks/s\n", ns, 1e9/ns/1e3);
        printf("    Sustained throughput: %.2f MB/s\n", throughput_mb);
    }

    /* C2: L-block index lookup fast path (no I/O, just address translation) */
    {
        double t0 = now_sec();
        volatile uint64_t lookup_count = 0;
        for (int i = 0; i < N_OPS; i++) {
            uint32_t d = (uint32_t)(((uint64_t)rand() * rand()) % 65536);
            int lo = 0, hi = SID_NODES - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (index[mid].hilbert_d == d) { lookup_count++; break; }
                if (index[mid].hilbert_d < d) lo = mid + 1;
                else hi = mid - 1;
            }
        }
        double t = now_sec() - t0;
        double ns = t * 1e9 / N_OPS;
        printf("\n  L-block index lookup (no I/O):\n");
        printf("    %8.1f ns/lookup = %7.2f M lookups/s (%d hits)\n",
               ns, 1000.0/ns, (int)lookup_count);
    }

    /* ── Cleanup ── */
    free(index);
    free(tmp);
    remove("/tmp/lblock_test.bin");

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("  BENCH COMPLETE\n");
    printf("══════════════════════════════════════════════════════════\n");
    return 0;
}

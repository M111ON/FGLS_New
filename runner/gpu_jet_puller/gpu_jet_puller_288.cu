/*
 * gpu_jet_puller_288.cu — GPU Jet Puller with 288-cell addressing
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Replaces RDH Ring-Wedge-Mirror with bridge_288:
 *   Old: (ring_group, wedge) → rdh_key → flat_key → byte_offset
 *   New: flat_key → bridge_288 → (face, direction, cell_pos) → byte_offset
 *
 * Both use 20736 slots = GEO_FULL = 12 × 6 × 288
 * The difference is in the addressing pattern:
 *   Old: RDH scatter across memory (random access)
 *   New: 288-cell structured layout (face-grouped, direction-sorted)
 *
 * Compile (CPU benchmark — no CUDA needed):
 *   gcc -O2 -Wall -o gpu_jet_puller_288_cpu gpu_jet_puller_288.cu -lm
 *
 * Compile (GPU — requires nvcc):
 *   nvcc -O2 -std=c++17 -arch=sm_75 -o gpu_jet_puller_288 gpu_jet_puller_288.cu -lm
 * ═══════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

/* ══════════════════════════════════════════════════════════════
   Constants
   ══════════════════════════════════════════════════════════════ */

#define GEO_FULL        20736
#define CELL_288        288
#define CELL_DIRS       6
#define DODECA_FACES    12
#define CELL_PER_FACE   1728
#define CHUNK_SZ        64u
#define STORE_SIZE      (GEO_FULL * CHUNK_SZ)   /* 1,327,104 bytes */
#define N_SWEEPS        4                        /* full GEO_FULL sweeps */

/* ══════════════════════════════════════════════════════════════
   bridge_288 — flat_key ↔ (face, direction, cell_pos)
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t face;       /* 0..11  */
    uint16_t direction;  /* 0..5   */
    uint16_t cell_pos;   /* 0..287 */
} Cell288Addr;

static inline Cell288Addr bridge_288(int64_t flat_key) {
    Cell288Addr a;
    a.face      = (uint16_t)((flat_key / CELL_PER_FACE) % DODECA_FACES);
    a.direction = (uint16_t)((flat_key / CELL_288) % CELL_DIRS);
    a.cell_pos  = (uint16_t)(flat_key % CELL_288);
    return a;
}

static inline int64_t bridge_288_key(Cell288Addr a) {
    return (int64_t)a.face * CELL_PER_FACE +
           (int64_t)a.direction * CELL_288 +
           (int64_t)a.cell_pos;
}

/* ══════════════════════════════════════════════════════════════
   stride-37 walk — full bijection on 1728
   ══════════════════════════════════════════════════════════════ */

static inline uint16_t stride37_next(uint16_t enc) {
    return (uint16_t)((enc + 37u) % 1728u);
}

static inline uint16_t stride37_enc(uint32_t t) {
    return (uint16_t)((uint64_t)t * 37u % 1728u);
}

/* ══════════════════════════════════════════════════════════════
   Data store — 20736 × 64B chunks
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  data[STORE_SIZE];
    uint32_t checksums[GEO_FULL];  /* XOR checksum per slot */
} JetStore;

static void jet_store_init(JetStore *store) {
    memset(store, 0, STORE_SIZE + GEO_FULL * sizeof(uint32_t));
    /* Write deterministic pattern */
    for (int64_t k = 0; k < GEO_FULL; k++) {
        uint64_t off = (uint64_t)k * CHUNK_SZ;
        uint8_t base = (uint8_t)(k & 0xFF);
        uint32_t ck = 0;
        for (uint32_t b = 0; b < CHUNK_SZ; b++) {
            uint8_t v = (uint8_t)((base + b) & 0xFF);
            store->data[off + b] = v;
            ck ^= v;
        }
        store->checksums[k] = ck;
    }
}

/* ══════════════════════════════════════════════════════════════
   Point Index — CPU builds, GPU reads
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t slot_id;
    uint64_t dram_offset;
    uint32_t size;
    uint32_t ref_checksum;
} PointIndexEntry;

typedef struct {
    PointIndexEntry entries[GEO_FULL];
    uint32_t        n_entries;
    uint32_t        epoch;
    uint32_t        bridge_count;
} PointIndexHeader;

/* ══════════════════════════════════════════════════════════════
   CPU kernel — pull chunks from store (simulates GPU)
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t n_pulls;
    uint32_t n_errors;
    double   kernel_us;
} PullResult;

static PullResult cpu_jet_pull(const PointIndexHeader *header,
                                const JetStore *store)
{
    PullResult r = {0, 0, 0};
    clock_t t0 = clock();

    for (uint32_t i = 0; i < header->n_entries; i++) {
        const PointIndexEntry *e = &header->entries[i];
        const uint8_t *chunk = store->data + e->dram_offset;

        /* XOR checksum */
        uint8_t ck = 0;
        for (uint32_t b = 0; b < e->size; b++)
            ck ^= chunk[b];

        if (ck != (uint8_t)(e->ref_checksum & 0xFF))
            r.n_errors++;
        r.n_pulls++;
    }

    clock_t t1 = clock();
    r.kernel_us = (double)(t1 - t0) / CLOCKS_PER_SEC * 1e6;
    return r;
}

/* ══════════════════════════════════════════════════════════════
   Index builders — 288-cell vs old RDH
   ══════════════════════════════════════════════════════════════ */

/* Build index using 288-cell bridge addressing */
static uint32_t build_index_288(PointIndexHeader *header,
                                 const JetStore *store,
                                 uint32_t epoch)
{
    header->n_entries = 0;
    header->epoch = epoch;

    for (int64_t k = 0; k < GEO_FULL; k++) {
        Cell288Addr ca = bridge_288(k);
        PointIndexEntry *e = &header->entries[header->n_entries];
        e->slot_id     = (uint32_t)k;
        e->dram_offset = (uint64_t)k * CHUNK_SZ;
        e->size        = CHUNK_SZ;
        e->ref_checksum = store->checksums[k];
        header->n_entries++;
    }

    header->bridge_count++;
    return header->n_entries;
}

/* Build index using stride-37 walk (old pattern) */
static uint32_t build_index_stride37(PointIndexHeader *header,
                                      const JetStore *store,
                                      uint32_t epoch)
{
    header->n_entries = 0;
    header->epoch = epoch;

    uint16_t enc = stride37_enc(epoch);
    for (int64_t k = 0; k < GEO_FULL; k++) {
        PointIndexEntry *e = &header->entries[header->n_entries];
        e->slot_id     = (uint32_t)k;
        e->dram_offset = (uint64_t)enc * CHUNK_SZ;
        e->size        = CHUNK_SZ;
        e->ref_checksum = store->checksums[enc];
        header->n_entries++;
        enc = stride37_next(enc);
    }

    header->bridge_count++;
    return header->n_entries;
}

/* ══════════════════════════════════════════════════════════════
   Memory layout analysis — 288-cell vs RDH
   ══════════════════════════════════════════════════════════════ */

static void analyze_memory_layout(void) {
    printf("\n=== Memory Layout: 288-cell vs RDH ===\n\n");

    /* 288-cell: face-grouped layout */
    printf("288-cell layout (face-grouped):\n");
    printf("  Face 0: slots 0..1727 (direction 0..5, cell 0..287)\n");
    printf("  Face 1: slots 1728..3455\n");
    printf("  ...\n");
    printf("  Face 11: slots 19008..20735\n");
    printf("  Total: 12 faces × 1728 slots × 64B = %.1f MB\n",
           12.0 * 1728 * 64 / 1048576);

    /* Coalescing: threads in same face access contiguous memory */
    printf("\n  GPU coalescing:\n");
    printf("    32 threads in same face → contiguous 2KB → 1 cache line\n");
    printf("    32 threads across faces → scattered → N cache lines\n");

    /* RDH: scattered layout */
    printf("\nRDH layout (Ring-Wedge-Mirror scattered):\n");
    printf("  (ring_group, wedge) → flat_key → offset\n");
    printf("  Neighbors in ring are NOT contiguous in memory\n");
    printf("  GPU coalescing: depends on thread ordering\n");

    /* Comparison */
    printf("\nComparison:\n");
    printf("  288-cell: face-local threads → HIGH coalescing\n");
    printf("  RDH:      random access       → LOW coalescing\n");
    printf("  Expected: 288-cell should be 2-4x faster for face-local access\n");
}

/* ══════════════════════════════════════════════════════════════
   Benchmark
   ══════════════════════════════════════════════════════════════ */

static void benchmark(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  GPU Jet Puller 288 — CPU Benchmark                 ║\n");
    printf("║  288-cell bridge vs stride-37 walk                  ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /* Init store */
    JetStore store;
    clock_t t0 = clock();
    jet_store_init(&store);
    clock_t t1 = clock();
    printf("Store init: %.3f ms (%.1f MB)\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC * 1000,
           STORE_SIZE / 1048576.0);

    /* Verify bridge_288 round-trip */
    int errors = 0;
    for (int64_t k = 0; k < GEO_FULL; k++) {
        Cell288Addr ca = bridge_288(k);
        int64_t k2 = bridge_288_key(ca);
        if (k2 != k) errors++;
    }
    printf("bridge_288 round-trip: %s (%d errors)\n",
           errors == 0 ? "PASS" : "FAIL", errors);

    /* Benchmark: 288-cell addressing */
    PointIndexHeader header_288;
    t0 = clock();
    build_index_288(&header_288, &store, 0);
    PullResult r288 = cpu_jet_pull(&header_288, &store);
    t1 = clock();
    double build_288_us = (double)(t1 - t0) / CLOCKS_PER_SEC * 1e6;

    /* Benchmark: stride-37 addressing */
    PointIndexHeader header_s37;
    t0 = clock();
    build_index_stride37(&header_s37, &store, 0);
    PullResult r37 = cpu_jet_pull(&header_s37, &store);
    t1 = clock();
    double build_s37_us = (double)(t1 - t0) / CLOCKS_PER_SEC * 1e6;

    /* Full sweep: 288-cell */
    t0 = clock();
    double total_288_us = 0;
    for (uint32_t sw = 0; sw < N_SWEEPS; sw++) {
        build_index_288(&header_288, &store, sw);
        PullResult r = cpu_jet_pull(&header_288, &store);
        total_288_us += r.kernel_us;
    }
    t1 = clock();
    double wall_288 = (double)(t1 - t0) / CLOCKS_PER_SEC * 1e6;

    /* Full sweep: stride-37 */
    t0 = clock();
    double total_s37_us = 0;
    for (uint32_t sw = 0; sw < N_SWEEPS; sw++) {
        build_index_stride37(&header_s37, &store, sw);
        PullResult r = cpu_jet_pull(&header_s37, &store);
        total_s37_us += r.kernel_us;
    }
    t1 = clock();
    double wall_s37 = (double)(t1 - t0) / CLOCKS_PER_SEC * 1e6;

    /* Results */
    printf("\n=== Single Bridge Results ===\n");
    printf("  288-cell: %u pulls, %u errors, %.1f µs (build %.1f µs)\n",
           r288.n_pulls, r288.n_errors, r288.kernel_us, build_288_us);
    printf("  Stride37: %u pulls, %u errors, %.1f µs (build %.1f µs)\n",
           r37.n_pulls, r37.n_errors, r37.kernel_us, build_s37_us);

    printf("\n=== Full Sweep (%d sweeps × %d slots) ===\n", N_SWEEPS, GEO_FULL);
    printf("  288-cell: %.1f ms total, %.1f µs/sweep\n",
           wall_288 / 1000, wall_288 / N_SWEEPS);
    printf("  Stride37: %.1f ms total, %.1f µs/sweep\n",
           wall_s37 / 1000, wall_s37 / N_SWEEPS);

    double total_data_gb = (double)N_SWEEPS * GEO_FULL * CHUNK_SZ / 1e9;
    printf("\n  Total data: %.3f GB\n", total_data_gb);
    printf("  288-cell BW: %.2f GB/s\n",
           total_data_gb / (wall_288 / 1e6));
    printf("  Stride37 BW: %.2f GB/s\n",
           total_data_gb / (wall_s37 / 1e6));
    printf("  Speedup:     %.2fx\n", wall_s37 / wall_288);

    /* Memory layout analysis */
    analyze_memory_layout();

    printf("\n=== BENCHMARK COMPLETE ===\n");
}

/* ══════════════════════════════════════════════════════════════
   Main
   ══════════════════════════════════════════════════════════════ */

int main(void) {
    benchmark();
    return 0;
}

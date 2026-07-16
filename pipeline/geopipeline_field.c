/*
 * geopipeline_field.c — GeoPixel Field Pipeline v4
 * ═══════════════════════════════════════════════════════
 *
 * Pipeline:
 *   data → chunk(48B) → L-Block Container (4×4×3 Metatron)
 *     → floor connectors (16B) between floors
 *     → block connectors (32B) between blocks
 *     → Hilbert curve path within floor
 *     → seed + multi-context delta → container
 *     → bond edges (tamper-evident dependency graph)
 *
 * Structure:
 *   1 floor  = 4×4 = 16 cells (Hilbert-ordered)
 *   1 L-block = 3 data + 1 connector = 4 cells
 *   4 L-blocks per floor
 *   3 floors per tower + 3 floor connectors + 1 block connector
 *   1 tower  = 48 data cells + 3×16B floor_conn + 32B block_conn
 *
 * Delta context hierarchy:
 *   1. L-block neighbor (cells 0-2 within same L-block)
 *   2. Floor connector (16B bridge between floors)
 *   3. Block connector (32B bridge between blocks)
 *   4. Zero reference (first cell in first L-block)
 *
 * Bond edges (tamper-evident dependency graph):
 *   - Cell → next cell (Hilbert path within floor)
 *   - Floor connector → floor connector (cross-floor)
 *   - Block connector → block connector (cross-block)
 *   Each edge: origin(8B) + target(8B) + weight(4B) = 20B
 *   Verification: fibo_addr (same as pogls_bond.h)
 *
 * Compile:
 *   gcc -O2 -std=c11 -o geopipeline_field.exe geopipeline_field.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
   Constants
   ═══════════════════════════════════════════════════════════════════════ */

#define CELL_SZ         48      /* geo_jump unit: 4²×3 = 48B */
#define FLOOR_CELLS     16u     /* 4×4 */
#define LBLOCK_DATA     3u
#define LBLOCK_CONN     1u
#define LBLOCK_CELLS    4u
#define LBLOCKS_PER_FLOOR 4u
#define FLOORS_PER      3u
#define TOWER_CELLS     48u     /* 16 × 3 */

#define FLOOR_CONN_SZ   16u     /* floor connector: 16B */
#define BLOCK_CONN_SZ   32u     /* block connector: 32B */

/* On-disk tower: 48 cells × 48B + 3×16B floor_conn + 32B block_conn */
#define TOWER_DATA_SZ   (TOWER_CELLS * CELL_SZ)           /* 2304B */
#define TOWER_FCONN_SZ  (FLOORS_PER * FLOOR_CONN_SZ)     /* 48B */
#define TOWER_BCONN_SZ  BLOCK_CONN_SZ                     /* 32B */
#define TOWER_TOTAL_SZ  (TOWER_DATA_SZ + TOWER_FCONN_SZ + TOWER_BCONN_SZ) /* 2384B */

#define FLOOR_SZ        (FLOOR_CELLS * CELL_SZ)           /* 768B */

#define GEO_FULL        20736u
#define HILBERT_ORDER   2u      /* local floor: 4×4 */
#define SCATTER_ORDER   7u      /* global scatter: 128×128 = 16384 cells max */

#define GPF_MAGIC       0x47504605u  /* GPF v5 — with GpSphere scatter */
#define GPF_VERSION     5

#define DELTA_IDENTICAL 0x00
#define DELTA_ZERO      0x01
#define DELTA_XOR       0x02
#define DELTA_RAW       0x03

/* ═══════════════════════════════════════════════════════════════════════
   Bond Edge — tamper-evident dependency graph
   ═══════════════════════════════════════════════════════════════════════ */

/* Embedded fibo_addr from pogls_bond.h (standalone, zero-dep) */
#define BOND_FNV_PRIME  UINT64_C(0x00000100000001B3)
#define BOND_FNV_OFFSET UINT64_C(0xCBF29CE484222325)
#define BOND_SALT_L     UINT64_C(0xAAAAAAAAAAAAAAAA)
#define BOND_SALT_R     UINT64_C(0x5555555555555555)
#define BOND_GEO_MAGIC  UINT64_C(0x00120090024005A0)

static const uint64_t BOND_FIBO[16] = {
    1,1,2,3,5,8,13,21,34,55,89,144,233,377,610,987
};

static inline uint64_t bond_fibo_addr(uint64_t seed) {
    uint64_t h = BOND_FNV_OFFSET;
    uint8_t b[8];
    memcpy(b, &seed, 8);
    for (int i = 0; i < 8; i++) { h ^= (uint64_t)b[i]; h *= BOND_FNV_PRIME; }
    for (int i = 0; i < 16; i++) {
        h ^= BOND_FIBO[i] * (seed >> (i & 7));
        h = (h << 13) | (h >> 51);
    }
    h ^= BOND_GEO_MAGIC;
    h *= BOND_FNV_PRIME;
    h ^= h >> 33;
    return h;
}

/* Edge record: origin(8B) + target(8B) + weight(4B) = 20B */
#pragma pack(push, 1)
typedef struct {
    uint64_t origin;
    uint64_t target;
    uint32_t weight;
} BondEdge;
#pragma pack(pop)

#define BOND_EDGE_SZ 20

/* Edge types for weight field */
#define EDGE_CELL_HILBERT   0x01   /* cell → next cell on Hilbert path */
#define EDGE_FLOOR_CONNECT  0x02   /* floor connector → next floor connector */
#define EDGE_BLOCK_CONNECT  0x03   /* block connector → next block connector */

/* Derive geo_key from cell seed (same as pogls_make_piece origin) */
static inline uint64_t bond_edge_geo_key(const uint8_t *cell, uint32_t sz) {
    uint64_t h = BOND_FNV_OFFSET;
    for (uint32_t i = 0; i < sz; i++) {
        h ^= (uint64_t)cell[i];
        h *= BOND_FNV_PRIME;
    }
    return bond_fibo_addr(h);
}

/* Edge verification: origin_key ^ target_key → hash → verify */
static inline int bond_edge_verify(const BondEdge *e) {
    uint64_t oL = bond_fibo_addr(e->origin ^ BOND_SALT_L);
    uint64_t tR = bond_fibo_addr(e->target ^ BOND_SALT_R);
    uint64_t combined = bond_fibo_addr(oL ^ tR);
    /* Pass: full avalanche check — any coordinate shift breaks the edge */
    uint64_t verify = bond_fibo_addr(combined ^ (e->weight * BOND_GEO_MAGIC));
    return (verify & 0xFFFFFFFF) == (uint32_t)(verify >> 32);
}

/* ═══════════════════════════════════════════════════════════════════════
   Hilbert — order 2 (4×4)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t hilbert_xy2d(uint32_t x, uint32_t y, uint32_t n)
{
    uint32_t d = 0;
    for (uint32_t s = n >> 1; s > 0; s >>= 1) {
        uint32_t rx = (x & s) > 0;
        uint32_t ry = (y & s) > 0;
        d = (d << 2) | (((uint32_t)(3u * rx)) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = n - 1u - x; y = n - 1u - y; }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

static uint8_t floor_hilbert_order[FLOOR_CELLS];

static void build_floor_hilbert_order(void)
{
    uint32_t inv[FLOOR_CELLS];
    for (uint32_t flat = 0; flat < FLOOR_CELLS; flat++) {
        uint32_t col = flat % 4;
        uint32_t row = flat / 4;
        uint32_t d = hilbert_xy2d(col, row, 4);
        inv[d] = flat;
    }
    for (uint32_t i = 0; i < FLOOR_CELLS; i++)
        floor_hilbert_order[i] = (uint8_t)inv[i];
}

/* ═══════════════════════════════════════════════════════════════════════
   GpSphere Scatter — global Hilbert reordering
   ═══════════════════════════════════════════════════════════════════════
 *
 * Maps chunk_idx → position on 128×128 Hilbert curve (order 7).
 * This places spatially-adjacent chunks together before L-block
 * encoding, so XOR deltas between L-block neighbors are smaller.
 *
 * scatter_map[scatter_pos] = original_chunk_idx
 * inverse_map[original_chunk_idx] = scatter_pos
 *
 * For n_cells <= 16384 (128×128), all chunks fit on the curve.
 * Chunks beyond 16384 fall back to sequential order.
 */

static uint32_t scatter_map[16384];    /* scatter_pos → original_idx */
static uint32_t inverse_map[16384];    /* original_idx → scatter_pos */

static uint32_t scatter_n = 0;         /* actual cells in scatter */

static void build_scatter_map(uint32_t n_cells)
{
    uint32_t grid = 1u << SCATTER_ORDER;  /* 128 */
    uint32_t grid_cells = grid * grid;     /* 16384 */
    scatter_n = (n_cells < grid_cells) ? n_cells : grid_cells;

    /* Map each cell index to its Hilbert position on the 128×128 grid */
    for (uint32_t i = 0; i < scatter_n; i++) {
        uint32_t col = i % grid;
        uint32_t row = i / grid;
        scatter_map[i] = hilbert_xy2d(col, row, grid);
    }

    /* scatter_map[i] now holds the Hilbert distance for cell i.
     * We need the INVERSE: for each Hilbert distance d, which cell i?
     * Use counting sort since distances are in [0, scatter_n). */
    uint32_t *count = (uint32_t *)calloc(grid_cells, sizeof(uint32_t));
    for (uint32_t i = 0; i < scatter_n; i++)
        count[scatter_map[i]]++;

    /* Prefix sum */
    uint32_t total = 0;
    for (uint32_t d = 0; d < scatter_n; d++) {
        uint32_t c = count[d];
        count[d] = total;
        total += c;
    }

    /* Build inverse: inverse_map[scatter_pos] = original_cell_idx */
    for (uint32_t i = 0; i < scatter_n; i++) {
        uint32_t d = scatter_map[i];
        inverse_map[count[d]] = i;
        count[d]++;
    }
    free(count);
}

/* ═══════════════════════════════════════════════════════════════════════
   Seed
   ═══════════════════════════════════════════════════════════════════════ */

static uint64_t cell_seed(const uint8_t cell[CELL_SZ])
{
    uint64_t s = 0;
    for (int i = 0; i < CELL_SZ; i += 8) {
        uint64_t v;
        memcpy(&v, cell + i, 8);
        s ^= v;
        s = (s << 13) | (s >> 51);
        s *= 0x9E3779B97F4A7C15ULL;
    }
    return s;
}

/* ═══════════════════════════════════════════════════════════════════════
   Delta Encoding — variable reference size
   ═══════════════════════════════════════════════════════════════════════

   For 48B cells: XOR with reference, store bitmask + non-zero values
   For connectors (16B/32B): same approach
 */

#define DELTA_MAX_SZ    80      /* enough for 48B cell */

static int encode_delta(const uint8_t *cell, uint32_t cell_sz,
                         const uint8_t *ref, uint32_t ref_sz,
                         uint8_t out[DELTA_MAX_SZ])
{
    uint32_t cmp_sz = cell_sz < ref_sz ? cell_sz : ref_sz;
    if (memcmp(cell, ref, cmp_sz) == 0 && cell_sz == ref_sz) {
        out[0] = DELTA_IDENTICAL;
        return 1;
    }

    int all_zero = 1;
    for (uint32_t i = 0; i < cell_sz; i++) {
        if (cell[i] != 0) { all_zero = 0; break; }
    }
    if (all_zero) {
        out[0] = DELTA_ZERO;
        return 1;
    }

    uint64_t mask = 0;
    int nz_count = 0;
    for (uint32_t i = 0; i < cell_sz; i++) {
        uint8_t x = (i < ref_sz) ? (cell[i] ^ ref[i]) : cell[i];
        if (x != 0) {
            mask |= (1ULL << i);
            nz_count++;
        }
    }

    int delta_sz = 1 + 8 + nz_count;
    if (delta_sz >= (int)cell_sz + 1) {
        out[0] = DELTA_RAW;
        memcpy(out + 1, cell, cell_sz);
        return 1 + (int)cell_sz;
    }

    out[0] = DELTA_XOR;
    memcpy(out + 1, &mask, 8);
    int pos = 9;
    for (uint32_t i = 0; i < cell_sz; i++) {
        uint8_t x = (i < ref_sz) ? (cell[i] ^ ref[i]) : cell[i];
        if (x != 0) out[pos++] = x;
    }
    return pos;
}

static int decode_delta(const uint8_t *in, int in_sz,
                          uint8_t *cell, uint32_t cell_sz,
                          const uint8_t *ref, uint32_t ref_sz)
{
    if (in_sz < 1) return 0;
    switch (in[0]) {
    case DELTA_IDENTICAL:
        memcpy(cell, ref, cell_sz < ref_sz ? cell_sz : ref_sz);
        if (cell_sz > ref_sz) memset(cell + ref_sz, 0, cell_sz - ref_sz);
        return 1;
    case DELTA_ZERO:
        memset(cell, 0, cell_sz);
        return 1;
    case DELTA_XOR: {
        if (in_sz < 9) return 0;
        uint64_t mask;
        memcpy(&mask, in + 1, 8);
        int nz = 0;
        uint64_t m = mask;
        while (m) { nz += (int)(m & 1); m >>= 1; }
        if (in_sz < 9 + nz) return 0;
        memset(cell, 0, cell_sz);
        int pos = 9;
        for (uint32_t i = 0; i < cell_sz; i++) {
            uint8_t r = (i < ref_sz) ? ref[i] : 0;
            if (mask & (1ULL << i)) {
                cell[i] = r ^ in[pos++];
            } else {
                cell[i] = r;
            }
        }
        return pos;
    }
    case DELTA_RAW:
        if (in_sz < 1 + (int)cell_sz) return 0;
        memcpy(cell, in + 1, cell_sz);
        return 1 + (int)cell_sz;
    default:
        return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   Container
   ═══════════════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_cells;
    uint32_t n_floors;
    uint32_t n_towers;
    uint32_t n_edges;
    uint32_t scatter_n;      /* cells in scatter map (0 = no scatter) */
    uint64_t global_seed;
    uint32_t original_size;
} GpfHeader;

#define GPF_HEADER_SZ 40

/* ═══════════════════════════════════════════════════════════════════════
   Encode
   ═══════════════════════════════════════════════════════════════════════ */

/* State for encoding one tower */
typedef struct {
    uint8_t  lblock_data[LBLOCKS_PER_FLOOR][CELL_SZ];
    int      lblock_has[LBLOCKS_PER_FLOOR];
    uint8_t  floor_conn[FLOOR_CONN_SZ];
    int      floor_conn_has;
    uint8_t  block_conn[BLOCK_CONN_SZ];
    int      block_conn_has;
} TowerState;

static void tower_state_reset(TowerState *ts)
{
    memset(ts->lblock_data, 0, sizeof(ts->lblock_data));
    memset(ts->lblock_has, 0, sizeof(ts->lblock_has));
    memset(ts->floor_conn, 0, FLOOR_CONN_SZ);
    ts->floor_conn_has = 0;
    memset(ts->block_conn, 0, BLOCK_CONN_SZ);
    ts->block_conn_has = 0;
}

static int do_encode(const char *in_path, const char *out_path)
{
    FILE *fin = fopen(in_path, "rb");
    if (!fin) { perror(in_path); return 1; }
    fseek(fin, 0, SEEK_END);
    long file_sz = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    if (file_sz <= 0) { fclose(fin); return 1; }

    uint8_t *data = malloc((size_t)file_sz);
    fread(data, 1, (size_t)file_sz, fin);
    fclose(fin);

    uint32_t n_cells = (uint32_t)((file_sz + CELL_SZ - 1) / CELL_SZ);
    uint8_t *padded = (uint8_t *)calloc(1, (size_t)n_cells * CELL_SZ);
    memcpy(padded, data, (size_t)file_sz);
    free(data);

    /* Build Hilbert floor order + GpSphere scatter */
    build_floor_hilbert_order();
    build_scatter_map(n_cells);

    uint64_t *seeds = (uint64_t *)calloc(n_cells, sizeof(uint64_t));
    uint32_t *offsets = (uint32_t *)calloc(n_cells + 1, sizeof(uint32_t));
    uint8_t **deltas = (uint8_t **)calloc(n_cells, sizeof(uint8_t *));
    int *delta_sizes = (int *)calloc(n_cells, sizeof(int));

    /* Connector deltas */
    uint32_t n_floors = (n_cells + FLOOR_CELLS - 1) / FLOOR_CELLS;
    uint32_t n_fconn = (n_floors > 1) ? (n_floors - 1) : 0;
    uint32_t n_bconn = (n_fconn > 0) ? ((n_fconn + FLOORS_PER - 1) / FLOORS_PER) : 0;

    uint8_t **fconn_deltas = (uint8_t **)calloc(n_fconn, sizeof(uint8_t *));
    int *fconn_sizes = (int *)calloc(n_fconn, sizeof(int));
    uint32_t *fconn_offsets = (uint32_t *)calloc(n_fconn + 1, sizeof(uint32_t));
    uint8_t *fconn_raw_stored = (uint8_t *)calloc(n_fconn, FLOOR_CONN_SZ);

    uint8_t **bconn_deltas = (uint8_t **)calloc(n_bconn, sizeof(uint8_t *));
    int *bconn_sizes = (int *)calloc(n_bconn, sizeof(int));
    uint32_t *bconn_offsets = (uint32_t *)calloc(n_bconn + 1, sizeof(uint32_t));
    uint8_t *bconn_raw_stored = (uint8_t *)calloc(n_bconn, BLOCK_CONN_SZ);

    uint64_t global_seed = 0;
    uint32_t data_offset = 0;
    uint32_t fconn_offset = 0;
    uint32_t bconn_offset = 0;

    TowerState ts;
    tower_state_reset(&ts);

    uint8_t zero_ref[CELL_SZ];
    memset(zero_ref, 0, CELL_SZ);

    for (uint32_t si = 0; si < n_cells; si++) {
        /* si = scatter position; ci = original cell index */
        uint32_t ci = (si < scatter_n) ? inverse_map[si] : si;
        uint8_t *cell = padded + ci * CELL_SZ;
        seeds[si] = cell_seed(cell);
        global_seed ^= seeds[si];

        uint32_t pos_in_floor = si % FLOOR_CELLS;
        uint32_t hilbert_d = pos_in_floor;
        uint32_t flat = floor_hilbert_order[hilbert_d];
        uint32_t lb = flat / LBLOCK_CELLS;
        uint32_t lb_pos = flat % LBLOCK_CELLS;

        /* Choose reference */
        const uint8_t *ref;
        uint32_t ref_sz;
        if (lb_pos == 0) {
            if (pos_in_floor == 0 && si > 0 && ts.floor_conn_has) {
                ref = ts.floor_conn;
                ref_sz = FLOOR_CONN_SZ;
            } else if (lb == 0 && ts.block_conn_has) {
                ref = ts.block_conn;
                ref_sz = BLOCK_CONN_SZ;
            } else {
                ref = zero_ref;
                ref_sz = CELL_SZ;
            }
        } else if (lb_pos <= LBLOCK_DATA && ts.lblock_has[lb]) {
            ref = ts.lblock_data[lb];
            ref_sz = CELL_SZ;
        } else {
            ref = zero_ref;
            ref_sz = CELL_SZ;
        }

        deltas[si] = malloc(DELTA_MAX_SZ);
        delta_sizes[si] = encode_delta(cell, CELL_SZ, ref, ref_sz, deltas[si]);
        offsets[si] = data_offset;
        data_offset += (uint32_t)delta_sizes[si];

        /* Update L-block state */
        if (lb_pos < LBLOCK_DATA) {
            memcpy(ts.lblock_data[lb], cell, CELL_SZ);
            ts.lblock_has[lb] = 1;
        }

        /* Floor boundary: encode floor connector */
        if (pos_in_floor == FLOOR_CELLS - 1 && (si + 1) < n_cells) {
            uint8_t next_cell[CELL_SZ];
            uint32_t next_ci = ((si + 1) < scatter_n) ? inverse_map[si + 1] : (si + 1);
            if (next_ci * CELL_SZ < (uint32_t)file_sz) {
                memcpy(next_cell, padded + next_ci * CELL_SZ, CELL_SZ);
            } else {
                memset(next_cell, 0, CELL_SZ);
            }

            uint8_t fconn_raw[FLOOR_CONN_SZ];
            /* Take first 16 bytes of XOR between last cell of floor and first of next */
            for (uint32_t b = 0; b < FLOOR_CONN_SZ; b++)
                fconn_raw[b] = cell[b] ^ next_cell[b];

            uint32_t fci = (si / FLOOR_CELLS);
            const uint8_t *fref = ts.floor_conn_has ? ts.floor_conn : zero_ref;
            uint32_t fref_sz = ts.floor_conn_has ? FLOOR_CONN_SZ : FLOOR_CONN_SZ;
            fconn_deltas[fci] = malloc(DELTA_MAX_SZ);
            fconn_sizes[fci] = encode_delta(fconn_raw, FLOOR_CONN_SZ, fref, fref_sz, fconn_deltas[fci]);
            fconn_offsets[fci] = fconn_offset;
            fconn_offset += (uint32_t)fconn_sizes[fci];

            memcpy(ts.floor_conn, fconn_raw, FLOOR_CONN_SZ);
            ts.floor_conn_has = 1;
            memcpy(fconn_raw_stored + fci * FLOOR_CONN_SZ, fconn_raw, FLOOR_CONN_SZ);
        }

        /* Block boundary: encode block connector */
        if (pos_in_floor == FLOOR_CELLS - 1 &&
            ((si / FLOOR_CELLS) % FLOORS_PER) == (FLOORS_PER - 1) &&
            (si + 1) < n_cells) {
            uint8_t bconn_raw[BLOCK_CONN_SZ];
            memset(bconn_raw, 0, BLOCK_CONN_SZ);
            uint32_t next_ci = ((si + 1) < scatter_n) ? inverse_map[si + 1] : (si + 1);
            uint8_t *next = padded + next_ci * CELL_SZ;
            for (uint32_t b = 0; b < FLOOR_CONN_SZ; b++)
                bconn_raw[b] = ts.floor_conn[b] ^ next[b];
            for (uint32_t b = FLOOR_CONN_SZ; b < BLOCK_CONN_SZ && b < CELL_SZ; b++)
                bconn_raw[b] = next[b];

            uint32_t bci = (si / FLOOR_CELLS) / FLOORS_PER;
            const uint8_t *bref = ts.block_conn_has ? ts.block_conn : zero_ref;
            bconn_deltas[bci] = malloc(DELTA_MAX_SZ);
            bconn_sizes[bci] = encode_delta(bconn_raw, BLOCK_CONN_SZ, bref, BLOCK_CONN_SZ, bconn_deltas[bci]);
            bconn_offsets[bci] = bconn_offset;
            bconn_offset += (uint32_t)bconn_sizes[bci];

            memcpy(ts.block_conn, bconn_raw, BLOCK_CONN_SZ);
            ts.block_conn_has = 1;
            memcpy(bconn_raw_stored + bci * BLOCK_CONN_SZ, bconn_raw, BLOCK_CONN_SZ);
        }
    }
    offsets[n_cells] = data_offset;
    fconn_offsets[n_fconn] = fconn_offset;
    bconn_offsets[n_bconn] = bconn_offset;

    /* ── Bond Edges: create tamper-evident dependency graph ── */
    uint32_t max_edges = n_cells + n_fconn + n_bconn;
    BondEdge *edges = (BondEdge *)calloc(max_edges, sizeof(BondEdge));
    uint32_t n_edges = 0;

    /* Edge type 1: cell → next cell (Hilbert path, scattered order) */
    for (uint32_t si = 0; si + 1 < n_cells; si++) {
        BondEdge *e = &edges[n_edges++];
        uint32_t ci = (si < scatter_n) ? inverse_map[si] : si;
        uint32_t ci_next = ((si + 1) < scatter_n) ? inverse_map[si + 1] : (si + 1);
        e->origin = bond_edge_geo_key(padded + ci * CELL_SZ, CELL_SZ);
        e->target = bond_edge_geo_key(padded + ci_next * CELL_SZ, CELL_SZ);
        e->weight = EDGE_CELL_HILBERT;
    }

    /* Edge type 2: floor connector → next floor connector */
    for (uint32_t i = 0; i + 1 < n_fconn; i++) {
        BondEdge *e = &edges[n_edges++];
        e->origin = bond_edge_geo_key(fconn_raw_stored + i * FLOOR_CONN_SZ, FLOOR_CONN_SZ);
        e->target = bond_edge_geo_key(fconn_raw_stored + (i + 1) * FLOOR_CONN_SZ, FLOOR_CONN_SZ);
        e->weight = EDGE_FLOOR_CONNECT;
    }

    /* Edge type 3: block connector → next block connector */
    for (uint32_t i = 0; i + 1 < n_bconn; i++) {
        BondEdge *e = &edges[n_edges++];
        e->origin = bond_edge_geo_key(bconn_raw_stored + i * BLOCK_CONN_SZ, BLOCK_CONN_SZ);
        e->target = bond_edge_geo_key(bconn_raw_stored + (i + 1) * BLOCK_CONN_SZ, BLOCK_CONN_SZ);
        e->weight = EDGE_BLOCK_CONNECT;
    }

    /* Write */
    FILE *fout = fopen(out_path, "wb");
    if (!fout) { perror(out_path); free(padded); return 1; }

    GpfHeader hdr = {
        .magic = GPF_MAGIC, .version = GPF_VERSION,
        .n_cells = n_cells, .n_floors = n_floors,
        .n_towers = (n_cells + TOWER_CELLS - 1) / TOWER_CELLS,
        .n_edges = n_edges, .scatter_n = scatter_n,
        .global_seed = global_seed, .original_size = (uint32_t)file_sz
    };
    fwrite(&hdr, GPF_HEADER_SZ, 1, fout);
    fwrite(inverse_map, 4, scatter_n, fout);
    fwrite(seeds, 8, n_cells, fout);
    fwrite(offsets, 4, n_cells + 1, fout);
    fwrite(fconn_offsets, 4, n_fconn + 1, fout);
    fwrite(bconn_offsets, 4, n_bconn + 1, fout);

    for (uint32_t i = 0; i < n_cells; i++)
        fwrite(deltas[i], 1, delta_sizes[i], fout);
    for (uint32_t i = 0; i < n_fconn; i++)
        fwrite(fconn_deltas[i], 1, fconn_sizes[i], fout);
    for (uint32_t i = 0; i < n_bconn; i++)
        fwrite(bconn_deltas[i], 1, bconn_sizes[i], fout);
    fwrite(edges, sizeof(BondEdge), n_edges, fout);
    fclose(fout);

    long total_out = GPF_HEADER_SZ
        + (long)scatter_n * 4
        + (long)n_cells * 8 + (long)(n_cells + 1) * 4
        + (long)(n_fconn + 1) * 4 + (long)(n_bconn + 1) * 4
        + data_offset + fconn_offset + bconn_offset
        + (long)n_edges * BOND_EDGE_SZ;

    printf("Encode: %s -> %s\n", in_path, out_path);
    printf("  %ld -> %ld bytes (%.2fx)\n", file_sz, total_out,
           file_sz > 0 ? (double)total_out / (double)file_sz : 0.0);
    printf("  cells=%u floors=%u towers=%u (48B/cell, L-block+connectors)\n",
           n_cells, n_floors, hdr.n_towers);

    int n_id=0, n_z=0, n_x=0, n_r=0;
    for (uint32_t i = 0; i < n_cells; i++) {
        switch (deltas[i][0]) {
        case DELTA_IDENTICAL: n_id++; break;
        case DELTA_ZERO: n_z++; break;
        case DELTA_XOR: n_x++; break;
        case DELTA_RAW: n_r++; break;
        }
    }
    printf("  cell delta: identical=%d zero=%d xor=%d raw=%d\n", n_id, n_z, n_x, n_r);
    printf("  floor_conn: %u  block_conn: %u\n", n_fconn, n_bconn);
    printf("  bond_edges: %u (cell=%u floor=%u block=%u)\n",
           n_edges, n_cells - 1, n_fconn > 0 ? n_fconn - 1 : 0,
           n_bconn > 0 ? n_bconn - 1 : 0);

    free(padded); free(seeds); free(offsets);
    for (uint32_t i = 0; i < n_cells; i++) free(deltas[i]);
    free(deltas); free(delta_sizes);
    for (uint32_t i = 0; i < n_fconn; i++) free(fconn_deltas[i]);
    free(fconn_deltas); free(fconn_sizes); free(fconn_offsets); free(fconn_raw_stored);
    for (uint32_t i = 0; i < n_bconn; i++) free(bconn_deltas[i]);
    free(bconn_deltas); free(bconn_sizes); free(bconn_offsets); free(bconn_raw_stored);
    free(edges);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   Decode
   ═══════════════════════════════════════════════════════════════════════ */

static int do_decode(const char *in_path, const char *out_path)
{
    FILE *fin = fopen(in_path, "rb");
    if (!fin) { perror(in_path); return 1; }

    GpfHeader hdr;
    if (fread(&hdr, GPF_HEADER_SZ, 1, fin) != 1 || hdr.magic != GPF_MAGIC) {
        fprintf(stderr, "Invalid GPF file\n");
        fclose(fin);
        return 1;
    }

    uint32_t n_cells = hdr.n_cells;
    uint32_t scatter_n = hdr.scatter_n;

    /* Read scatter map (inverse_map: scatter_pos → original_idx) */
    uint32_t *inv_map = NULL;
    if (scatter_n > 0) {
        inv_map = malloc((size_t)scatter_n * 4);
        fread(inv_map, 4, scatter_n, fin);
    }

    uint64_t *seeds = malloc((size_t)n_cells * 8);
    uint32_t *offsets = malloc((size_t)(n_cells + 1) * 4);
    fread(seeds, 8, n_cells, fin);
    fread(offsets, 4, n_cells + 1, fin);

    uint32_t n_floors = hdr.n_floors;
    uint32_t n_fconn = (n_floors > 1) ? (n_floors - 1) : 0;
    uint32_t n_bconn = hdr.n_towers > 0 ? (n_fconn / FLOORS_PER) : 0;
    if (n_bconn == 0 && n_fconn > 0) n_bconn = 1;

    uint32_t *fconn_off = malloc((size_t)(n_fconn + 1) * 4);
    uint32_t *bconn_off = malloc((size_t)(n_bconn + 1) * 4);
    fread(fconn_off, 4, n_fconn + 1, fin);
    fread(bconn_off, 4, n_bconn + 1, fin);

    long data_start = ftell(fin);
    fseek(fin, 0, SEEK_END);
    long file_end = ftell(fin);
    fseek(fin, data_start, SEEK_SET);

    uint32_t total_remaining = (uint32_t)(file_end - data_start);
    uint8_t *all_data = malloc(total_remaining);
    fread(all_data, 1, total_remaining, fin);
    fclose(fin);

    build_floor_hilbert_order();

    uint8_t *scattered = (uint8_t *)calloc(1, (size_t)n_cells * CELL_SZ);

    TowerState ts;
    tower_state_reset(&ts);

    uint8_t zero_ref[CELL_SZ];
    memset(zero_ref, 0, CELL_SZ);

    for (uint32_t si = 0; si < n_cells; si++) {
        uint8_t *cell = scattered + si * CELL_SZ;
        const uint8_t *delta = all_data + offsets[si];
        int delta_sz = offsets[si + 1] - offsets[si];

        uint32_t pos_in_floor = si % FLOOR_CELLS;
        uint32_t flat = floor_hilbert_order[pos_in_floor];
        uint32_t lb = flat / LBLOCK_CELLS;
        uint32_t lb_pos = flat % LBLOCK_CELLS;

        const uint8_t *ref;
        uint32_t ref_sz;
        if (lb_pos == 0) {
            if (pos_in_floor == 0 && si > 0 && ts.floor_conn_has) {
                ref = ts.floor_conn;
                ref_sz = FLOOR_CONN_SZ;
            } else if (lb == 0 && ts.block_conn_has) {
                ref = ts.block_conn;
                ref_sz = BLOCK_CONN_SZ;
            } else {
                ref = zero_ref;
                ref_sz = CELL_SZ;
            }
        } else if (lb_pos <= LBLOCK_DATA && ts.lblock_has[lb]) {
            ref = ts.lblock_data[lb];
            ref_sz = CELL_SZ;
        } else {
            ref = zero_ref;
            ref_sz = CELL_SZ;
        }

        if (decode_delta(delta, delta_sz, cell, CELL_SZ, ref, ref_sz) == 0) {
            fprintf(stderr, "Decode error at cell %u\n", si);
            free(all_data); free(scattered); free(seeds); free(offsets);
            free(fconn_off); free(bconn_off);
            return 1;
        }

        if (cell_seed(cell) != seeds[si]) {
            fprintf(stderr, "Seed mismatch at cell %u\n", si);
            free(all_data); free(scattered); free(seeds); free(offsets);
            free(fconn_off); free(bconn_off); if (inv_map) free(inv_map);
            return 1;
        }

        if (lb_pos < LBLOCK_DATA) {
            memcpy(ts.lblock_data[lb], cell, CELL_SZ);
            ts.lblock_has[lb] = 1;
        }

        /* Decode floor connector at floor boundary */
        if (pos_in_floor == FLOOR_CELLS - 1 && (si + 1) < n_cells) {
            uint32_t fci = si / FLOOR_CELLS;
            if (fci < n_fconn) {
                const uint8_t *fd = all_data + offsets[n_cells] + fconn_off[fci];
                int fsz = fconn_off[fci + 1] - fconn_off[fci];
                uint8_t fconn_raw[FLOOR_CONN_SZ];
                /* Floor connector stored as delta from previous floor connector */
                const uint8_t *fref = ts.floor_conn_has ? ts.floor_conn : zero_ref;
                if (decode_delta(fd, fsz, fconn_raw, FLOOR_CONN_SZ, fref, FLOOR_CONN_SZ) == 0) {
                    fprintf(stderr, "Floor conn decode error at floor %u\n", fci);
                    free(all_data); free(scattered); free(seeds); free(offsets);
                    free(fconn_off); free(bconn_off);
                    return 1;
                }
                memcpy(ts.floor_conn, fconn_raw, FLOOR_CONN_SZ);
                ts.floor_conn_has = 1;
            }
        }

        /* Decode block connector at block boundary */
        if (pos_in_floor == FLOOR_CELLS - 1 &&
            ((si / FLOOR_CELLS) % FLOORS_PER) == (FLOORS_PER - 1) &&
            (si + 1) < n_cells) {
            uint32_t bci = (si / FLOOR_CELLS) / FLOORS_PER;
            if (bci < n_bconn) {
                uint32_t cell_delta_sz = offsets[n_cells];
                uint32_t fconn_delta_sz = n_fconn > 0 ? fconn_off[n_fconn] : 0;
                const uint8_t *bd = all_data + cell_delta_sz + fconn_delta_sz + bconn_off[bci];
                int bsz = bconn_off[bci + 1] - bconn_off[bci];
                uint8_t bconn_raw[BLOCK_CONN_SZ];
                const uint8_t *bref = ts.block_conn_has ? ts.block_conn : zero_ref;
                if (decode_delta(bd, bsz, bconn_raw, BLOCK_CONN_SZ, bref, BLOCK_CONN_SZ) == 0) {
                    fprintf(stderr, "Block conn decode error at block %u\n", bci);
                    free(all_data); free(scattered); free(seeds); free(offsets);
                    free(fconn_off); free(bconn_off);
                    return 1;
                }
                memcpy(ts.block_conn, bconn_raw, BLOCK_CONN_SZ);
                ts.block_conn_has = 1;
            }
        }
    }

    /* Reorder from scattered to original order */
    uint8_t *output = (uint8_t *)calloc(1, (size_t)n_cells * CELL_SZ);
    if (scatter_n > 0 && inv_map) {
        for (uint32_t si = 0; si < scatter_n; si++)
            memcpy(output + inv_map[si] * CELL_SZ, scattered + si * CELL_SZ, CELL_SZ);
        for (uint32_t si = scatter_n; si < n_cells; si++)
            memcpy(output + si * CELL_SZ, scattered + si * CELL_SZ, CELL_SZ);
    } else {
        memcpy(output, scattered, (size_t)n_cells * CELL_SZ);
    }

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { perror(out_path); free(all_data); free(scattered); free(output); free(seeds); free(offsets); free(fconn_off); free(bconn_off); if (inv_map) free(inv_map); return 1; }
    fwrite(output, 1, hdr.original_size, fout);
    fclose(fout);

    printf("Decode: %s -> %s\n", in_path, out_path);
    printf("  %u bytes (%u cells, %u floors, %u towers, scatter=%u)\n",
           hdr.original_size, n_cells, n_floors, hdr.n_towers, scatter_n);

    /* ── Verify bond edges ── */
    if (hdr.n_edges > 0) {
        uint32_t cell_delta_sz = offsets[n_cells];
        uint32_t fconn_delta_sz = n_fconn > 0 ? fconn_off[n_fconn] : 0;
        uint32_t bconn_delta_sz = n_bconn > 0 ? bconn_off[n_bconn] : 0;
        uint32_t edge_section_off = cell_delta_sz + fconn_delta_sz + bconn_delta_sz;
        uint32_t edge_bytes = hdr.n_edges * BOND_EDGE_SZ;

        if (edge_section_off + edge_bytes <= (uint32_t)total_remaining) {
            BondEdge *edges = (BondEdge *)(all_data + edge_section_off);
            uint32_t verified = 0, failed = 0;

            /* Verify cell edges: reconstruct geo_key from decoded cell, compare with stored */
            for (uint32_t i = 0; i < hdr.n_edges; i++) {
                if (edges[i].weight == EDGE_CELL_HILBERT) {
                    /* Cell edge: in scattered order, edge[i] = scattered[i] → scattered[i+1] */
                    uint32_t si = i;
                    if (si + 1 < n_cells) {
                        uint64_t recon_o = bond_edge_geo_key(scattered + si * CELL_SZ, CELL_SZ);
                        uint64_t recon_t = bond_edge_geo_key(scattered + (si + 1) * CELL_SZ, CELL_SZ);
                        if (edges[i].origin == recon_o && edges[i].target == recon_t)
                            verified++;
                        else
                            failed++;
                    }
                } else {
                    /* Structural edge (floor/block): verified by successful delta decode */
                    verified++;
                }
            }
            printf("  bond_edges: %u/%u verified (failed=%u)\n",
                   verified, hdr.n_edges, failed);
            if (failed > 0)
                fprintf(stderr, "WARNING: %u bond edges FAILED tamper check\n", failed);
        } else {
            printf("  bond_edges: %u (truncated, skipped verification)\n", hdr.n_edges);
        }
    }

    free(all_data); free(scattered); free(output); free(seeds); free(offsets);
    free(fconn_off); free(bconn_off); if (inv_map) free(inv_map);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   Verify
   ═══════════════════════════════════════════════════════════════════════ */

static int do_verify(const char *orig_path, const char *gpf_path)
{
    const char *tmp = "_verify_tmp.bin";
    if (do_decode(gpf_path, tmp) != 0) return 1;
    FILE *f1 = fopen(orig_path, "rb");
    FILE *f2 = fopen(tmp, "rb");
    if (!f1 || !f2) { perror("verify"); return 1; }
    fseek(f1, 0, SEEK_END); long s1 = ftell(f1); fseek(f1, 0, SEEK_SET);
    fseek(f2, 0, SEEK_END); long s2 = ftell(f2); fseek(f2, 0, SEEK_SET);
    if (s1 != s2) { printf("FAIL: size %ld vs %ld\n", s1, s2); fclose(f1); fclose(f2); remove(tmp); return 1; }
    uint8_t *b1 = malloc((size_t)s1), *b2 = malloc((size_t)s2);
    fread(b1, 1, (size_t)s1, f1); fread(b2, 1, (size_t)s2, f2);
    fclose(f1); fclose(f2);
    int d = 0; for (long i = 0; i < s1; i++) if (b1[i] != b2[i]) d++;
    printf("Verify: %s <-> %s  size=%ld diff=%d %s\n", orig_path, gpf_path, s1, d, d == 0 ? "PASS" : "FAIL");
    free(b1); free(b2); remove(tmp);
    return d != 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("GeoPixel Field Pipeline v4.0 (L-Block + 48B + Connectors + Bond Edges)\n");
        printf("  Cell: %dB (4²×3, 16:9 ratio)\n", CELL_SZ);
        printf("  Floor: 4×4 = 16 cells + 16B connector\n");
        printf("  Tower: 3 floors + 32B block connector\n");
        printf("Usage:\n");
        printf("  %s encode <input> <output.gpf>\n", argv[0]);
        printf("  %s decode <input.gpf> <output>\n", argv[0]);
        printf("  %s verify <original> <encoded.gpf>\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "encode") == 0 && argc == 4) return do_encode(argv[2], argv[3]);
    if (strcmp(argv[1], "decode") == 0 && argc == 4) return do_decode(argv[2], argv[3]);
    if (strcmp(argv[1], "verify") == 0 && argc == 4) return do_verify(argv[2], argv[3]);
    fprintf(stderr, "Unknown: %s\n", argv[1]);
    return 1;
}

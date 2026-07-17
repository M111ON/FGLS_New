/*
 * pogls_hc_geojump.h — Hilbert L-Block Container × GeoJump Integration
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Combines the Hilbert L-block container with geo_jump's 4×4×3 Metatron
 * address space. Each Metatron block (48 cells = 3 floors × 16 cells)
 * becomes the container unit, with Hilbert-curve ordering within each
 * floor for spatial locality.
 *
 * Metatron structure:
 *   1 floor  = 4×4  = 16 cells (Hilbert-ordered)
 *   1 block  = 3 floors × 16 = 48 cells
 *   1 tower  = 3 blocks × 48 = 144 cells
 *   GEO_FULL = 144² = 20736 cells
 *
 * Container layout per block:
 *   [floor0 192B][fconn0 32B][floor1 192B][fconn1 32B][floor2 192B][bconn 32B]
 *
 *   floor data  = 16 cells × 64B = 1024B
 *   floor conn  = 16B (half-floor bridge between floors)
 *   block conn  = 32B (bridge to next Metatron block)
 *
 * Dependencies: geo_jump.h (with GEO_JUMP_INLINE), stdint.h
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_HC_GEOJUMP_H
#define POGLS_HC_GEOJUMP_H

/* Define GEO_JUMP_INLINE before including geo_jump.h for header-only */
#ifndef GEO_JUMP_INLINE
#define GEO_JUMP_INLINE
#endif

#include "../../geopixel/include/geo_jump.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
   CONSTANTS — derived from geo_jump Metatron structure
   ═══════════════════════════════════════════════════════════════════════ */

#define HC_GJ_MAGIC         0x474A4C42u  /* 'GJLB' — GeoJump L-Block */
#define HC_GJ_VERSION       1u
#define HC_GJ_CELL_SZ       64u          /* bytes per cell */
#define HC_GJ_FLOOR_CELLS   GEO_METATRON_CELLS  /* 16 cells per floor */
#define HC_GJ_FLOOR_CONN    16u          /* half-floor connector (16B) */
#define HC_GJ_BLOCK_CONN    32u          /* block-to-block connector (32B) */

/* Per-floor on-disk: 16 cells × 64B = 1024B data + 16B floor connector */
#define HC_GJ_FLOOR_DATA_SZ (HC_GJ_FLOOR_CELLS * HC_GJ_CELL_SZ)  /* 1024B */
#define HC_GJ_FLOOR_UNIT_SZ (HC_GJ_FLOOR_DATA_SZ + HC_GJ_FLOOR_CONN)  /* 1040B */

/* Per-block on-disk: 3 floors × 1040B + 32B block connector */
#define HC_GJ_FLOORS_PER    GEO_METATRON_FLOORS  /* 3 */
#define HC_GJ_BLOCK_DATA_SZ (HC_GJ_FLOORS_PER * HC_GJ_FLOOR_DATA_SZ) /* 3072B */
#define HC_GJ_BLOCK_UNIT_SZ (HC_GJ_FLOORS_PER * HC_GJ_FLOOR_UNIT_SZ + HC_GJ_BLOCK_CONN) /* 3152B */

#define HC_GJ_HEADER_SZ     20u

/* L-block within floor: 4 cells in Hilbert L-shape */
#define HC_GJ_LBLOCK_CELLS  4u           /* 3 data + 1 in-floor connector */
#define HC_GJ_LBLOCKS_PER   (HC_GJ_FLOOR_CELLS / HC_GJ_LBLOCK_CELLS) /* 4 */

/* Flags */
#define HC_GJ_FLAG_COMPRESSED  (1u << 0)
#define HC_GJ_FLAG_HAS_CONN    (1u << 1)

/* ═══════════════════════════════════════════════════════════════════════
   TYPES
   ═══════════════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* HC_GJ_MAGIC */
    uint16_t version;       /* HC_GJ_VERSION */
    uint16_t block_count;   /* number of Metatron blocks stored */
    uint32_t tower_id;      /* starting tower index (0..143) */
    uint32_t total_cells;   /* block_count × 48 */
    uint32_t flags;         /* HC_GJ_FLAG_* bitmask */
} HC_GeoJumpHeader;         /* sizeof = 20 */
#pragma pack(pop)

/* ═══════════════════════════════════════════════════════════════════════
   HILBERT WITHIN FLOOR — wraps geo_jump's _hilbert_idx
   ═══════════════════════════════════════════════════════════════════════ */

/* Map flat cell index (0..15) within a floor to Hilbert order */
static inline uint32_t hc_gj_cell_to_hilbert(uint32_t flat_idx)
{
    uint32_t x = flat_idx / GEO_METATRON_COLS;
    uint32_t y = flat_idx % GEO_METATRON_COLS;
    return _hilbert_idx(x, y, GEO_METATRON_COLS);
}

/* Map Hilbert order within floor to flat cell index */
static inline uint32_t hc_gj_hilbert_to_cell(uint32_t hilbert_d)
{
    uint32_t x, y;
    /* Inverse Hilbert for 4×4 (order 2) */
    uint32_t d = hilbert_d;
    x = 0; y = 0;
    for (uint32_t s = 1; s < GEO_METATRON_COLS; s <<= 1) {
        uint32_t rx = (d >> 1) & 1;
        uint32_t ry = (d ^ rx) & 1;
        if (ry == 0) {
            if (rx == 1) { x = s - 1 - x; y = s - 1 - y; }
            uint32_t t = x; x = y; y = t;
        }
        x += rx * s;
        y += ry * s;
        d >>= 2;
    }
    return x * GEO_METATRON_COLS + y;
}

/* ═══════════════════════════════════════════════════════════════════════
   ADDRESS MAPPING — geo_jump node ↔ container position
   ═══════════════════════════════════════════════════════════════════════ */

/* Given a geo_jump node_id, extract (tower, block, floor, cell) */
static inline void hc_gj_node_decompose(uint32_t node_id,
                                         uint32_t *tower,
                                         uint32_t *block,
                                         uint32_t *floor,
                                         uint32_t *cell)
{
    if (tower) *tower = node_id / GEO_TOWER;
    uint32_t within_tower = node_id % GEO_TOWER;
    if (block)  *block  = within_tower / GEO_BLOCK;
    if (floor)  *floor  = (within_tower % GEO_BLOCK) / GEO_METATRON_CELLS;
    if (cell)   *cell   = within_tower % GEO_METATRON_CELLS;
}

/* Rebuild node_id from (tower, block, floor, hilbert_cell) */
static inline uint32_t hc_gj_node_compose(uint32_t tower,
                                           uint32_t block,
                                           uint32_t floor,
                                           uint32_t hilbert_cell)
{
    return GEO_WRAP(tower * GEO_TOWER
                    + block * GEO_BLOCK
                    + floor * GEO_METATRON_CELLS
                    + hilbert_cell);
}

/* Get the 3 data node_ids for an L-block within a floor.
 * L-block 0: cells 0,1,2 (Hilbert d)  + connector at d=3
 * L-block 1: cells 4,5,6 (Hilbert d)  + connector at d=7
 * L-block 2: cells 8,9,10 (Hilbert d) + connector at d=11
 * L-block 3: cells 12,13,14 (Hilbert d) + connector at d=15 */
static inline void hc_gj_lblock_nodes(uint32_t tower,
                                       uint32_t block,
                                       uint32_t floor,
                                       uint32_t lblock_idx,
                                       uint32_t out_nodes[3],
                                       uint32_t *conn_node)
{
    /* geo_jump uses Hilbert distance d directly as the cell index
     * within a floor. So node = tower*TOWER + block*BLOCK + floor*16 + d */
    uint32_t base_d = lblock_idx * HC_GJ_LBLOCK_CELLS;
    uint32_t floor_base = tower * GEO_TOWER + block * GEO_BLOCK + floor * GEO_METATRON_CELLS;
    for (int i = 0; i < 3; i++)
        out_nodes[i] = floor_base + base_d + (uint32_t)i;
    if (conn_node) *conn_node = floor_base + base_d + 3;
}

/* ═══════════════════════════════════════════════════════════════════════
   WRITER
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    HC_GeoJumpHeader hdr;
    /* Block data: block_count × (3 floors × 1024B data + 3×16B floor_conn + 32B block_conn) */
    uint8_t *data;
    uint32_t cur_block;     /* current block being written */
    uint32_t cur_floor;     /* current floor within block (0..2) */
    uint32_t cur_cell;      /* cell within floor (0..15) */
} HC_GeoJumpWriter;

static inline int hc_gj_writer_init(HC_GeoJumpWriter *w, uint32_t block_count,
                                     uint32_t tower_id)
{
    if (!w || block_count == 0 || block_count > 144) return -1;
    w->hdr.magic       = HC_GJ_MAGIC;
    w->hdr.version     = HC_GJ_VERSION;
    w->hdr.block_count = (uint16_t)block_count;
    w->hdr.tower_id    = tower_id;
    w->hdr.total_cells = block_count * GEO_BLOCK;
    w->hdr.flags       = HC_GJ_FLAG_HAS_CONN;
    w->data = (uint8_t *)calloc((size_t)block_count, HC_GJ_BLOCK_UNIT_SZ);
    if (!w->data) return -2;
    w->cur_block = 0;
    w->cur_floor = 0;
    w->cur_cell  = 0;
    return 0;
}

static inline void hc_gj_writer_destroy(HC_GeoJumpWriter *w)
{
    if (w && w->data) { free(w->data); w->data = NULL; }
}

/* Offset to block[b] data section in buffer */
static inline size_t _hc_gj_block_offset(uint32_t block_idx)
{
    return (size_t)block_idx * HC_GJ_BLOCK_UNIT_SZ;
}

/* Offset to floor[f] data within a block */
static inline size_t _hc_gj_floor_offset(uint32_t floor_idx)
{
    return (size_t)floor_idx * HC_GJ_FLOOR_UNIT_SZ;
}

/* Write a cell (64B) into the current position. Auto-advances.
 * Returns block_index on success, -1 when full. */
static inline int hc_gj_write_cell(HC_GeoJumpWriter *w, const uint8_t *cell_data)
{
    if (!w || !w->data) return -1;
    if (w->cur_block >= w->hdr.block_count) return -1;

    size_t off = _hc_gj_block_offset(w->cur_block)
               + _hc_gj_floor_offset(w->cur_floor)
               + (size_t)w->cur_cell * HC_GJ_CELL_SZ;
    memcpy(w->data + off, cell_data, HC_GJ_CELL_SZ);

    uint32_t result = (uint32_t)w->cur_block;
    w->cur_cell++;
    if (w->cur_cell >= HC_GJ_FLOOR_CELLS) {
        w->cur_cell = 0;
        w->cur_floor++;
        if (w->cur_floor >= HC_GJ_FLOORS_PER) {
            w->cur_floor = 0;
            w->cur_block++;
        }
    }
    return (int)result;
}

/* Write floor connector (16B) for a specific block+floor */
static inline void hc_gj_write_floor_conn(HC_GeoJumpWriter *w,
                                            uint32_t block_idx,
                                            uint32_t floor_idx,
                                            const uint8_t *conn_data)
{
    if (!w || !w->data) return;
    if (block_idx >= w->hdr.block_count || floor_idx >= HC_GJ_FLOORS_PER) return;
    size_t off = _hc_gj_block_offset(block_idx)
               + _hc_gj_floor_offset(floor_idx)
               + HC_GJ_FLOOR_DATA_SZ;
    memcpy(w->data + off, conn_data, HC_GJ_FLOOR_CONN);
}

/* Write block connector (32B) for a specific block */
static inline void hc_gj_write_block_conn(HC_GeoJumpWriter *w,
                                            uint32_t block_idx,
                                            const uint8_t *conn_data)
{
    if (!w || !w->data) return;
    if (block_idx >= w->hdr.block_count) return;
    size_t off = _hc_gj_block_offset(block_idx)
               + (size_t)HC_GJ_FLOORS_PER * HC_GJ_FLOOR_UNIT_SZ;
    memcpy(w->data + off, conn_data, HC_GJ_BLOCK_CONN);
}

/* Get a geo_jump node_id for the current write position */
static inline uint32_t hc_gj_writer_node(const HC_GeoJumpWriter *w)
{
    if (!w) return 0;
    return hc_gj_node_compose(w->hdr.tower_id, w->cur_block,
                               w->cur_floor, w->cur_cell);
}

/* ═══════════════════════════════════════════════════════════════════════
   SERIALIZATION
   ═══════════════════════════════════════════════════════════════════════ */

static inline size_t hc_gj_serialized_size(const HC_GeoJumpWriter *w)
{
    if (!w) return 0;
    return HC_GJ_HEADER_SZ + (size_t)w->hdr.block_count * HC_GJ_BLOCK_UNIT_SZ;
}

static inline size_t hc_gj_serialize(const HC_GeoJumpWriter *w,
                                      uint8_t *dst, size_t dst_cap)
{
    if (!w || !dst) return 0;
    size_t need = hc_gj_serialized_size(w);
    if (dst_cap < need) return 0;

    size_t off = 0;
    memcpy(dst + off, &w->hdr, HC_GJ_HEADER_SZ);
    off += HC_GJ_HEADER_SZ;
    memcpy(dst + off, w->data, (size_t)w->hdr.block_count * HC_GJ_BLOCK_UNIT_SZ);
    off += (size_t)w->hdr.block_count * HC_GJ_BLOCK_UNIT_SZ;
    return off;
}

/* ═══════════════════════════════════════════════════════════════════════
   READER
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    HC_GeoJumpHeader hdr;
    const uint8_t   *base;
    size_t           file_sz;
    const uint8_t   *blk_data;  /* base + HC_GJ_HEADER_SZ */
} HC_GeoJumpReader;

static inline int hc_gj_reader_init(HC_GeoJumpReader *r,
                                     const void *data, size_t data_sz)
{
    if (!r || !data || data_sz < HC_GJ_HEADER_SZ) return -1;
    r->base    = (const uint8_t *)data;
    r->file_sz = data_sz;
    memcpy(&r->hdr, r->base, HC_GJ_HEADER_SZ);
    if (r->hdr.magic != HC_GJ_MAGIC) return -2;
    if (r->hdr.version != HC_GJ_VERSION) return -3;
    size_t expected = HC_GJ_HEADER_SZ +
                      (size_t)r->hdr.block_count * HC_GJ_BLOCK_UNIT_SZ;
    if (data_sz < expected) return -4;
    r->blk_data = r->base + HC_GJ_HEADER_SZ;
    return 0;
}

static inline int hc_gj_verify(const HC_GeoJumpReader *r)
{
    if (!r) return -1;
    if (r->hdr.magic != HC_GJ_MAGIC) return -2;
    if (r->hdr.version != HC_GJ_VERSION) return -3;
    if (r->hdr.block_count == 0) return -4;
    return 0;
}

/* Get cell data for block[b], floor[f], cell[c] */
static inline const uint8_t *hc_gj_get_cell(const HC_GeoJumpReader *r,
                                             uint32_t block_idx,
                                             uint32_t floor_idx,
                                             uint32_t cell_idx)
{
    if (!r || !r->blk_data) return NULL;
    if (block_idx >= r->hdr.block_count) return NULL;
    if (floor_idx >= HC_GJ_FLOORS_PER) return NULL;
    if (cell_idx >= HC_GJ_FLOOR_CELLS) return NULL;
    size_t off = (size_t)block_idx * HC_GJ_BLOCK_UNIT_SZ
               + (size_t)floor_idx * HC_GJ_FLOOR_UNIT_SZ
               + (size_t)cell_idx * HC_GJ_CELL_SZ;
    return r->blk_data + off;
}

/* Get floor connector for block[b], floor[f] */
static inline const uint8_t *hc_gj_get_floor_conn(const HC_GeoJumpReader *r,
                                                    uint32_t block_idx,
                                                    uint32_t floor_idx)
{
    if (!r || !r->blk_data) return NULL;
    if (block_idx >= r->hdr.block_count) return NULL;
    if (floor_idx >= HC_GJ_FLOORS_PER) return NULL;
    size_t off = (size_t)block_idx * HC_GJ_BLOCK_UNIT_SZ
               + (size_t)floor_idx * HC_GJ_FLOOR_UNIT_SZ
               + HC_GJ_FLOOR_DATA_SZ;
    return r->blk_data + off;
}

/* Get block connector for block[b] */
static inline const uint8_t *hc_gj_get_block_conn(const HC_GeoJumpReader *r,
                                                    uint32_t block_idx)
{
    if (!r || !r->blk_data) return NULL;
    if (block_idx >= r->hdr.block_count) return NULL;
    size_t off = (size_t)block_idx * HC_GJ_BLOCK_UNIT_SZ
               + (size_t)HC_GJ_FLOORS_PER * HC_GJ_FLOOR_UNIT_SZ;
    return r->blk_data + off;
}

/* Get cell by geo_jump node_id (resolves tower→block→floor→cell) */
static inline const uint8_t *hc_gj_get_node(const HC_GeoJumpReader *r,
                                             uint32_t node_id)
{
    uint32_t tower, block, floor, cell;
    hc_gj_node_decompose(node_id, &tower, &block, &floor, &cell);
    (void)tower;
    return hc_gj_get_cell(r, block, floor, cell);
}

/* ═══════════════════════════════════════════════════════════════════════
   FILE I/O
   ═══════════════════════════════════════════════════════════════════════ */

static inline int hc_gj_write_file(const char *path, const HC_GeoJumpWriter *w)
{
    if (!path || !w) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    size_t sz = hc_gj_serialized_size(w);
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { fclose(f); return -3; }
    size_t written = hc_gj_serialize(w, buf, sz);
    if (written != sz) { free(buf); fclose(f); return -4; }
    size_t ok = fwrite(buf, 1, sz, f);
    free(buf); fclose(f);
    return (ok == sz) ? 0 : -5;
}

static inline int hc_gj_read_file(const char *path, HC_GeoJumpReader *r,
                                   void **out_data, size_t *out_sz)
{
    if (!path || !r) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -2;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -3; }
    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -4; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return -5; }
    int err = hc_gj_reader_init(r, buf, (size_t)sz);
    if (err) { free(buf); return err; }
    if (out_data) *out_data = buf; else free(buf);
    if (out_sz) *out_sz = (size_t)sz;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* POGLS_HC_GEOJUMP_H */

/*
 * pogls_hilbert_container.h — Hilbert L-Block Container Format
 * ═══════════════════════════════════════════════════════════════════════
 *
 * A Hilbert-curve-based tiling container where:
 *   - Each L-block = 3 cells in L-shape (3 × 64B = 192B data)
 *   - Each connector = half-block bridge (32B) between L-blocks
 *   - Hilbert curve ordering preserves spatial locality
 *
 * Container layout:
 *   [Header 20B] [L0 192B][C0 32B] [L1 192B][C1 224B] ...
 *
 * For Hilbert order N:
 *   - Total cells = 4^N
 *   - L-block count = 4^(N-1)
 *   - Total size = 20 + block_count × 224 bytes
 *
 * Dependencies: <stdint.h> <stddef.h> <string.h> <stdio.h>
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_HILBERT_CONTAINER_H
#define POGLS_HILBERT_CONTAINER_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════ */

#define HC_MAGIC          0x48424C4Bu  /* 'HBLK' */
#define HC_VERSION        1u
#define HC_CELL_SZ        64u
#define HC_CONN_SZ        32u         /* half-block connector */
#define HC_LBLOCK_CELLS   3u          /* cells per L-block */
#define HC_LBLOCK_DATA_SZ (HC_LBLOCK_CELLS * HC_CELL_SZ)  /* 192B */
#define HC_LBLOCK_UNIT_SZ (HC_LBLOCK_DATA_SZ + HC_CONN_SZ) /* 224B */
#define HC_HEADER_SZ      20u

/* Flags */
#define HC_FLAG_COMPRESSED  (1u << 0)  /* cells use geopixel compression */
#define HC_FLAG_HAS_CONN    (1u << 1)  /* connector data is present */
#define HC_FLAG_HAS_META    (1u << 2)  /* extended metadata section */

/* ═══════════════════════════════════════════════════════════════════════
   TYPES
   ═══════════════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* HC_MAGIC */
    uint16_t version;       /* HC_VERSION */
    uint16_t order;         /* Hilbert curve order (N) */
    uint32_t block_count;   /* number of L-blocks = 4^(N-1) */
    uint32_t total_cells;   /* 4^N */
    uint32_t flags;         /* HC_FLAG_* bitmask */
} HilbertContainerHeader;   /* sizeof = 20 */
#pragma pack(pop)

/* L-block: 3 cells + 1 connector */
typedef struct {
    uint32_t addr[HC_LBLOCK_CELLS];                    /* Hilbert d-indices */
    uint8_t  data[HC_LBLOCK_CELLS][HC_CELL_SZ];        /* cell data (192B) */
    uint8_t  conn[HC_CONN_SZ];                         /* connector (32B) */
} HilbertLBlock;   /* sizeof = 236 */

/* Write context */
typedef struct {
    HilbertContainerHeader hdr;
    HilbertLBlock          *blocks;    /* array[block_count] */
    uint32_t                cur_block; /* current block index */
    uint32_t                cur_cell;  /* cell within current block (0..2) */
} HilbertContainerWriter;

/* Read context — on-disk format uses raw data+conn (no addr[]).
 * Reader uses pointer arithmetic on base, NOT struct cast. */
typedef struct {
    HilbertContainerHeader hdr;
    const uint8_t         *base;       /* mmap or file base */
    size_t                 file_sz;    /* total file size */
    /* Pointer to first block's data section in file */
    const uint8_t         *blk_data;   /* base + HC_HEADER_SZ */
} HilbertContainerReader;

/* ═══════════════════════════════════════════════════════════════════════
   HILBERT CURVE (standard iterative, order N)
   ═══════════════════════════════════════════════════════════════════════ */

static inline uint32_t hc_hilbert_xy_to_d(uint32_t x, uint32_t y,
                                           uint32_t order)
{
    uint32_t d = 0;
    for (uint32_t s = 1u << (order - 1); s; s >>= 1) {
        uint32_t rx = (x & s) ? 1 : 0;
        uint32_t ry = (y & s) ? 1 : 0;
        d = (d << 2) | ((3 * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) {
                x = (s - 1) - x;
                y = (s - 1) - y;
            }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

static inline void hc_hilbert_d_to_xy(uint32_t d, uint32_t order,
                                       uint32_t *x, uint32_t *y)
{
    uint32_t hx = 0, hy = 0;
    uint32_t s = 1;
    for (uint32_t i = 0; i < order; i++) {
        uint32_t rx, ry;
        uint32_t rot = d & 3;
        rx = (rot >> 1) & 1;
        ry = (rot & 1) ^ rx;
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

/* ═══════════════════════════════════════════════════════════════════════
   L-BLOCK MAPPING
   ═══════════════════════════════════════════════════════════════════════
 *
 * Hilbert order N → 4^N cells → 4^(N-1) L-blocks.
 * Each L-block spans 4 consecutive Hilbert indices:
 *   block[i] → cells [4i, 4i+1, 4i+2] (data) + [4i+3] (connector)
 *
 * ═══════════════════════════════════════════════════════════════════════ */

/* Compute total cells and block count for given order */
static inline uint32_t hc_total_cells(uint32_t order)
{
    return 1u << (2 * order);  /* 4^order */
}

static inline uint32_t hc_block_count(uint32_t order)
{
    if (order == 0) return 0;
    return 1u << (2 * (order - 1));  /* 4^(order-1) */
}

/* Map Hilbert index d → block index and cell position within block */
static inline void hc_d_to_block_cell(uint32_t d, uint32_t order,
                                       uint32_t *block_idx,
                                       uint32_t *cell_pos)
{
    (void)order;
    *block_idx = d / 4;
    *cell_pos  = d % 4;
}

/* Get the 3 Hilbert addresses for block[i] */
static inline void hc_block_addrs(uint32_t block_idx, uint32_t order,
                                   uint32_t addrs[3])
{
    uint32_t base = block_idx * 4;
    addrs[0] = base;
    addrs[1] = base + 1;
    addrs[2] = base + 2;
    /* addrs[3] = base + 3 is the connector address (not stored in addr[]) */
}

/* Get connector address for block[i] */
static inline uint32_t hc_conn_addr(uint32_t block_idx)
{
    return block_idx * 4 + 3;
}

/* ═══════════════════════════════════════════════════════════════════════
   WRITER
   ═══════════════════════════════════════════════════════════════════════ */

/* Initialize writer for given Hilbert order */
static inline int hc_writer_init(HilbertContainerWriter *w, uint32_t order)
{
    if (!w || order == 0 || order > 8) return -1;

    w->hdr.magic       = HC_MAGIC;
    w->hdr.version     = HC_VERSION;
    w->hdr.order       = (uint16_t)order;
    w->hdr.block_count = hc_block_count(order);
    w->hdr.total_cells = hc_total_cells(order);
    w->hdr.flags       = 0;

    w->blocks = (HilbertLBlock *)calloc(w->hdr.block_count,
                                         sizeof(HilbertLBlock));
    if (!w->blocks) return -1;

    /* Initialize block addresses */
    for (uint32_t i = 0; i < w->hdr.block_count; i++) {
        hc_block_addrs(i, order, w->blocks[i].addr);
    }

    w->cur_block = 0;
    w->cur_cell  = 0;
    return 0;
}

/* Free writer resources */
static inline void hc_writer_destroy(HilbertContainerWriter *w)
{
    if (w && w->blocks) {
        free(w->blocks);
        w->blocks = NULL;
    }
}

/* Write one cell into current position.
 * Automatically advances: cell 0→1→2→next block.
 * Returns block index where data was written, or -1 on error. */
static inline int hc_write_cell(HilbertContainerWriter *w,
                                 const uint8_t *cell_data)
{
    if (!w || !cell_data || !w->blocks) return -1;
    if (w->cur_block >= w->hdr.block_count) return -1;
    if (w->cur_cell >= HC_LBLOCK_CELLS) return -1;

    HilbertLBlock *blk = &w->blocks[w->cur_block];
    memcpy(blk->data[w->cur_cell], cell_data, HC_CELL_SZ);

    int result = (int)w->cur_block;
    w->cur_cell++;
    if (w->cur_cell >= HC_LBLOCK_CELLS) {
        w->cur_cell = 0;
        w->cur_block++;
    }
    return result;
}

/* Write connector data for a specific block */
static inline int hc_write_connector(HilbertContainerWriter *w,
                                      uint32_t block_idx,
                                      const uint8_t *conn_data)
{
    if (!w || !conn_data || !w->blocks) return -1;
    if (block_idx >= w->hdr.block_count) return -1;

    memcpy(w->blocks[block_idx].conn, conn_data, HC_CONN_SZ);
    w->hdr.flags |= HC_FLAG_HAS_CONN;
    return 0;
}

/* Write connector for current block (auto-advances to next block) */
static inline int hc_write_current_connector(HilbertContainerWriter *w,
                                              const uint8_t *conn_data)
{
    if (!w || !conn_data) return -1;
    uint32_t blk = (w->cur_cell == 0 && w->cur_block > 0)
                   ? w->cur_block - 1
                   : w->cur_block;
    return hc_write_connector(w, blk, conn_data);
}

/* Finalize: compute final block count (may be less than allocated
 * if not all blocks were filled). Returns final block count. */
static inline uint32_t hc_writer_finalize(HilbertContainerWriter *w)
{
    if (!w) return 0;
    /* Adjust block count to actual filled blocks */
    if (w->cur_block < w->hdr.block_count) {
        w->hdr.block_count = w->cur_block;
        w->hdr.total_cells = w->cur_block * 4;
    }
    return w->hdr.block_count;
}

/* Compute serialized size */
static inline size_t hc_serialized_size(const HilbertContainerWriter *w)
{
    if (!w) return 0;
    return HC_HEADER_SZ + (size_t)w->hdr.block_count * HC_LBLOCK_UNIT_SZ;
}

/* Serialize to buffer. dst must be >= hc_serialized_size().
 * On-disk format: [Header 20B] [data0 192B][conn0 32B] [data1 192B]...
 * Returns bytes written, or 0 on error. */
static inline size_t hc_serialize(const HilbertContainerWriter *w,
                                   uint8_t *dst, size_t dst_cap)
{
    if (!w || !dst) return 0;
    size_t need = hc_serialized_size(w);
    if (dst_cap < need) return 0;

    size_t off = 0;
    memcpy(dst + off, &w->hdr, HC_HEADER_SZ);
    off += HC_HEADER_SZ;

    for (uint32_t i = 0; i < w->hdr.block_count; i++) {
        memcpy(dst + off, w->blocks[i].data, HC_LBLOCK_DATA_SZ);
        off += HC_LBLOCK_DATA_SZ;
        memcpy(dst + off, w->blocks[i].conn, HC_CONN_SZ);
        off += HC_CONN_SZ;
    }

    return off;
}

/* ═══════════════════════════════════════════════════════════════════════
   READER
   ═══════════════════════════════════════════════════════════════════════ */

/* Initialize reader from buffer */
static inline int hc_reader_init(HilbertContainerReader *r,
                                  const void *data, size_t data_sz)
{
    if (!r || !data || data_sz < HC_HEADER_SZ) return -1;

    r->base    = (const uint8_t *)data;
    r->file_sz = data_sz;

    memcpy(&r->hdr, r->base, HC_HEADER_SZ);

    if (r->hdr.magic != HC_MAGIC) return -2;
    if (r->hdr.version != HC_VERSION) return -3;

    /* Validate size */
    size_t expected = HC_HEADER_SZ +
                      (size_t)r->hdr.block_count * HC_LBLOCK_UNIT_SZ;
    if (data_sz < expected) return -4;

    r->blk_data = r->base + HC_HEADER_SZ;
    return 0;
}

/* Get block count */
static inline uint32_t hc_reader_block_count(const HilbertContainerReader *r)
{
    return r ? r->hdr.block_count : 0;
}

/* Pointer to block[i] data section in file (192B of cell data) */
static inline const uint8_t *_hc_block_ptr(const HilbertContainerReader *r,
                                            uint32_t block_idx)
{
    return r->blk_data + (size_t)block_idx * HC_LBLOCK_UNIT_SZ;
}

/* Get cell data for block[i], cell[j] (j=0,1,2) */
static inline const uint8_t *hc_get_cell(const HilbertContainerReader *r,
                                          uint32_t block_idx,
                                          uint32_t cell_pos)
{
    if (!r || !r->blk_data) return NULL;
    if (block_idx >= r->hdr.block_count) return NULL;
    if (cell_pos >= HC_LBLOCK_CELLS) return NULL;
    const uint8_t *blk = _hc_block_ptr(r, block_idx);
    return blk + (size_t)cell_pos * HC_CELL_SZ;
}

/* Get connector data for block[i] */
static inline const uint8_t *hc_get_connector(const HilbertContainerReader *r,
                                               uint32_t block_idx)
{
    if (!r || !r->blk_data) return NULL;
    if (block_idx >= r->hdr.block_count) return NULL;
    const uint8_t *blk = _hc_block_ptr(r, block_idx);
    return blk + HC_LBLOCK_DATA_SZ;  /* conn is after 192B of cell data */
}

/* Get address for block[i], cell[j] — computed, not stored */
static inline uint32_t hc_get_addr(const HilbertContainerReader *r,
                                    uint32_t block_idx, uint32_t cell_pos)
{
    if (!r) return 0;
    if (block_idx >= r->hdr.block_count) return 0;
    if (cell_pos >= HC_LBLOCK_CELLS) return 0;
    return block_idx * 4 + cell_pos;
}

/* ═══════════════════════════════════════════════════════════════════════
   VERIFY
   ═══════════════════════════════════════════════════════════════════════ */

/* Verify container integrity. Returns 0 on success, <0 on error. */
static inline int hc_verify(const HilbertContainerReader *r)
{
    if (!r) return -1;
    if (r->hdr.magic != HC_MAGIC) return -2;
    if (r->hdr.version != HC_VERSION) return -3;
    if (r->hdr.order == 0 || r->hdr.order > 8) return -4;

    uint32_t exp_cells = hc_total_cells(r->hdr.order);
    uint32_t exp_blocks = hc_block_count(r->hdr.order);

    if (r->hdr.total_cells != exp_cells) return -5;
    if (r->hdr.block_count != exp_blocks) return -6;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   FILE I/O
   ═══════════════════════════════════════════════════════════════════════ */

/* Write container to file. Returns 0 on success. */
static inline int hc_write_file(const char *path,
                                 const HilbertContainerWriter *w)
{
    if (!path || !w) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;

    size_t sz = hc_serialized_size(w);
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { fclose(f); return -3; }

    size_t written = hc_serialize(w, buf, sz);
    if (written != sz) { free(buf); fclose(f); return -4; }

    size_t ok = fwrite(buf, 1, sz, f);
    free(buf);
    fclose(f);
    return (ok == sz) ? 0 : -5;
}

/* Read container from file. Caller must free(*data_out) when done.
 * Returns 0 on success. */
static inline int hc_read_file(const char *path,
                                HilbertContainerReader *r,
                                void **data_out, size_t *data_sz_out)
{
    if (!path || !r) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -2;

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -3; }

    void *data = malloc((size_t)sz);
    if (!data) { fclose(f); return -4; }

    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(data); return -5; }

    int err = hc_reader_init(r, data, (size_t)sz);
    if (err) { free(data); return err; }

    if (data_out)    *data_out    = data;
    if (data_sz_out) *data_sz_out = (size_t)sz;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   SPATIAL QUERY — find L-block containing (x,y)
   ═══════════════════════════════════════════════════════════════════════ */

/* Find which block and cell position contains Hilbert address d */
static inline int hc_find_block(const HilbertContainerReader *r,
                                 uint32_t d,
                                 uint32_t *block_idx,
                                 uint32_t *cell_pos)
{
    if (!r) return -1;
    if (d >= r->hdr.total_cells) return -2;

    *block_idx = d / 4;
    *cell_pos  = d % 4;

    if (*cell_pos == 3) {
        /* This is a connector address — connector data lives in
         * block[*block_idx].conn, not in data[][] */
        return 1;  /* special: connector */
    }
    return 0;  /* normal: cell */
}

/* ═══════════════════════════════════════════════════════════════════════
   DUMP — debug print
   ═══════════════════════════════════════════════════════════════════════ */

static inline void hc_dump_header(const HilbertContainerReader *r)
{
    if (!r) return;
    printf("HilbertContainer v%d  order=%u  blocks=%u  cells=%u  "
           "flags=0x%X\n",
           r->hdr.version, r->hdr.order, r->hdr.block_count,
           r->hdr.total_cells, r->hdr.flags);
    printf("  cell_sz=%u  conn_sz=%u  unit_sz=%u\n",
           HC_CELL_SZ, HC_CONN_SZ, HC_LBLOCK_UNIT_SZ);
    printf("  file_size=%zu  header=%u  data=%zu\n",
           r->file_sz, HC_HEADER_SZ,
           r->file_sz - HC_HEADER_SZ);
}

static inline void hc_dump_block(const HilbertContainerReader *r,
                                  uint32_t block_idx)
{
    if (!r || block_idx >= r->hdr.block_count) return;
    uint32_t a0 = block_idx * 4 + 0;
    uint32_t a1 = block_idx * 4 + 1;
    uint32_t a2 = block_idx * 4 + 2;

    printf("  block[%u]  addrs=[%u,%u,%u]  conn_addr=%u\n",
           block_idx, a0, a1, a2, hc_conn_addr(block_idx));

    /* Print first 8 bytes of each cell */
    for (int c = 0; c < 3; c++) {
        const uint8_t *cell = hc_get_cell(r, block_idx, (uint32_t)c);
        printf("    cell[%d] d=%u: ", c, block_idx * 4 + c);
        for (int b = 0; b < 8 && b < HC_CELL_SZ; b++)
            printf("%02x", cell[b]);
        printf("...\n");
    }

    /* Print connector first 8 bytes */
    const uint8_t *conn = hc_get_connector(r, block_idx);
    printf("    conn: ");
    for (int b = 0; b < 8 && b < HC_CONN_SZ; b++)
        printf("%02x", conn[b]);
    printf("...\n");
}

#ifdef __cplusplus
}
#endif

#endif /* POGLS_HILBERT_CONTAINER_H */

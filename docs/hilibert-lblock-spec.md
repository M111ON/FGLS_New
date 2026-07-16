# Hilbert L-Block Container — Technical Specification

## 1. Overview

The Hilbert L-Block Container is a topology-aware data container format that organizes 64-byte data cells into L-shaped blocks following a Hilbert curve. It exists in two variants:

| Variant | File | Address Space | Unit Size | Dependencies |
|---|---|---|---|---|
| Standalone | `pogls_hilbert_container.h` | 1D Hilbert (4^N) | 224B per L-block | `<stdint.h>` only |
| GeoJump | `pogls_hc_geojump.h` | 3D Metatron (20736 nodes) | 3152B per block | `geo_jump.h` |

### Core Principle

> L-block changes the primitive from "byte chunk" to "topology-aware container" that knows position, neighbors, and connection points.

### Value Proposition

| Value | Source |
|---|---|
| Locality / Cache-friendly | ✅ L-block — 3-cell units are co-accessed, enabling prefetch, cache hit, SIMD batching |
| Connector graph for prediction | ✅ L-block — floor/block connectors create A-B/C-D dependency structure |
| Shared node_id all stages | ✅ L-block — universal address across Bond/Shell/Pixel |
| O(1) access | ⚠️ Deterministic indexing (RDH/geo_jump bitmap popcount) |
| Compression | ❌ FLAT codec handles zeros. L-block organizes, not compresses. |

---

## 2. Standalone Variant (`pogls_hilbert_container.h`)

### 2.1 Constants

```c
#define HC_MAGIC          0x48424C4Bu  /* 'HBLK' */
#define HC_VERSION        1u
#define HC_CELL_SZ        64u          /* bytes per cell */
#define HC_CONN_SZ        32u          /* half-block connector */
#define HC_LBLOCK_CELLS   3u           /* cells per L-block */
#define HC_LBLOCK_DATA_SZ 192u         /* 3 × 64B */
#define HC_LBLOCK_UNIT_SZ 224u         /* 192B data + 32B connector */
#define HC_HEADER_SZ      20u
```

### 2.2 File Format

```
Offset  Size   Field
──────  ─────  ─────────────────────────
0       4      magic (0x48424C4B = 'HBLK')
4       2      version (1)
6       2      order (Hilbert curve order N, 1..8)
8       4      block_count = 4^(N-1)
12      4      total_cells = 4^N
16      4      flags
──────  ─────  ─────────────────────────
20      192    block[0].data[0..2]      (3 × 64B = 192B cell data)
212     32     block[0].conn            (connector)
244     192    block[1].data[0..2]
436     32     block[1].conn
...
```

**Total size:** `20 + block_count × 224 bytes`

### 2.3 Hilbert Curve Mapping

For Hilbert order N:
- Total cells = 4^N
- L-block count = 4^(N-1)
- Each L-block spans 4 consecutive Hilbert indices: `[4i, 4i+1, 4i+2]` (data) + `[4i+3]` (connector)

```
Hilbert order 2 (16 cells, 4 L-blocks):

  d=0,1,2 → L-block 0 data (3 cells)
  d=3     → L-block 0 connector
  d=4,5,6 → L-block 1 data
  d=7     → L-block 1 connector
  d=8,9,10→ L-block 2 data
  d=11    → L-block 2 connector
  d=12,13,14 → L-block 3 data
  d=15    → L-block 3 connector
```

### 2.4 API Reference

#### Types

```c
typedef struct {
    uint32_t magic;         /* HC_MAGIC */
    uint16_t version;       /* HC_VERSION */
    uint16_t order;         /* Hilbert curve order (1..8) */
    uint32_t block_count;   /* number of L-blocks */
    uint32_t total_cells;   /* 4^order */
    uint32_t flags;         /* HC_FLAG_* bitmask */
} HilbertContainerHeader;   /* 20 bytes */

typedef struct {
    uint32_t addr[3];                    /* Hilbert d-indices (not stored on disk) */
    uint8_t  data[3][64];               /* cell data (192B) */
    uint8_t  conn[32];                  /* connector (32B) */
} HilbertLBlock;   /* 236 bytes (in-memory only) */

typedef struct {
    HilbertContainerHeader hdr;
    HilbertLBlock          *blocks;
    uint32_t                cur_block;
    uint32_t                cur_cell;
} HilbertContainerWriter;

typedef struct {
    HilbertContainerHeader hdr;
    const uint8_t         *base;
    size_t                 file_sz;
    const uint8_t         *blk_data;
} HilbertContainerReader;
```

#### Hilbert Functions

```c
uint32_t hc_hilbert_xy_to_d(uint32_t x, uint32_t y, uint32_t order);
void     hc_hilbert_d_to_xy(uint32_t d, uint32_t order, uint32_t *x, uint32_t *y);
```

#### L-Block Mapping

```c
uint32_t hc_total_cells(uint32_t order);      /* 4^order */
uint32_t hc_block_count(uint32_t order);       /* 4^(order-1) */
void     hc_d_to_block_cell(uint32_t d, uint32_t order,
                            uint32_t *block_idx, uint32_t *cell_pos);
void     hc_block_addrs(uint32_t block_idx, uint32_t order,
                        uint32_t addrs[3]);
uint32_t hc_conn_addr(uint32_t block_idx);
```

#### Writer

```c
int      hc_writer_init(HilbertContainerWriter *w, uint32_t order);
void     hc_writer_destroy(HilbertContainerWriter *w);
int      hc_write_cell(HilbertContainerWriter *w, const uint8_t *cell_data);
int      hc_write_connector(HilbertContainerWriter *w, uint32_t block_idx,
                            const uint8_t *conn_data);
uint32_t hc_writer_finalize(HilbertContainerWriter *w);
size_t   hc_serialized_size(const HilbertContainerWriter *w);
size_t   hc_serialize(const HilbertContainerWriter *w,
                      uint8_t *dst, size_t dst_cap);
```

#### Reader

```c
int      hc_reader_init(HilbertContainerReader *r,
                        const void *data, size_t data_sz);
uint32_t hc_reader_block_count(const HilbertContainerReader *r);
const uint8_t *hc_get_cell(const HilbertContainerReader *r,
                           uint32_t block_idx, uint32_t cell_pos);
const uint8_t *hc_get_connector(const HilbertContainerReader *r,
                                uint32_t block_idx);
uint32_t hc_get_addr(const HilbertContainerReader *r,
                     uint32_t block_idx, uint32_t cell_pos);
int      hc_verify(const HilbertContainerReader *r);
int      hc_find_block(const HilbertContainerReader *r, uint32_t d,
                       uint32_t *block_idx, uint32_t *cell_pos);
```

#### File I/O

```c
int hc_write_file(const char *path, const HilbertContainerWriter *w);
int hc_read_file(const char *path, HilbertContainerReader *r,
                 void **data_out, size_t *data_sz_out);
```

#### Debug

```c
void hc_dump_header(const HilbertContainerReader *r);
void hc_dump_block(const HilbertContainerReader *r, uint32_t block_idx);
```

---

## 3. GeoJump Variant (`pogls_hc_geojump.h`)

### 3.1 Constants

```c
#define HC_GJ_MAGIC         0x474A4C42u  /* 'GJLB' */
#define HC_GJ_VERSION       1u
#define HC_GJ_CELL_SZ       64u          /* bytes per cell */
#define HC_GJ_FLOOR_CELLS   16u          /* 4×4 Hilbert per floor */
#define HC_GJ_FLOOR_CONN    16u          /* half-floor connector */
#define HC_GJ_BLOCK_CONN    32u          /* block-to-block connector */
#define HC_GJ_FLOORS_PER    3u           /* 3 floors per block */
#define HC_GJ_LBLOCK_CELLS  4u           /* 3 data + 1 in-floor connector */
#define HC_GJ_LBLOCKS_PER   4u           /* 4 L-blocks per floor */
```

### 3.2 Metatron Structure

```
1 floor  = 4×4  = 16 cells  (Hilbert-ordered)
1 block  = 3 floors × 16 = 48 cells
1 tower  = 3 blocks × 48 = 144 cells
GEO_FULL = 144² = 20736 cells
```

### 3.3 File Format

```
Offset  Size    Field
──────  ──────  ─────────────────────────
0       4       magic (0x474A4C4B = 'GJLB')
4       2       version (1)
6       2       block_count
8       4       tower_id (starting tower index)
12      4       total_cells = block_count × 48
16      4       flags
──────  ──────  ─────────────────────────
20      1024    block[0].floor[0].data[0..15]   (16 × 64B)
1044    16      block[0].floor[0].conn           (floor connector)
1060    1024    block[0].floor[1].data
2084    16      block[0].floor[1].conn
2100    1024    block[0].floor[2].data
3124    32      block[0].conn                    (block connector)
3156    1024    block[1].floor[0].data
...
```

**Per-block size:** `3 × (1024 + 16) + 32 = 3152 bytes`
**Total size:** `20 + block_count × 3152 bytes`

### 3.4 L-Block Within Floor

Each floor has 16 cells (Hilbert-ordered) divided into 4 L-blocks:

```
L-block 0: cells d=0,1,2 (data) + d=3 (in-floor connector)
L-block 1: cells d=4,5,6 (data) + d=7 (in-floor connector)
L-block 2: cells d=8,9,10 (data) + d=11 (in-floor connector)
L-block 3: cells d=12,13,14 (data) + d=15 (in-floor connector)
```

### 3.5 Connector Types

| Connector | Size | Bridges | Purpose |
|---|---|---|---|
| Floor connector | 16B | floor[f] ↔ floor[f+1] | Vertical prediction between floors |
| Block connector | 32B | block[b] ↔ block[b+1] | Cross-block prediction between Metatron blocks |
| In-floor connector | (part of L-block) | cell[d] ↔ cell[d+4] | Adjacent L-block boundary within floor |

### 3.6 API Reference

#### Address Mapping

```c
void     hc_gj_node_decompose(uint32_t node_id,
                              uint32_t *tower, uint32_t *block,
                              uint32_t *floor, uint32_t *cell);
uint32_t hc_gj_node_compose(uint32_t tower, uint32_t block,
                            uint32_t floor, uint32_t hilbert_cell);
uint32_t hc_gj_cell_to_hilbert(uint32_t flat_idx);
uint32_t hc_gj_hilbert_to_cell(uint32_t hilbert_d);
void     hc_gj_lblock_nodes(uint32_t tower, uint32_t block,
                            uint32_t floor, uint32_t lblock_idx,
                            uint32_t out_nodes[3], uint32_t *conn_node);
```

#### Writer

```c
int  hc_gj_writer_init(HC_GeoJumpWriter *w, uint32_t block_count,
                       uint32_t tower_id);
void hc_gj_writer_destroy(HC_GeoJumpWriter *w);
int  hc_gj_write_cell(HC_GeoJumpWriter *w, const uint8_t *cell_data);
void hc_gj_write_floor_conn(HC_GeoJumpWriter *w, uint32_t block_idx,
                            uint32_t floor_idx, const uint8_t *conn_data);
void hc_gj_write_block_conn(HC_GeoJumpWriter *w, uint32_t block_idx,
                            const uint8_t *conn_data);
uint32_t hc_gj_writer_node(const HC_GeoJumpWriter *w);
size_t hc_gj_serialized_size(const HC_GeoJumpWriter *w);
size_t hc_gj_serialize(const HC_GeoJumpWriter *w,
                       uint8_t *dst, size_t dst_cap);
```

#### Reader

```c
int  hc_gj_reader_init(HC_GeoJumpReader *r, const void *data, size_t data_sz);
int  hc_gj_verify(const HC_GeoJumpReader *r);
const uint8_t *hc_gj_get_cell(const HC_GeoJumpReader *r,
                              uint32_t block_idx, uint32_t floor_idx,
                              uint32_t cell_idx);
const uint8_t *hc_gj_get_floor_conn(const HC_GeoJumpReader *r,
                                    uint32_t block_idx, uint32_t floor_idx);
const uint8_t *hc_gj_get_block_conn(const HC_GeoJumpReader *r,
                                    uint32_t block_idx);
const uint8_t *hc_gj_get_node(const HC_GeoJumpReader *r, uint32_t node_id);
```

#### File I/O

```c
int hc_gj_write_file(const char *path, const HC_GeoJumpWriter *w);
int hc_gj_read_file(const char *path, HC_GeoJumpReader *r,
                    void **out_data, size_t *out_sz);
```

---

## 4. Pipeline Integration

### 4.1 Insertion Point

```
Current:  Input → Chunk(64B) → [Binary Shell] → Hamburger → GPX5
With L-block:
          Input → Chunk(64B) → [L-block Container] → Bond → Shell → Pixel → Hamburger → GPX5
```

### 4.2 Data Flow

```
Raw file
  │
  ▼
Stage 0: CHUNK (existing)
  │  Split into 64B chunks
  │
  ▼
Stage 1: L-BLOCK CONTAINER (new)
  │  - Receive 64B chunks
  │  - Arrange into Metatron blocks (4×4×3 = 48 cells)
  │  - Hilbert ordering within each floor
  │  - Add floor connectors (16B)
  │  - Add block connectors (32B)
  │  - Sparse cells → fill with zeros (FLAT codec handles)
  │
  │  Output: MetatronBlock[] (3152B/unit)
  │  Each chunk now has geo_jump node_id
  │
  ▼
Stage 2: BOND (existing)
  │  Use node_id directly as seed base
  │
  ▼
Stage 3: SHELL (existing)
  │  shell_addr from node_id
  │
  ▼
... → GPX5
```

### 4.3 Why Between Chunk and Bond

- **After Chunk:** Raw 64B chunks are available → L-block receives and organizes them
- **Before Bond:** Bond needs `seed` (fibo_addr) derived from chunk index → L-block ordering provides spatial context
- **Before Shell:** Shell uses `shell_addr` from geo_key → L-block node_id provides Metatron position

---

## 5. Sparse Data Handling

Sparse data (common in model weights) fills unused cells with zeros:

```
Raw:       48 cells × 64B = 3072B
           5 cells with data + 43 cells zeros

FLAT codec: 0×64B chunk → [0x00, 0x00] = 2B per chunk

Compressed: 5 × ~32B (geopixel) + 43 × 2B (FLAT) = ~246B
```

Container format stays uniform — no sparse bitmap, no special cases. Decoder sees all cells as present; FLAT codec compresses zeros automatically.

---

## 6. Spatial Locality Properties

### Hilbert Curve Property

Consecutive Hilbert cells are spatially nearby on the 2D grid:
- Manhattan distance between consecutive cells ≤ 2
- This means adjacent cells in the container are physically close in the model's weight space

### L-Block Co-access

The 3 cells in each L-block are almost always accessed together:
- **Prefetch:** Load entire L-block (192B) while processing current cell
- **Cache hit:** 192B fits in L1/L2 cache line → all 3 cells loaded together
- **SIMD batching:** Same operation on 3 cells in parallel
- **Page fault reduction:** mmap pages contain full L-blocks

### GeoJump 3D Locality

The Metatron structure adds vertical locality:
- Floor 0 ↔ Floor 1 ↔ Floor 2 are stacked vertically
- Floor connectors bridge adjacent floors
- Block connectors bridge adjacent Metatron blocks

---

## 7. Test Coverage

| Test Suite | Tests | Status |
|---|---|---|
| Standalone (`test_pogls_hilbert_container.c`) | 87 | ✅ All pass |
| GeoJump (`test_pogls_hc_geojump.c`) | 220 | ✅ All pass |
| **Total** | **307** | **✅** |

### Test Categories

- Hilbert XY↔D roundtrip
- L-block mapping (block_count, addresses, connectors)
- Writer: cell write, full-block detection, data verification
- Connector write + serialize/deserialize roundtrip
- File I/O roundtrip
- Spatial locality verification (Manhattan distance ≤ 2)
- Spatial lookup (d → block_idx + cell_pos)
- Edge cases (invalid order, full writer, bad magic)
- GeoJump node_id compose/decompose
- GeoJump L-block node mapping
- GeoJump floor/block connector roundtrip

---

## 8. Files

```
runner/pogls_hilbert_container/
├── pogls_hilbert_container.h    — Standalone header (544 lines)
├── pogls_hc_geojump.h           — GeoJump header (439 lines)
├── test_pogls_hilbert_container.c — Standalone tests
└── test_pogls_hc_geojump.c      — GeoJump tests
```

---

## 9. Build & Test

```bash
# Standalone
gcc -O2 -std=c11 -I. test_pogls_hilbert_container.c -o test_pogls_hilbert_container.exe
./test_pogls_hilbert_container.exe

# GeoJump
gcc -O2 -std=c11 -I../../geopixel/include -I. test_pogls_hc_geojump.c -o test_pogls_hc_geojump.exe
./test_pogls_hc_geojump.exe
```

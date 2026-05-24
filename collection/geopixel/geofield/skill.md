---
name: geofield
description: Unified Geometric Encode/Decode Field System — GpSphere × FrustumBlock × Metatron × Trit × Flow
---

## Architecture Overview

```
data ──→ [GeoField] ──→ FrustumBlocks ──→ file (.geofield)
              │
              ├── GpSphere (Goldberg polyhedron coords)
              ├── FrustumBlock (4896B container, 54×64B diamonds)
              ├── Metatron (shape routing: ORBITAL/CHIRAL/CROSS/HUB)
              ├── Trit (3³=27 decomposition)
              └── Flow (content-driven chunk boundaries)
```

### Key Concepts
| Term | Meaning |
|------|---------|
| **FrustumBlock** | 4896B storage container: 3456B data (54×64B) + 1440B metadata |
| **GpAddr** | `{tile_id, dim}` — coordinate on Goldberg sphere |
| **gp_level** | 1..8 subspace richness | Subdivision depth: 1=12 tiles, 8=642 |
| **Pentagon** | 12 fixed anchor tiles (tile_id 0..11) — Euler invariant |
| **Tring** | Perpendicular fiber channel across dimension layers |
| **Metatron** | Cube routing between 12 pentagon faces |

---

## File Structure

| File | Purpose |
|------|---------|
| `geo_field_core.h` | **API v1** — basic encode/decode/scale/save/load |
| `geo_field_core_2.h` | **API v2** — adds heptagon fence + atomic reshape on write |
| `geo_field_core_3.h` | **API v3 (recommended)** — fixes fiber+packed+route+advisory fence |
| `geo_field_main.c` | Demo 4-in-1 program |
| `geo_flow_chunker_v8.h` | Content-driven chunk boundary detection |
| `geo_flow_chunker_v8_1.h` | v8.1 — `int64_t` return type fix |
| `*.h` (17 files) | Dependency headers (tring, frustum, Metatron, fabric, goldberg, etc.) |
| `Makefile` | Build system (`make`, `make run`) |

---

## API Reference (geo_field_core_3.h — latest)

### Init / Free
```c
int geo_field_init(GeoField *gf, uint8_t gp_level, uint32_t n_blocks);
void geo_field_free(GeoField *gf);
```

### Encode (data → FrustumBlock array)
```c
int geo_field_encode_chunk(GeoField *gf, uint64_t chunk_idx,
                            const uint8_t chunk[64],
                            SkelEncCtx *skel, GeoFieldEncodeStats *stats);
int geo_field_encode(GeoField *gf, const uint8_t *data, size_t data_sz,
                      GeoFieldEncodeStats *stats);
```

### Decode (FrustumBlock array → data)
```c
int geo_field_decode_chunk(const GeoField *gf, uint64_t chunk_idx,
                            uint8_t out[64]);
int64_t geo_field_decode(const GeoField *gf, uint8_t *out, size_t max_sz,
                          GeoFieldDecodeStats *stats);
```

### Complete Roundtrip
```c
int geo_field_roundtrip(const uint8_t *data, size_t data_sz, uint8_t gp_level,
                         GeoFieldEncodeStats *enc, GeoFieldDecodeStats *dec);
// Returns: 0=OK, -1=encode fail, -2=decode fail, -3=data mismatch
```

### Scale (Zoom)
```c
uint32_t geo_field_zoom_in(uint8_t current_level, uint8_t *out_new_level);
uint32_t geo_field_zoom_out(uint8_t current_level, uint8_t *out_new_level);
GpAddr  geo_field_scale_addr(const GpAddr *src, uint8_t src_level, uint8_t dst_level);
int     geo_field_encode_multires(const uint8_t *data, size_t data_sz,
                                   uint8_t min_level, uint8_t max_level,
                                   GeoField *fields_out, GeoFieldEncodeStats *stats_out);
```

### Shape Dimension Access (Metatron Routing)
```c
uint16_t geo_field_tile_to_enc(uint32_t tile_id, uint8_t gp_level);
GeoFieldShapeAccess geo_field_shape_route(uint32_t src_tile_id,
                                           uint8_t dst_face, uint8_t gp_level);
int geo_field_shape_read(const GeoField *gf, uint64_t chunk_idx,
                          MetaRouteType route, uint8_t dst_face,
                          uint8_t out_chunk[64]);
// route: META_ROUTE_ORBITAL / CHIRAL / CROSS / HUB
```

### Fiber (per-tile dimension traversal)
```c
int geo_field_fiber_count(const GeoField *gf, uint32_t tile_id);
typedef void (*geo_fiber_cb)(void *ctx, uint32_t tile_id,
                              uint8_t dim, const uint8_t chunk[64]);
int geo_field_fiber_walk(const GeoField *gf, uint32_t tile_id,
                          geo_fiber_cb cb, void *ctx);
```

### File I/O
```c
int     geo_field_save(const GeoField *gf, const uint8_t *data_orig,
                        size_t data_sz, const char *path);
int64_t geo_field_load(GeoField *gf, const char *path);
// File format: [32B header] + [n × 4896B FrustumBlock]
```

### Stats Structures
```c
typedef struct {
    uint64_t total_chunks, total_blocks, zone_resets;
    uint32_t skel_hits[6];  // IDENTITY/FLAT/DIFF/BREF/GEOM/RAW histogram
} GeoFieldEncodeStats;

typedef struct {
    uint64_t chunks_decoded, chunks_missing, bytes_written;
} GeoFieldDecodeStats;
```

---

## Build & Run

```bash
cd I:\FGLS_new\collection\geopixel\geofield
make          # compiles geo_field_demo.exe
make run      # runs demo (4 tests)
make clean    # removes binary + .geofield files
```

**Include in your project:**
```c
#include "geo_field_core.h"   // or geo_field_core_3.h for latest
// Only need: -Ipath/to/geofield
```

---

## Feature Details

### 1. Complete Encode/Decode Loop

**Flow:**
```
input file → 64B chunks → GpAddr{tile_id, dim} → FrustumBlock[block_idx].data[slot*64]
                     ↓  drain_state[] bit0 = active marker
                     ↓  heptagon_fence_write() → PATH A advisory
                     ↓  reshape_collect() → atomic_reshape() if threshold met
decode: drain_state bit0 check → memcpy from data[slot*64] → reassemble
```

- Chunk→addr mapping: `tile_id = chunk_idx % face_max`, `dim = chunk_idx / face_max`
- Block addressing: `block_idx = dim × blocks_per_layer + tile_id ÷ 54`
- Pentagon tiles (0..11) trigger skeleton zone reset + fence PATH A

### 2. Scale (Resolution Zoom)

- `gp_face_count(n) = 10n² + 2`
- Pentagons are fixed across levels; hexagons scale proportionally
- `geo_field_encode_multires()` encodes same data at multiple levels simultaneously

### 3. Shape Routing (Metatron)

4 O(1) route types between 12 pentagon faces (720-slot encoding space):

| Route | Operation | Example |
|-------|-----------|---------|
| ORBITAL | Same face, slot+1 | face 0 → face 0 (slot+1) |
| CHIRAL | Opposite face (f↔f+6) | face 0 → face 6 |
| CROSS | Inter-ring 3-step | face 0 → face 9 |
| HUB | Any face via center | face 0 → face 1..5,7..11 |

Circuit switch condition: `tri[5] = metatron_cond(enc) = face×3 + enc%3`, range 0..35

### 4. Fiber (Perpendicular Channel)

- Same tile_id across all dimension layers (0..7)
- Fixed: v3 uses `gp_blk_read()` directly from FrustumBlock
- Example: read how tile 5 changes across zoom levels

### 5. File Format

```
Offset  Size  Content
──────  ────  ───────
     0     4  Magic "GEOF"
     4     1  Version (1)
     5     1  gp_level
     6     1  _pad
     7     4  n_blocks
    11     8  orig_size
    19     8  digest (xxh64)
    27     4  _pad
    32  4896  FrustumBlock[0]
  4928  4896  FrustumBlock[1]
    ...
```

---

## Writing New Features

### Pattern: Add a new operation on GeoField
```c
static inline int geo_field_my_op(const GeoField *gf, /* params */) {
    if (!gf || !gf->blocks) return -1;
    // Iterate blocks, use gp_blk_read()/gp_blk_write() for data
    // Use drain_state[] for existence checks
    // Use frustum_header() for FrustumHeader access
}
```

### Pattern: Add a new route type (Metatron extension)
1. Add to `MetaRouteType` enum in `geo_metatron_route.h`
2. Implement routing function (e.g., `meta_myroute(enc)`)
3. Add case in `geo_field_shape_read()` switch
4. Add name/display in demo

### Pattern: Custom stats collector
```c
typedef struct { /* your counters here */ } MyStats;
// Pass through GeoFieldEncodeStats or use separate callback
```

### Important Rules
- **No malloc in hot path** — pre-allocate in `geo_field_init()`
- **No float** — all integer arithmetic
- **O(1)** — no search, no recursion
- **Header-only** — all functions `static inline`; put `#define` before include to control
- **Read from FrustumBlock** — use `gp_blk_read()` not `gp_lens_read()` (Tring not populated on encode)
- **FrustumBlock packed** — `__attribute__((packed))` on structs; `_Static_assert` validates sizes

---

## Version History

| Version | File | Changes |
|---------|------|---------|
| v1 | `geo_field_core.h` | Basic encode/decode/scale/route/save/load |
| v2 | `geo_field_core_2.h` | +heptagon_fence PATH A gate, +atomic_reshape, +`goto skel_step` (data loss on deny) |
| v3 | `geo_field_core_3.h` | **Fixed** fiber→`gp_blk_read`, header packed, route switch, fence advisory‑only, no data loss |

**v3 recommended for all new development.**

# geo_flow_chunker_v8.h

> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.

**Module:** `collection`  
**Path:** `collection/geopixel/geofield/geo_flow_chunker_v8.h`  
**Status:** `stale`  
**Note:** last modified 47d ago  
**Generated:** 2026-07-30 02:42  

## Description

* geo_flow_chunker.h — Content-driven chunk boundaries using geometric flow
* Based on test_integrity_v2: DiamondBlock flow uses `fold_fibo_intersect`
* popcnt as a measure of geometric structure. When popcnt drops to 0,
* the "vein" has run out — it's a natural dead zone → chunk boundary.
* This produces variable-length chunks that align with the data's own
* geometric structure, unlike fixed 64B partitions that may cut through
* patterns.
* Algorithm:
*   Fit a DiamondBlock to every byte position via fold_block_init(face,edge,z).
*   fold_fibo_intersect ≈ 0 near unstructured inter-seam zones.
*   Use min_chunk and max_chunk constraints to avoid degenerate splits.
* Parameters:
*   window_sz      — DiamondBlock stride (default 64, same as chunk)
*   min_chunk      — minimum chunk size in bytes (default 32)
*   max_chunk      — maximum chunk size (default 4096)
*   isect_thresh   — popcnt threshold for dead zone (default 0)
* Output: array of {offset, length} segment descriptors.

## Structures

- `typedef struct`

## API Functions

- `static inline uint64_t flow_derive_seed(const uint8_t *data)`
- `static inline void flow_derive_coord(uint64_t seed, uint8_t *face, uint8_t *edge, uint8_t *z)`
- `static inline int64_t flow_isect_at(const uint8_t *data, size_t data_sz, size_t offset)`
- `return __builtin_popcountll(isect)`
- `static inline int flow_chunk(const uint8_t *data, size_t data_sz,`
- `static inline void flow_segments_free(FlowSegment *segs)`

## Constants

- `#define FLOW_MIN_CHUNK    32u`
- `#define FLOW_MAX_CHUNK    4096u`
- `#define FLOW_WINDOW       64u`
- `#define FLOW_ISECT_THRESH 0u    /* dead zone = popcnt <= this */`


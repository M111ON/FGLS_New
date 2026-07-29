# geo_field_core.h

> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.

**Module:** `collection`  
**Path:** `collection/geopixel/geofield/geo_field_core.h`  
**Status:** `stale`  
**Note:** last modified 65d ago  
**Generated:** 2026-07-29 19:18  

## Description

* geo_field_core.h — Unified Geometric Encode/Decode Field
* ═══════════════════════════════════════════════════════════════════════
* Integrates:
*   GpSphere (Goldberg polyhedron)  — tile_id × dim coordinate system
*   FrustumBlock (4896B container)  — geometric storage backend
*   Metatron routing                — shape dimension access / navigation
*   Trit decomposition              — address encoding
*   Flow chunking                   — content-driven boundary detection
* Complete loop:
*   encode: file → chunks → GpAddr → FrustumBlock → serialize
*   decode: serialize → FrustumBlock → GpAddr → chunks → file
* Scale (zoom in/out):  change gp_level (1..8) → more/fewer tiles
*   → more tiles = finer subdivision = "zoomed in"
*   → fewer tiles = coarser subdivision = "zoomed out"
* Shape dimension access:  navigate between pentagon faces (0..11)
*   via Metatron's 4 route types:
*     ORBITAL — stay on same face, slot+1
*     CHIRAL  — jump to opposite face (face ↔ face+6)
*     CROSS   — inter-ring non-chiral
*     HUB     — any face via center

## Structures

- `typedef struct`
- `typedef struct`
- `typedef struct`
- `typedef struct`
- `typedef struct`

## API Functions

- `static inline int geo_field_init(GeoField *gf, uint8_t gp_level,`
- `static inline void geo_field_free(GeoField *gf)`
- `static inline int geo_field_encode_chunk(GeoField         *gf,`
- `static inline int geo_field_encode(GeoField           *gf,`
- `static inline int geo_field_decode_chunk(const GeoField *gf,`
- `static inline int64_t geo_field_decode(const GeoField  *gf,`
- `static inline GpAddr geo_field_scale_addr(const GpAddr *src,`
- `static inline uint32_t geo_field_zoom_in(uint8_t current_level,`
- `return gp_face_count(nl)`
- `static inline uint32_t geo_field_zoom_out(uint8_t current_level,`
- `return gp_face_count(nl)`
- `static inline int geo_field_encode_multires(const uint8_t  *data,`
- `static inline uint16_t geo_field_tile_to_enc(uint32_t tile_id,`
- `static inline GeoFieldShapeAccess geo_field_shape_route(uint32_t     src_tile_id,`
- `static inline int geo_field_shape_read(const GeoField     *gf,`
- `return geo_field_decode_chunk(gf,`
- `static inline int geo_field_fiber_count(const GeoField *gf, uint32_t tile_id)`
- `typedef void (*geo_fiber_cb)(void *ctx, uint32_t tile_id,`
- `static inline int geo_field_fiber_walk(const GeoField *gf,`
- `static inline uint64_t _gf_rot(uint64_t x, int r)`

## Constants

- `#define GEO_FIELD_CORE_H`
- `#define GF_CHUNK_SZ         64u        /* Diamond lens = 1 cache line    */`
- `#define GF_BLOCK_DATA_SZ    FGLS_DATA_BYTES  /* 3456 = 54×64B           */`
- `#define GF_BLOCK_TOTAL_SZ   FGLS_TOTAL_BYTES /* 4896 = full FrustumBlock */`
- `#define GF_CHUNKS_PER_BLOCK (GF_BLOCK_DATA_SZ / GF_CHUNK_SZ)  /* 54    */`
- `#define GF_FILE_MAGIC   "GEOF"`
- `#define GF_FILE_VERSION 1u`


# geo_o4_connector.h

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `geopixel`  
**Path:** `geopixel/include/geopixel/geo_o4_connector.h`  
**Status:** `active`  
**Note:** modified 25d ago; included by 3 file(s)  
**Generated:** 2026-07-30 02:41  

## Description

* geo_o4_connector.h — O4 Connector: GeoPixel v18 tile → TRing → GeoPixel grid
* ══════════════════════════════════════════════════════════════════════════════
* Bridges geopixel_v18 encode output → POGLS geometric storage.
* Pipeline (O4):
*   encode_tile() blob  (v18 output, variable size)
*       ↓
*   slice blob → 3B chunks (GV_CHUNK_BYTES)
*       ↓
*   for each chunk: trit/spoke/coset/letter/fibo fields via geo_pixel_encode()
*       ↓
*   assign to TringAddr slot via tring_encode(tx, ty, ch)
*       ↓
*   write into 27×N pixel grid (W=27 canonical width)
*       ↓
*   ready for PNG lossless encode (geometric pattern → high compression)
* Read path (O4 reverse):
*   PNG decode → 27×N pixel grid
*       ↓
*   geo_pixel_decode() → fields
*       ↓

## Structures

- `typedef struct`
- `typedef struct`

## API Functions

- `static inline void o4_encode(const uint8_t *blob, uint32_t blob_sz,`
- `static inline void o4_decode(const O4GridCtx *ctx,`
- `static inline TringAddr o4_tile_route(uint8_t tx, uint8_t ty, uint8_t ch)`
- `return tring_encode(tx, ty, ch)`
- `static inline uint32_t o4_grid_to_rgb(const O4GridCtx *ctx,`
- `static inline void o4_rgb_to_grid(const uint8_t *rgb, uint32_t grid_h,`
- `static inline uint32_t o4_roundtrip_verify(void)`

## Constants

- `#define GEO_O4_CONNECTOR_H`
- `#define O4_CHUNK_BYTES    3u      /* bytes per GeoPixel slot           */`
- `#define O4_GRID_W        27u      /* canonical grid width (trit space) */`
- `#define O4_MAX_SLOTS   6912u      /* TRING_TOTAL                       */`
- `#define O4_MAX_GRID_H   256u      /* max rows: 256×27 = 6912 slots     */`
- `#define O4_GRID_PIXELS (O4_GRID_W * O4_MAX_GRID_H)  /* 186,624 B max  */`


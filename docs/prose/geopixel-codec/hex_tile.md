# hex_tile.h

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `geopixel`  
**Path:** `geopixel/include/hex_tile.h`  
**Status:** `active`  
**Note:** modified 24d ago; included by 8 file(s)  
**Generated:** 2026-07-29 18:52  

## Structures

- `typedef struct`

## API Functions

- `static inline int _hex_triplet_flat(const HexTile *t, int ti)`
- `static inline uint8_t _hex_predict(const HexTile *t)`
- `static inline uint8_t _hex_classify(const HexTile *t)`
- `static inline int hex_tile_encode(const HexTile *t, uint8_t *dst)`
- `static inline int hex_tile_decode(const uint8_t *src, size_t src_len, HexTile *t)`

## Constants

- `#define HEX_CELLS    7`
- `#define HEX_CENTER   6`
- `#define HEX_RING     6`
- `#define HENC_FLAT         0x00`
- `#define HENC_TRIPLET_FLAT 0x01`
- `#define HENC_GRADIENT     0x02`
- `#define HENC_EDGE         0x03`


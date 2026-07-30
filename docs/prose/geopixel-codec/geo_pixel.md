# geo_pixel.h

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `geopixel`  
**Path:** `geopixel/include/geopixel/geo_pixel.h`  
**Status:** `active`  
**Note:** modified 25d ago; included by 2 file(s)  
**Generated:** 2026-07-30 04:45  

## Description

* geo_pixel.h — GeoPixel Encode/Decode Roundtrip
* ═══════════════════════════════════════════════
* Encodes geometric index → RGB pixel.
* Decode: reconstruct (trit,spoke,coset,letter,fibo) from RGB.
* Encode formula (Roadmap Phase 3):
*   R = ((idx%27)<<3) | (idx%6)        — trit(5b) | spoke(3b)
*   G = ((idx%9)<<4)  | (idx%26 & 0xF) — coset(4b) | letter_lo(4b)
*   B = idx % 144                       — fibo clock position
* Uniqueness guarantee:
*   W=27 grid → no two pixels share all 5 fields (trit,spoke,coset,letter,fibo)
* No malloc. No float. No heap.
* ═══════════════════════════════════════════════
═══════════════════════════════════════
ENCODE: idx → GeoPixel
═══════════════════════════════════════
═══════════════════════════════════════
DECODE: GeoPixel → GeoFields
Note: letter reconstruction is lossy above bit 3 (only low 4 bits stored).
Full letter (0..25) cannot be recovered from G alone.
Roundtrip verifies fields that ARE losslessly stored.

## Structures

- `typedef struct`
- `typedef struct`

## API Functions

- `static inline GeoPixel geo_pixel_encode(uint32_t idx, uint32_t W)`
- `static inline GeoFields geo_pixel_decode(GeoPixel p)`
- `static inline uint32_t geo_pixel_roundtrip_verify(uint32_t W, uint32_t H)`
- `static inline uint32_t geo_pixel_uniqueness_check(uint32_t W)`
- `static inline uint8_t geo_pixel_to_trit(GeoPixel p)`
- `return geo_pixel_decode(p).trit`

## Constants

- `#define GEO_PIXEL_H`
- `#define GP_TRIT_MOD    27u`
- `#define GP_SPOKE_MOD    6u`
- `#define GP_COSET_MOD    9u`
- `#define GP_LETTER_MOD  26u`
- `#define GP_FIBO_MOD   144u`
- `#define GP_GRID_W      27u   /* canonical grid width */`


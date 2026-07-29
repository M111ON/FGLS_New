# geo_goldberg_tile.h

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `geopixel`  
**Path:** `geopixel/include/geopixel/geo_goldberg_tile.h`  
**Status:** `active`  
**Note:** modified 24d ago; included by 19 file(s)  
**Generated:** 2026-07-29 20:36  

## Description

* geo_goldberg_tile.h — Goldberg GP(1,1) Tile Integration for GeoPixel
* ═══════════════════════════════════════════════════════════════════════
* Two integration points:
*   POINT 3 — Circuit-based residual routing (per tile)
*     ggt_tile_scan()  → GGTileResult { circuit_fired, max_tension }
*     OR aggressive: if circuit says LOOSE → override to LOOSE regardless of gerr
*   POINT 2 — Blueprint dedup index (per 144-tile window)
*     ggt_window_feed()  → accumulate stamp per tile
*     ggt_window_flush() → GGBlueprint { stamp_hash, circuit_map, window_id }
*     Blueprint stored in index; matching stamp_hash → ref pointer dedup
* Self-contained — baked LUT inline, no external POGLS headers needed.
* ═══════════════════════════════════════════════════════════════════════
─────────────────────────────────────────────────────────────────────
* POINT 3: Per-tile scan
* Samples the tile by mapping 32 pixel positions → 32 Goldberg faces.
* Pixel sampling: stride across tile to cover spatial distribution.
* Each face gets one pixel value XOR'd into its state.
* Usage:
*   GGTileResult r = ggt_tile_scan(iY, x0,y0,x1,y1, W);
*   if (r.n_circuits >= 3) → LOOSE or DELTA

## Structures

- `typedef struct`
- `typedef struct`
- `typedef struct`
- `typedef struct`

## API Functions

- `static inline GGTileResult ggt_tile_scan(const int *iY,`
- `static inline int ggt_mode_override(int base_mode, const GGTileResult *r)`
- `else if(r->n_circuits >= 3) gb_mode = GGT_MODE_LOOSE`
- `static inline void ggt_window_init(GGWindow *w)`
- `static inline void ggt_window_feed(GGWindow *w, const GGTileResult *r)`
- `static inline GGBlueprint ggt_window_flush(GGWindow *w)`

## Constants

- `#define GEO_GOLDBERG_TILE_H`
- `#define GGT_N_FACES     32   /* 12 pentagon + 20 hexagon              */`
- `#define GGT_N_TRIGAP    60   /* triangle gaps = I-symmetry order      */`
- `#define GGT_N_PAIRS      6   /* bipolar pentagon pairs                */`
- `#define GGT_FLUSH_PERIOD 144 /* Fibonacci flush boundary              */`
- `#define GGT_MODE_NORMAL  0`
- `#define GGT_MODE_LOOSE   1`
- `#define GGT_MODE_DELTA   2`


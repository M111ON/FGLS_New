# geopixel_v21_o25.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `geopixel`  
**Path:** `geopixel/src/geopixel_v21_o25.c`  
**Status:** `active`  
**Note:** modified 24d ago  
**Generated:** 2026-07-29 05:10  

## Description

* GeoPixel v18 — Goldberg Full Integration: Point 2 (dedup) + Point 3 (pure circuit)
* Point 3: gerr REMOVED — pure circuit_fired replaces all threshold logic
*   n_circuits 0-2 → GRAD9_NORMAL   (simple, avg_var ~215)
*   n_circuits 3-5 → GRAD9_LOOSE    (moderate, avg_var ~311-437)
*   n_circuits 6   → DELTA          (complex, avg_var ~575)
*   Mapping calibrated from probe data on test01.bmp
* Point 2: Blob dedup via stamp hash (XOR fold of blob bytes)
*   Duplicate tiles → ref-pointer in index (bit31 set)
*   Decoder resolves ref-pointer transparently
*   Dedup table: 4096 slots, 16-probe linear hash
* Compile: gcc -O3 -o geopixel_v18 geopixel_v18_c.c -lm -lzstd -lpthread
════════════════════════════════════════════════════════
* O23: Pentagon Address System (O21 geometry, embedded)
* Used by encoder to build GEOA layer (GPX4_LAYER_GEO)
* ════════════════════════════════════════════════════════
* o23_build_geo_addrs — O21 geometry + balance-refine
*   W,H    : image pixel dims
*   TW,TH  : tile grid dims (W/TILE, H/TILE)
*   out    : caller-allocated Gpx4GeoAddr[TW*TH]
*   Balance-refine: pentagons with >1.5x mean tile count

## Structures

- `typedef struct`
- `typedef struct`
- `typedef struct`
- `typedef struct`
- `typedef struct`
- `typedef struct`
- `typedef struct`
- `typedef struct`
- `typedef struct`
- `struct timespec t0,t1`

## API Functions

- `static O23V3 o23v3n(O23V3 v)`
- `static double o23v3d(O23V3 a,O23V3 b)`
- `static void o23_build_ico(void)`
- `static int o23_nearest_pent(O23V3 s)`
- `static uint32_t o23_hilbert(uint32_t n,uint32_t x,uint32_t y)`
- `static void o23_build_geo_addrs(int W,int H,int TW,int TH,Gpx4GeoAddr *out)`
- `static inline uint8_t pack_i6(int v)`
- `static inline int unpack_i6(uint8_t v)`
- `static int compute_grad_error(`
- `static FILE *gv_open_memory_file(const uint8_t *data, size_t size)`
- `static int bmp_load(const char *path, Img *img)`
- `static void bmp_save(const char *path, const uint8_t *px, int w, int h)`
- `static inline void rgb_to_ycgco(int r,int g,int b,int*Y,int*Cg,int*Co)`
- `static inline void ycgco_to_rgb(int Y,int Cg,int Co,int*r,int*g,int*b)`
- `static inline int clamp255(int v)`
- `static inline uint16_t zigzag(int16_t v)`
- `static inline int16_t unzigzag(uint16_t v)`
- `static void     w32(uint8_t*b,int o,uint32_t v)`
- `static uint32_t r32(const uint8_t*b,int o)`
- `static void     w16(uint8_t*b,int o,uint16_t v)`

## Constants

- `#define GEO_GPX_ANIM_IMPL`
- `#define NOISE_HIST_N 10`
- `#define GGT_MAX_BLUEPRINTS  64`
- `#define MAGIC_V13   0x47503D78u  /* v15 magic */`
- `#define TILE        32`
- `#define ZST_LVL     9`
- `#define O23_PHI 1.61803398874989484820`
- `#define O23_HILBERT_N 16  /* must match tile grid (512/32=16) */`
- `#define MAX_FLAT_COLORS 4`
- `#define TTYPE_FLAT     0`
- `#define TTYPE_GRADIENT 1`
- `#define TTYPE_EDGE     2`
- `#define TTYPE_NOISE    3`
- `#define BMODE_NONE    0   /* FLAT: no boundary stored */`
- `#define BMODE_LINEAR2 1   /* smooth boundary: Y0+dx+dy per ch = 9B */`


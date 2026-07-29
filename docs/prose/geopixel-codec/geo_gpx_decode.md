# geo_gpx_decode.h

> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.

**Module:** `geopixel`  
**Path:** `geopixel/include/geopixel/geo_gpx_decode.h`  
**Status:** `stale`  
**Note:** included by only 1 file(s)  
**Generated:** 2026-07-29 22:20  

## Description

* geo_gpx_decode.h  —  Standalone GPX2 decoder  (header-only)
* Dependencies:  geo_o4_connector.h, libpng, libzstd
* Usage:
*   #define GEO_GPX_DECODE_IMPL   (once, before include)
*   #include "geo_gpx_decode.h"
*   int gpx_decode_to_bmp(const char *gpx, const char *bmp_out);
*   // returns 0 on success, non-zero on error
* Optional: decode into caller-owned RGB buffer (W*H*3, row-major).
* Caller must free *px_out. *w_out and *h_out are set on success.
═══════════════════════════════════════════════════════════════════
* IMPLEMENTATION
* ═══════════════════════════════════════════════════════════════════

## API Functions

- `int gpx_decode_to_bmp(const char *gpx_path, const char *out_bmp)`
- `int gpx_decode_to_rgb(const char *gpx_path,`
- `static inline void _gpx_ycgco_to_rgb(int Y,int Cg,int Co,int*r,int*g,int*b)`
- `static inline int  _gpx_clamp255(int v)`
- `static inline int16_t _gpx_unzigzag(uint16_t v)`
- `static inline uint16_t _gpx_r16(const uint8_t*b,int o)`
- `static inline uint32_t _gpx_r32(const uint8_t*b,int o)`
- `static inline int _gpx_unpack_i6(uint8_t v)`
- `static void _gpx_bdec_linear2(const uint8_t*blob,int*off,`
- `static void _gpx_bdec_delta(const uint8_t*blob,int*off,`
- `static void _gpx_bdec_grad9(const uint8_t*blob,int*off,`
- `static void _gpx_bdec_ch(int bmode,const uint8_t*blob,int*off,`
- `else if(bmode==_BMODE_LINEAR2) _gpx_bdec_linear2(blob,off,col,row,corner,th,tw)`
- `else                           _gpx_bdec_delta   (blob,off,col,row,corner,th,tw)`
- `static inline int _gpx_predict(int pid,`
- `else if(x==0&&y>0) TL=cl?cl[y-1]:def`
- `else if(x>0&&y==0) TL=ct?ct[x-1]:def`
- `static void _gpx_decode_tile_blob(`
- `static uint8_t* _gpx_png_decode(const uint8_t*buf, size_t bufsz,`
- `int gpx_decode_to_rgb(const char *gpx_path,`

## Constants

- `#define GEO_GPX_DECODE_H`


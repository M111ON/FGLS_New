# geo_gpx_anim_o23.h

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `geopixel`  
**Path:** `geopixel/include/geopixel/geo_gpx_anim_o23.h`  
**Status:** `active`  
**Note:** modified 25d ago; included by 16 file(s)  
**Generated:** 2026-07-30 00:39  

## Description

* geo_gpx_anim.h — GPX4 animation encoder / decoder  (header-only, v2)
* Keyframe path: tile RGB → ZSTD blob → O4 geometric encode → PNG
* Delta path:    per-tile YCgCo int8 diff → ZSTD → concatenated blob
* GPX4 animation file layout:
*   AHDR  — Gpx4AnimHdr  (16B, no tile table)
*   F000  — O4 keyframe  (tile table + PNG blob of 27×H O4 grid)
*   D001  — DELTA frame  (tile table + concatenated ZSTD delta blobs)
*   D002  — ...
*   F016  — O4 keyframe  (every keyframe_interval frames)
* Dependencies: gpx4_container.h, geo_o4_connector.h, libpng, libzstd
* Usage:
*   #define GEO_GPX_ANIM_IMPL
*   #include "geo_gpx_anim.h"
*   GpxAnimEncCfg cfg = {24,1,16,9,32};
*   gpx_anim_encode(frames, n_frames, W, H, &cfg, "out.gpx4");
*   gpx_anim_decode("out.gpx4", my_cb, userdata);
════════════════════════════════════════════════════════
* PUBLIC: gpx_anim_encode
* ════════════════════════════════════════════════════════
════════════════════════════════════════════════════════

## Structures

- `typedef struct`

## API Functions

- `int gpx_anim_encode(uint8_t **frames, int n_frames, int W, int H,`
- `typedef int (*GpxAnimFrameCb)(int frame_idx, uint8_t *rgb, void *ud)`
- `int gpx_anim_decode(const char *path, GpxAnimFrameCb cb, void *ud)`
- `int gpx_anim_info  (const char *path, Gpx4AnimHdr *hdr_out)`
- `static inline void _ga_to_ycgco(int r,int g,int b,int*Y,int*Cg,int*Co)`
- `static inline void _ga_to_rgb(int Y,int Cg,int Co,int*r,int*g,int*b)`
- `static inline int _ga_clamp(int v)`
- `static inline int8_t _ga_ci8(int v)`
- `static uint8_t* _ga_png_enc(const uint8_t *rgb, uint32_t W, uint32_t H,`
- `static uint8_t* _ga_png_dec(const uint8_t *blob, uint32_t bsz,`
- `static uint8_t* _ga_tile_enc(const uint8_t *img,`
- `static void _ga_tile_dec(const uint8_t *blob, uint32_t bsz,`
- `static int _ga_enc_keyframe(`
- `static int _ga_dec_keyframe(`
- `static int _ga_enc_delta(`
- `int gpx_anim_encode(uint8_t **frames, int n_frames, int W, int H,`
- `int gpx_anim_decode(const char *path, GpxAnimFrameCb cb, void *ud)`
- `int gpx_anim_info(const char *path, Gpx4AnimHdr *hdr_out)`

## Constants

- `#define GEO_GPX_ANIM_H`


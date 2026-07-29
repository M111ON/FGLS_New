# pogls_to_geopixel.h

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `geopixel`  
**Path:** `geopixel/include/pogls_to_geopixel.h`  
**Status:** `active`  
**Note:** modified 24d ago; included by 6 file(s)  
**Generated:** 2026-07-29 17:08  

## Description

* pogls_to_geopixel.h — POGLS H64 → Geopixel Bridge  (Path A: CODEC_SEED)
* ═══════════════════════════════════════════════════════════════════════
*  Path A: Pure seed storage — every tile stores seed only (4B)
*  No pixel synthesis. No content storage. Only geometric address.
*  Pipeline:
*    ScanEntry stream
*        ↓  pogls_hilbert64_encoder.h
*    HilbertPacket64  (64 cells, RGB balanced, invert derived)
*        ↓  THIS FILE
*    H64TileIn[]  →  hamburger CODEC_SEED path
*        ↓
*    .gpx5 output:  4B × 64 tiles = 256B per packet
*  Storage per packet:
*    positive tiles  : 48 × 4B = 192B
*    invert tiles    : 12 × 4B =  48B  (derived, verify-only)
*    ghost tiles     : 0..4 × 4B
*    header          : 16B  (one per file)
*    ──────────────────────────────────
*    max per packet  : 256B  vs  4096B raw  →  16× theoretical ratio
*  Decoder re-derives content from seed + epoch + face_count.

## Structures

- `typedef struct __attribute__((packed))`
- `typedef struct`
- `typedef struct`

## API Functions

- `static inline uint32_t h64_to_geopixel(const H64Encoder  *enc,`
- `static inline uint32_t h64_pipeline(const ScanEntry   *entries,`
- `return h64_to_geopixel(&enc, out)`
- `static inline uint32_t h64_storage_bytes(uint32_t n_tiles)`
- `static inline float h64_ratio(uint32_t n_tiles)`

## Constants

- `#define POGLS_TO_GEOPIXEL_H`
- `#define H64_BRIDGE_MAGIC  0x48363450u   /* "H64P" */`
- `#define H64_BFLAG_GEOMETRIC    0x01u`
- `#define H64_BFLAG_GHOST_LIVE   0x02u`
- `#define H64_BFLAG_RGB_BALANCED 0x04u`
- `#define H64_BFLAG_SEED_ONLY    0x08u   /* Path A active                */`
- `#define H64T_FLAG_VALID    0x01u`
- `#define H64T_FLAG_INVERT   0x02u`
- `#define H64T_FLAG_GHOST    0x04u`
- `#define H64T_FLAG_PASSTHRU 0x08u`


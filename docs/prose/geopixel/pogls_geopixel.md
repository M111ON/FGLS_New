# pogls_geopixel.h

**Module:** `pogls_geopixel`  
**Path:** `pogls_geopixel/pogls_geopixel.h`  
**Generated:** 2026-07-28 10:21  

## Description

* pogls_geopixel.h — Standalone Geopixel Spatial Coherence Compression
* ═══════════════════════════════════════════════════════════════════════
* Compresses 64-byte blocks by exploiting spatial coherence:
*   FLAT     (0x00): All 64 bytes identical           → 2 bytes
*   SMOOTH   (0x01): Near-constant (|byte-mean| ≤ 16) → 10 bytes
*   GRADIENT (0x02): Piecewise-linear trend            → 10 bytes
*   EDGE     (0x03): Random/no pattern (raw)           → 65 bytes
* Full tensor encode splits data into 64-byte blocks and emits a tagged
* stream. Decoder dispatches on tag byte per block.
* Hilbert 2D curve maps 8×8 block coordinates to a 1D index (order=3).
* Session feed tracks incremental frames + block accumulation.
* No libpng, no file I/O, no heap in hot path.
* Dependencies: <stdint.h> <stddef.h> <string.h> <stdio.h>
* ═══════════════════════════════════════════════════════════════════════
═══════════════════════════════════════════════════════════════════════
CONSTANTS
═══════════════════════════════════════════════════════════════════════
═══════════════════════════════════════════════════════════════════════
SESSION
═══════════════════════════════════════════════════════════════════════

## Structures

- `typedef struct`

## API Functions

- `uint32_t pogls_geopixel_encode_block(uint8_t *dst, size_t dst_cap,`
- `uint32_t pogls_geopixel_decode_block(uint8_t *dst, size_t dst_cap,`
- `uint32_t pogls_geopixel_encode(uint8_t *dst, size_t dst_cap,`
- `uint32_t pogls_geopixel_decode(uint8_t *dst, size_t dst_cap,`
- `uint32_t pogls_geopixel_hilbert_xy_to_d(uint32_t x, uint32_t y, uint32_t order)`
- `void     pogls_geopixel_hilbert_d_to_xy(uint32_t d, uint32_t order,`
- `int      pogls_geopixel_session_init(PoglsGeopixelSession *s, uint32_t blocks)`
- `uint32_t pogls_geopixel_session_feed(PoglsGeopixelSession *s,`
- `void     pogls_geopixel_session_stats(const PoglsGeopixelSession *s)`

## Constants

- `#define POGLS_GEOPIXEL_H`
- `#define POGLS_GEOPIXEL_BLOCK_SIZE  64u`
- `#define POGLS_GEOPIXEL_FLAT      0x00u   /* 2 bytes:  [tag][value]            */`
- `#define POGLS_GEOPIXEL_SMOOTH    0x01u   /* 10 bytes: [tag][mean][res0..res7] */`
- `#define POGLS_GEOPIXEL_GRADIENT  0x02u   /* 10 bytes: [tag][slope][icpt][r0..r6] */`
- `#define POGLS_GEOPIXEL_EDGE      0x03u   /* 65 bytes: [tag][raw 64 bytes]     */`
- `#define POGLS_GEOPIXEL_FLAT_SZ      2u`
- `#define POGLS_GEOPIXEL_SMOOTH_SZ   10u`
- `#define POGLS_GEOPIXEL_GRADIENT_SZ 10u`
- `#define POGLS_GEOPIXEL_EDGE_SZ     65u`
- `#define POGLS_GEOPIXEL_SMOOTH_MAX_DIFF  16`


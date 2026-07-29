# pogls_geopixel.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_geopixel`  
**Path:** `pogls_geopixel/pogls_geopixel.c`  
**Status:** `active`  
**Note:** modified 16d ago  
**Generated:** 2026-07-28 14:03  

## Description

* pogls_geopixel.c — Geopixel Spatial Coherence Compression
* ═══════════════════════════════════════════════════════════════════════
═══════════════════════════════════════════════════════════════════════
INTERNAL HELPERS
═══════════════════════════════════════════════════════════════════════
〉══════════════════════════════════════════════════════════════════════
CLASSIFY 64-byte block → best compression mode
═══════════════════════════════════════════════════════════════════════
═══════════════════════════════════════════════════════════════════════
BLOCK ENCODER
═══════════════════════════════════════════════════════════════════════
═══════════════════════════════════════════════════════════════════════
BLOCK DECODER
═══════════════════════════════════════════════════════════════════════
═══════════════════════════════════════════════════════════════════════
FULL TENSOR ENCODE
═══════════════════════════════════════════════════════════════════════
═══════════════════════════════════════════════════════════════════════
FULL TENSOR DECODE
═══════════════════════════════════════════════════════════════════════

## API Functions

- `static inline uint8_t _clamp_u8(int v)`
- `static uint32_t _classify(const uint8_t *block)`
- `uint32_t pogls_geopixel_encode_block(uint8_t *dst, size_t dst_cap,`
- `uint32_t pogls_geopixel_decode_block(uint8_t *dst, size_t dst_cap,`
- `uint32_t pogls_geopixel_encode(uint8_t *dst, size_t dst_cap,`
- `uint32_t pogls_geopixel_decode(uint8_t *dst, size_t dst_cap,`
- `uint32_t pogls_geopixel_hilbert_xy_to_d(uint32_t x, uint32_t y, uint32_t order)`
- `void pogls_geopixel_hilbert_d_to_xy(uint32_t d, uint32_t order,`
- `int pogls_geopixel_session_init(PoglsGeopixelSession *s, uint32_t blocks)`
- `uint32_t pogls_geopixel_session_feed(PoglsGeopixelSession *s,`
- `void pogls_geopixel_session_stats(const PoglsGeopixelSession *s)`


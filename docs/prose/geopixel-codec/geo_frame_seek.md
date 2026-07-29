# geo_frame_seek.h

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `geopixel`  
**Path:** `geopixel/include/geo_frame_seek.h`  
**Status:** `active`  
**Note:** modified 24d ago; included by 8 file(s)  
**Generated:** 2026-07-29 11:55  

## Description

* geo_frame_seek.h — Deterministic Frame Seek on Fibo 1440 Timeline
* ══════════════════════════════════════════════════════════════════
* 1 frame = 12 edges (9 Hilbert active + 3 Peano on invert/line-12)
* All subsequent frames = same structure × phase iteration
* Everything is deterministic — store only enc (2 bytes)
* Timeline: 1440 positions (fibo cycle)
*   enc(t) = (t × 37) % 1440     — stride-37 walk, full bijection
*   seek(enc) → frame O(1)       — no replay needed
*   next(enc) → (enc + 37) % 1440
* Frame decomposition from enc:
*   Hilbert: group(0..2), edge(0..2), is_skip
*   Peano:   step(0..3) on line-12, sub(0..2) ternary
*   ico_idx: 0..161 icosphere address
* Sacred constants (FROZEN):
*   TRING_WALK_CYCLE  = 1440  (12 × 120)
*   TRING_WALK_STRIDE = 37    (prime, gcd(37,1440)=1)
*   META_FACE_SZ      = 120   (slots per face)
*   FRAME_EDGES       = 12    (9 Hilbert + 3 Peano)
*   ICO_NODES         = 162   (81 × 2 poles)
* No malloc. No float. Stateless O(1).

## Structures

- `typedef struct`
- `typedef struct`
- `typedef struct`

## API Functions

- `static inline DualFrame frame_at(uint16_t enc)`
- `static inline uint16_t frame_enc(uint32_t t)`
- `static inline uint16_t frame_next(uint16_t enc)`
- `static inline uint16_t frame_prev(uint16_t enc)`
- `static inline DualFrame frame_seek(uint32_t t)`
- `return frame_at(frame_enc(t))`
- `static inline uint16_t frame_cpair(uint16_t enc)`
- `static inline int geo_frame_seek_verify(void)`

## Constants

- `#define GEO_FRAME_SEEK_H`
- `#define FRAME_CYCLE       1440u   /* fibo timeline length            */`
- `#define FRAME_STRIDE        37u   /* prime walk, gcd(37,1440)=1      */`
- `#define FRAME_FACE_SZ      120u   /* slots per face (1440/12)        */`
- `#define FRAME_EDGES         12u   /* edges per frame (9H + 3P)       */`
- `#define FRAME_H_ACTIVE       9u   /* Hilbert active edges            */`
- `#define FRAME_P_STEPS        4u   /* Peano steps on line-12          */`
- `#define FRAME_ICO_NODES    162u   /* icosphere L2 (81×2)             */`
- `#define FRAME_PEANO_GRID    81u   /* 3⁴ ternary space                */`


# pogls_hilbert64_encoder.h

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `geopixel`  
**Path:** `geopixel/include/pogls_hilbert64_encoder.h`  
**Status:** `active`  
**Note:** modified 24d ago; included by 10 file(s)  
**Generated:** 2026-07-29 15:24  

## Description

* pogls_hilbert64_encoder.h — POGLS Hilbert-64 RGB Encoder
* ═══════════════════════════════════════════════════════════════════════
*  Input  : ScanEntry stream from pogls_scanner (coord_face 0..11, seed)
*  Output : HilbertPacket64 — 64-slot Hilbert grid with RGB balanced
*  Design:
*    - 12 faces × 4 hilbert paths (3 positive + 1 invert) = 48 slots
*    - remaining 16 slots = invert accumulator (ghost/residual zone)
*    - RGB channel = face % 3 → deterministic, no external state
*    - Hilbert curve maps 64 slots → 8×8 grid (Z-order compatible)
*    - invert = XOR of 3 positive paths per face group (4 faces per channel)
*  RGB balance rule:
*    face 0,3,6,9   → R channel   (4 faces × 3 paths = 12 slots)
*    face 1,4,7,10  → G channel   (4 faces × 3 paths = 12 slots)
*    face 2,5,8,11  → B channel   (4 faces × 3 paths = 12 slots)
*    slot 48..63    → invert accumulator (16 slots, shared RGB ghost)
*    Total = 48 + 16 = 64 ✓
*  Hilbert slot assignment:
*    slot = (face * 4) + path_id   (path_id 0..2 = positive, 3 = invert)
*    invert slot 48..63 = face_group * 4 + invert_phase (0..3)
*  Frozen rules:

## Structures

- `typedef struct`
- `typedef struct`
- `typedef struct`

## API Functions

- `static inline uint8_t h64_channel(uint8_t face)`
- `static inline uint8_t h64_slot(uint8_t face, uint8_t path_id)`
- `static inline uint8_t h64_ghost_slot(uint8_t face, uint8_t phase)`
- `static inline void h64_encoder_init(H64Encoder *enc)`
- `static inline uint8_t h64_feed(H64Encoder   *enc,`
- `static inline uint8_t h64_derive_invert(H64Encoder *enc, uint8_t face)`
- `static inline uint8_t h64_commit_ghost(H64Encoder *enc,`
- `static inline bool h64_reconstruct_path(H64Encoder *enc,`
- `static inline void h64_rgb_balance(const H64Encoder *enc,`
- `static inline void h64_finalize(H64Encoder *enc)`

## Constants

- `#define POGLS_HILBERT64_ENCODER_H`
- `#define H64_SLOTS          64u`
- `#define H64_POSITIVE_PATHS  3u   /* R, G, B per face                  */`
- `#define H64_INVERT_PATH     3u   /* path_id = 3 → invert              */`
- `#define H64_FACE_COUNT     12u   /* dodecahedron faces                 */`
- `#define H64_POSITIVE_SLOTS 48u   /* 12 faces × 4 paths (incl invert)  */`
- `#define H64_GHOST_SLOTS    16u   /* slots 48..63 = residual/ghost zone */`
- `#define H64_FLAG_VALID    0x01u`
- `#define H64_FLAG_INVERT   0x02u   /* this cell is derived (invert)     */`
- `#define H64_FLAG_GHOST    0x04u   /* residual zone cell                */`
- `#define H64_FLAG_PASSTHRU 0x08u   /* pre-compressed, skip tile codec   */`


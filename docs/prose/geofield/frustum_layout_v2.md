# frustum_layout_v2.h

> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.

**Module:** `collection`  
**Path:** `collection/geopixel/geofield/frustum_layout_v2.h`  
**Status:** `stale`  
**Note:** last modified 65d ago  
**Generated:** 2026-07-29 12:21  

## Description

*
* fgls_block_layout.h
* FrustumBlock = 4896B container = 1 complete frustum unit
* Layout (frozen):
*   [  17B ] header   — security DNA (17 = prime, not in 2^n×3^m)
*   [3456B ] data     — 54 × 64B DiamondBlock (FIXED, no touch)
*   [1440B ] meta     — structured, geometry-aligned
*   ──────────────────
*   [4896B ] total    = 17 × 2 × 144
letter_map: 144B — 1 byte per clock tick
* maps tick → letter/symbol index in LC space
* offset in meta: 0
slope_map: 288B — 2 bytes per clock tick
* maps tick → frustum slope (uint16, fixed-point Q8.8)
* offset in meta: 144
drain_bitmap: 16B — 12 drains + control flags
* bit layout per drain (1 byte each):
*   bit0   = active drain (1=open, 0=closed)
*   bit1   = flush_pending
*   bit2   = merkle_dirty (root needs recompute)

## Structures

- `typedef struct __attribute__((packed))`
- `typedef struct __attribute__((packed))`
- `typedef struct __attribute__((packed))`

## API Functions

- `static inline FrustumHeader* frustum_header(FrustumBlock *b)`

## Constants

- `#define FGLS_BLOCK_LAYOUT_H`
- `#define FGLS_DIAMOND_COUNT    54        /* 2×3³ = Rubik stickers       */`
- `#define FGLS_DIAMOND_BYTES    64        /* 64B per DiamondBlock         */`
- `#define FGLS_DATA_BYTES       3456      /* 54×64                        */`
- `#define FGLS_CLOCK_TICKS      144       /* FiboClock cycle              */`
- `#define FGLS_DRAIN_COUNT      12        /* pentagon drains (Goldberg)   */`
- `#define FGLS_SHADOW_COUNT     28        /* outer boundary (4×7)         */`
- `#define FGLS_MERKLE_BYTES     32        /* SHA-256 per drain root       */`
- `#define FGLS_META_BYTES       1440      /* 10×144 = gear family         */`
- `#define FGLS_TOTAL_BYTES      4896      /* 17×2×144 = file boundary     */`


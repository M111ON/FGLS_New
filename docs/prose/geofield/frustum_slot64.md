# frustum_slot64.h

> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.

**Module:** `collection`  
**Path:** `collection/geopixel/geofield/frustum_slot64.h`  
**Status:** `stale`  
**Note:** last modified 65d ago  
**Generated:** 2026-07-29 14:05  

## Description

* frustum_slot64.h — DiamondBlock 64B Storage
* ════════════════════════════════════════════════════════════════
* 54 slots × 64B = 3456B = GEO_FULL_N  (self-similar seam ✓)
* Each slot = one DiamondBlock = 64B = 2⁶ bytes
* Slot layout (64B exact):
*   core[4]        16B  — merkle roots per level (0..3)
*   reserved_mask   2B  — coset silence bitmap (9 bits, one per coset)
*   write_count     2B  — writes into this slot (overflow wraps)
*   slope_lo        4B  — last slope fingerprint low 32 bits
*   _pad           40B  — reserved, zero
* Block index:
*   block_index  = addr / DIAMOND_BLOCK   (0..53 for Lv1)
*   block_offset = addr % DIAMOND_BLOCK   (0..63)
*   54 = GEAR_MESH = 2×3³ — aligns with both trit and face decomposition
* Write rule:
*   slot = store[trit.coset * 6 + trit.face]  ← coset×face addressing
*   if trit_coset_silent(slot.reserved_mask, trit.coset) → drop silently
*   else slot.core[trit.level] = merkle_root  (latest wins)
* No malloc. No float. No heap.
* Depends: frustum_trit.h

## Structures

- `typedef struct`
- `typedef struct`
- `typedef struct`

## API Functions

- `static inline void frustum_store_init(FrustumStore *fs)`
- `static inline uint8_t frustum_slot_idx(const TritAddr *t)`
- `static inline int frustum_write(FrustumStore    *fs,`
- `static inline void frustum_silence_coset(FrustumStore *fs,`
- `static inline uint32_t frustum_read(const FrustumStore *fs,`
- `static inline FrustumStats frustum_stats(const FrustumStore *fs)`

## Constants

- `#define FRUSTUM_SLOT64_H`
- `#define DIAMOND_BLOCK    64u   /* 2⁶ bytes per slot              */`
- `#define GEAR_MESH        54u   /* 2×3³ slots, tiles Lv1 exactly  */`
- `#define FRUSTUM_DATA_SZ  3456u /* GEAR_MESH × DIAMOND_BLOCK      */`


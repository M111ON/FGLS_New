# frustum_gcfs.h

> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.

**Module:** `collection`  
**Path:** `collection/geopixel/geofield/frustum_gcfs.h`  
**Status:** `stale`  
**Note:** last modified 65d ago  
**Generated:** 2026-07-29 10:37  

## Description

* frustum_gcfs.h — Serialize FrustumStore to 4896B GCFS File
* ════════════════════════════════════════════════════════════════
* GCFS = GiantCube FrustumStore — output format for frustum pipeline.
* File layout (4896B total = 288×17):
*   [0    ..3455]  data zone  — 54 × 64B DiamondBlocks (verbatim copy)
*   [3456 ..3464]  coset_mask — 9B (one byte per coset, reserved_mask[0..8])
*   [3465 ..3490]  letter_map — 26B (A..Z, caller-supplied)
*   [3491 ..3498]  slope      — 8B  (uint64_t last slope fingerprint)
*   [3499 ..3502]  merkle_root— 4B  (XOR of all slot core[0..3])
*   [3503 ..4895]  _pad       — 1393B zeros (boundary reserve)
* 4896 = 2⁵×3²×17  — factor 17 ∈ FACE_PRIME {7,11,13,17,19,23}
*   → file boundary unreachable by pure 2ⁿ×3ᵐ arithmetic (security seam)
*   → metadata zone = 1440B = 2⁵×3²×5 (has factor 5 = intentional marker)
* gcfs_serialize: FrustumStore → out[4896]
* gcfs_deserialize: in[4896]  → FrustumStore
* No malloc. No float. No heap.
* Depends: frustum_slot64.h → frustum_trit.h
* ════════════════════════════════════════════════════════════════
* Collapse 54 slots into 9 coset bytes:
*   coset_out[c] = OR of all slots' reserved_mask bits for coset c

## API Functions

- `static inline uint32_t _gcfs_merkle(const FrustumStore *fs)`
- `static inline void _gcfs_coset_summary(const FrustumStore *fs,`
- `static inline uint32_t gcfs_serialize(const FrustumStore *fs,`
- `static inline int gcfs_deserialize(FrustumStore *fs,`
- `static inline int gcfs_merkle_verify(const FrustumStore *fs,`

## Constants

- `#define FRUSTUM_GCFS_H`
- `#define GCFS_FILE_SIZE     4896u   /* 2⁵×3²×17                    */`
- `#define GCFS_DATA_OFFSET      0u   /* DiamondBlock data zone start  */`
- `#define GCFS_DATA_SIZE     3456u   /* 54×64 = FRUSTUM_DATA_SZ       */`
- `#define GCFS_META_OFFSET   3456u   /* metadata zone start           */`
- `#define GCFS_META_SIZE     1440u   /* 2⁵×3²×5 — has factor 5        */`
- `#define GCFS_COSET_OFFSET  3456u   /*  9B coset_mask                */`
- `#define GCFS_LETTER_OFFSET 3465u   /* 26B letter_map                */`
- `#define GCFS_SLOPE_OFFSET  3491u   /*  8B slope fingerprint         */`
- `#define GCFS_MERKLE_OFFSET 3499u   /*  4B merkle summary            */`
- `#define GCFS_PAD_OFFSET    3503u   /* 1393B reserved zeros          */`
- `#define GCFS_PRIME_MARKER    17u   /* FACE_PRIME boundary           */`


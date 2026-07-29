# fabric_wire_drain.h

> 🟡 **[STALE]** — This file may be inactive or pending removal. Check before relying on it.

**Module:** `collection`  
**Path:** `collection/geopixel/geofield/fabric_wire_drain.h`  
**Status:** `stale`  
**Note:** last modified 65d ago  
**Generated:** 2026-07-29 07:10  

## Description

* fabric_wire_drain.h — Task #1 complete
* Wire drain_gap = (trit^slope)&1  →  FrustumBlock.meta.drain_state[]
* ═══════════════════════════════════════════════════════════════════════
*  Depends on:
*    fabric_wire.h          (FabricWireState, fabric_wire_state, fence)
*    fgls_block_layout.h    (FrustumBlock, FrustumMeta offsets)
*  drain_state[id] bit layout (from fgls_block_layout.h):
*    bit0 = active         (1 = drain open)
*    bit1 = flush_pending
*    bit2 = merkle_dirty
*    bit3 = confirmed_delete  (tombstone committed)
*    bit4-7 = reserved
*  shadow_state[id] bit layout:
*    bit0 = occupied
*    bit1 = heptagon_fence  (once set: read→flush only, irreversible)
*    bit2-7 = reserved
*  Switch Gate → drain mapping:
*    bit6 = 0 → STORE  → drain stays closed (bit0=0)
*    bit6 = 1 → DRAIN  → drain opens        (bit0=1), flush_pending set
*  Drain ID selection:

## API Functions

- `static inline WireResult fabric_wire_commit(FrustumBlock *block,`
- `static inline bool fabric_drain_flush(FrustumBlock *block, uint8_t drain_id)`
- `static inline uint8_t fabric_shadow_flush_pending(const FrustumBlock *block)`
- `static inline bool fabric_atomic_reshape_check(const FrustumBlock *block)`
- `return pogls_reshape_ready(fabric_shadow_flush_pending(block))`
- `static inline uint8_t fabric_rotation_advance(FrustumBlock *block)`

## Constants

- `#define FABRIC_WIRE_DRAIN_H`
- `#define DRAIN_BIT_ACTIVE    (1u << 0)   /* drain open                  */`
- `#define DRAIN_BIT_FLUSH     (1u << 1)   /* flush_pending               */`
- `#define DRAIN_BIT_MERKLE    (1u << 2)   /* merkle root needs recompute */`
- `#define DRAIN_BIT_TOMBSTONE (1u << 3)   /* confirmed delete committed  */`
- `#define SHADOW_BIT_OCCUPIED (1u << 0)   /* data written, not flushed   */`
- `#define SHADOW_BIT_FENCE    (1u << 1)   /* heptagon fence locked       */`


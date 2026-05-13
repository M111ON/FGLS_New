/*
 * heptagon_fence.h — Task #5: Full Heptagon Fence Path
 * ═════════════════════════════════════════════════════════════════
 *
 * Extends fabric_wire.h fence with rotation+phase awareness.
 *
 * Three fence paths (all three must pass independently):
 *
 *   PATH A — CORE→SHADOW WRITE
 *     src_layer ∈ {CORE, GEAR}  → permitted
 *     src_layer ∉ {CORE, GEAR}  → FENCE_DENY
 *     fence_key(rot, phase) == 0 (neutral) → always permit if layer ok
 *     fence_key != 0 AND phase=1 → require fence_key matches slot's key
 *
 *   PATH B — SHADOW→FLUSH READ
 *     dst_layer == SECURITY → permitted (flush process only)
 *     dst_layer != SECURITY → FENCE_DENY
 *     is_drain must be true (valve only opens on drain)
 *
 *   PATH C — DENY (shadow→core direct read)
 *     src_layer == SHADOW AND !is_drain → always FENCE_DENY
 *     No exceptions. Architecturally absent path.
 *
 * Rotation gating (Task #4 hook):
 *   fence_key = (rot ^ phase) & 0x3F
 *   fence_key == 0 → neutral, no extra constraint
 *   fence_key != 0 → slot must carry matching fence_key to unlock
 *   This creates 6 × 2 = 12 distinct gate states (rot × phase)
 *   matching the 12 pentagon drains (POGLS_TRING_FACES = 12) ← geometry ✓
 *
 * Shadow slot fence state (per-slot, stored in shadow_state[]):
 *   SHADOW_BIT_FENCE (bit1) — one-way valve: once set, irreversible
 *   SHADOW_BIT_KEY   (bit2..7) — fence_key stored at lock time (6 bits)
 *   Reading key: (shadow_state[id] >> 2) & 0x3F
 *
 * Irreversibility guarantee:
 *   Once SHADOW_BIT_FENCE is set, the slot can only drain → flush.
 *   No path exists to clear it except Atomic Reshape (Task #6).
 *
 * Sacred: POGLS_LANE_GROUPS=6, POGLS_TRING_FACES=12 — FROZEN
 * No malloc. No float. No heap.
 * ═════════════════════════════════════════════════════════════════
 */

#ifndef HEPTAGON_FENCE_H
#define HEPTAGON_FENCE_H

#include <stdint.h>
#include <stdbool.h>
#include "fabric_wire.h"       /* PoglsLayer, pogls_fence_check(), etc. */
#include "fabric_wire_drain.h" /* FrustumBlock, SHADOW_BIT_FENCE, WireResult */
#include "pogls_rotation.h"    /* fabric_rotation_state(), pogls_fence_key() */

/* ══════════════════════════════════════════════════════════════
   SHADOW SLOT KEY — extended shadow_state bit layout
   Original (Task #1):
     bit0 = SHADOW_BIT_OCCUPIED
     bit1 = SHADOW_BIT_FENCE
     bit2-7 = reserved
   Task #5 extension (additive, no breakage):
     bit2..7 = fence_key stored at lock time (6 bits, 0..63)
   ══════════════════════════════════════════════════════════════ */

#define SHADOW_BIT_KEY_SHIFT  2u
#define SHADOW_BIT_KEY_MASK   0xFCu   /* bits 2..7 = 0b11111100 */

/* store fence_key in shadow_state[id] bits 2..7 */
static inline void _fence_key_store(uint8_t *shadow_byte, uint8_t fence_key)
{
    *shadow_byte = (uint8_t)((*shadow_byte & ~SHADOW_BIT_KEY_MASK)
                             | ((fence_key & 0x3Fu) << SHADOW_BIT_KEY_SHIFT));
}

/* read fence_key from shadow_state[id] bits 2..7 */
static inline uint8_t _fence_key_read(uint8_t shadow_byte)
{
    return (uint8_t)((shadow_byte >> SHADOW_BIT_KEY_SHIFT) & 0x3Fu);
}

/* ══════════════════════════════════════════════════════════════
   FENCE RESULT (extended from WireResult)
   ══════════════════════════════════════════════════════════════ */

typedef enum {
    FENCE_OK           =  0,   /* transaction permitted                    */
    FENCE_DENY_LAYER   = -1,   /* wrong src/dst layer                      */
    FENCE_DENY_LOCKED  = -2,   /* shadow slot already fence-locked         */
    FENCE_DENY_KEY     = -3,   /* fence_key mismatch (rot+phase gate)      */
    FENCE_DENY_NODRN   = -4,   /* READ path requires is_drain=true         */
    FENCE_DENY_DIRECT  = -5,   /* shadow→core direct read (arch. absent)   */
} FenceResult;

/* ══════════════════════════════════════════════════════════════
   PATH A — CORE→SHADOW WRITE
   ══════════════════════════════════════════════════════════════
 *
 * heptagon_fence_write(block, shadow_id, src_layer, rot, phase)
 *
 * Checks:
 *   1. Layer gate: src ∈ {CORE, GEAR}
 *   2. Irreversibility: slot not already fence-locked
 *   3. Key gate: if fence_key != 0 → slot must be unlocked (first write wins)
 *
 * On FENCE_OK: marks slot OCCUPIED, stores fence_key in bits 2..7.
 *              Does NOT set SHADOW_BIT_FENCE (that's PATH B / drain).
 */
static inline FenceResult heptagon_fence_write(FrustumBlock *block,
                                                uint8_t       shadow_id,
                                                PoglsLayer    src_layer,
                                                uint8_t       rot,
                                                uint8_t       phase)
{
    /* Gate 1: layer check */
    if (!pogls_fence_write_ok(src_layer))
        return FENCE_DENY_LAYER;

    FrustumMeta *m  = &block->meta;
    uint8_t     *sb = &m->shadow_state[shadow_id % FGLS_SHADOW_COUNT];

    /* Gate 2: already fence-locked → DENY (one-way valve) */
    if (*sb & SHADOW_BIT_FENCE)
        return FENCE_DENY_LOCKED;

    /* Gate 3: rotation key check
     * fence_key=0 (neutral, rot=0 phase=0) → skip key gate
     * fence_key≠0 → slot must not already hold a different key */
    uint8_t fk = pogls_fence_key(rot, phase);
    if (fk != 0u) {
        uint8_t stored_key = _fence_key_read(*sb);
        /* if slot already occupied with a different key → deny */
        if ((*sb & SHADOW_BIT_OCCUPIED) && stored_key != 0u && stored_key != fk)
            return FENCE_DENY_KEY;
    }

    /* PERMIT: mark occupied, store key */
    *sb |= SHADOW_BIT_OCCUPIED;
    _fence_key_store(sb, fk);

    return FENCE_OK;
}

/* ══════════════════════════════════════════════════════════════
   PATH B — SHADOW→FLUSH READ (drain path)
   ══════════════════════════════════════════════════════════════
 *
 * heptagon_fence_drain(block, shadow_id, dst_layer, is_drain, rot, phase)
 *
 * Checks:
 *   1. is_drain must be true (valve only opens on drain signal)
 *   2. dst_layer must be SECURITY (flush process)
 *   3. Slot must be occupied (something to drain)
 *   4. Key gate: if fence_key≠0, must match stored key
 *
 * On FENCE_OK: sets SHADOW_BIT_FENCE (locks valve, irreversible).
 *              Caller must then call fabric_drain_flush().
 */
static inline FenceResult heptagon_fence_drain(FrustumBlock *block,
                                                uint8_t       shadow_id,
                                                PoglsLayer    dst_layer,
                                                bool          is_drain,
                                                uint8_t       rot,
                                                uint8_t       phase)
{
    /* Gate 1: valve only opens on drain signal */
    if (!is_drain)
        return FENCE_DENY_NODRN;

    /* Gate 2: only flush process (SECURITY layer) reads shadow */
    if (!pogls_fence_read_ok(dst_layer))
        return FENCE_DENY_LAYER;

    FrustumMeta *m  = &block->meta;
    uint8_t     *sb = &m->shadow_state[shadow_id % FGLS_SHADOW_COUNT];

    /* Gate 3: must have something to drain */
    if (!(*sb & SHADOW_BIT_OCCUPIED))
        return FENCE_DENY_LOCKED;   /* nothing here to drain */

    /* Gate 4: key match */
    uint8_t fk = pogls_fence_key(rot, phase);
    if (fk != 0u) {
        uint8_t stored_key = _fence_key_read(*sb);
        if (stored_key != 0u && stored_key != fk)
            return FENCE_DENY_KEY;
    }

    /* PERMIT: lock fence (irreversible) */
    *sb |= SHADOW_BIT_FENCE;

    return FENCE_OK;
}

/* ══════════════════════════════════════════════════════════════
   PATH C — DENY (shadow→core direct read)
   ══════════════════════════════════════════════════════════════
 * Architecturally absent — always FENCE_DENY_DIRECT.
 * Provided as explicit API so callers have a named function to call.
 */
static inline FenceResult heptagon_fence_direct_read(void)
{
    return FENCE_DENY_DIRECT;   /* no exceptions, no conditions */
}

/* ══════════════════════════════════════════════════════════════
   COMBINED GATE — full transaction check
   Wraps all three paths into one call.
   ══════════════════════════════════════════════════════════════ */

typedef enum {
    HFENCE_PATH_WRITE = 0,   /* PATH A: core→shadow */
    HFENCE_PATH_DRAIN = 1,   /* PATH B: shadow→flush */
    HFENCE_PATH_READ  = 2,   /* PATH C: shadow→core (always deny) */
} HFencePath;

static inline FenceResult heptagon_fence_check(FrustumBlock *block,
                                                uint8_t       shadow_id,
                                                PoglsLayer    src_layer,
                                                PoglsLayer    dst_layer,
                                                HFencePath    path,
                                                bool          is_drain,
                                                uint8_t       rot,
                                                uint8_t       phase)
{
    switch (path) {
        case HFENCE_PATH_WRITE:
            return heptagon_fence_write(block, shadow_id,
                                        src_layer, rot, phase);
        case HFENCE_PATH_DRAIN:
            return heptagon_fence_drain(block, shadow_id,
                                        dst_layer, is_drain, rot, phase);
        case HFENCE_PATH_READ:
        default:
            return heptagon_fence_direct_read();
    }
    (void)src_layer; (void)dst_layer;  /* suppress unused for PATH_READ */
}

/* ══════════════════════════════════════════════════════════════
   INSPECT HELPERS
   ══════════════════════════════════════════════════════════════ */

/* is slot fence-locked? (irreversible) */
static inline bool heptagon_slot_locked(const FrustumBlock *block,
                                         uint8_t shadow_id)
{
    return (bool)(block->meta.shadow_state[shadow_id % FGLS_SHADOW_COUNT]
                  & SHADOW_BIT_FENCE);
}

/* is slot occupied (but maybe not yet locked)? */
static inline bool heptagon_slot_occupied(const FrustumBlock *block,
                                           uint8_t shadow_id)
{
    return (bool)(block->meta.shadow_state[shadow_id % FGLS_SHADOW_COUNT]
                  & SHADOW_BIT_OCCUPIED);
}

/* read fence_key stored in slot */
static inline uint8_t heptagon_slot_key(const FrustumBlock *block,
                                         uint8_t shadow_id)
{
    return _fence_key_read(
        block->meta.shadow_state[shadow_id % FGLS_SHADOW_COUNT]);
}

/* count locked slots (for reshape threshold check) */
static inline uint8_t heptagon_locked_count(const FrustumBlock *block)
{
    uint8_t n = 0u;
    for (uint8_t i = 0u; i < FGLS_SHADOW_COUNT; i++)
        if (block->meta.shadow_state[i] & SHADOW_BIT_FENCE) n++;
    return n;
}

/* ══════════════════════════════════════════════════════════════
   RESET (Atomic Reshape clears fence — Task #6 hook)
   Only called after successful Atomic Reshape sequence.
   Clears FENCE + KEY bits, preserves OCCUPIED until drain confirms.
   ══════════════════════════════════════════════════════════════ */
static inline void heptagon_fence_reshape_clear(FrustumBlock *block)
{
    for (uint8_t i = 0u; i < FGLS_SHADOW_COUNT; i++) {
        /* clear fence + key bits, keep occupied for drain tracking */
        block->meta.shadow_state[i] &= SHADOW_BIT_OCCUPIED;
    }
}

#endif /* HEPTAGON_FENCE_H */

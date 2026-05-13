/*
 * pogls_rotation.h — Task #4: Global Rotation State + 1440 Phase Wire
 * ════════════════════════════════════════════════════════════════════
 *
 * Design:
 *   rotation_state = global phase driver (0..5, wraps at 6 = POGLS_LANE_GROUPS)
 *   Advance trigger: phase boundary (idx720==0 AND phase==1)
 *   → "once per 1440 cycle" — stable, predictable, geometry-aligned
 *
 *   bit6 formula (Task #4 extension):
 *     phase=0: bit6 = (trit ^ slope)       & 1   ← base (Task #3 compat)
 *     phase=1: bit6 = (trit ^ slope ^ 1 ^ rot) & 1  ← rotation active
 *
 *   Rationale:
 *     720  = spatial  (one tring cycle)
 *     1440 = temporal (two-phase cycle)
 *     rot  = bridge   (global lane group driver, 0..5)
 *     Task #5 hook: heptagon fence will gate on (rot + phase)
 *
 * Non-breaking:
 *   phase=0 + rot=0  →  bit6 identical to Task #3 output  ✓
 *   pogls_dispatch_1440(idx<720) untouched                 ✓
 *
 * Sacred: POGLS_BASE_CYCLE=720, POGLS_EXT_CYCLE=1440, POGLS_LANE_GROUPS=6
 *         All FROZEN.
 * ════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_ROTATION_H
#define POGLS_ROTATION_H

#include <stdint.h>
#include "pogls_1440.h"   /* Pogls1440Slot, POGLS_BASE_CYCLE, POGLS_EXT_CYCLE */
#include "fabric_wire.h"  /* POGLS_LANE_GROUPS=6, pogls_lane_group() */

/* ══════════════════════════════════════════════════════════════
   GLOBAL ROTATION STATE
   Single global — one rotation_state per system instance.
   Not thread-safe by design (POGLS is single-threaded pipeline).
   ══════════════════════════════════════════════════════════════ */

static uint8_t _pogls_rotation_state = 0u;

/* fabric_rotation_state() — read current rotation (0..5) */
static inline uint8_t fabric_rotation_state(void)
{
    return _pogls_rotation_state;
}

/*
 * fabric_rotation_advance_global() — advance and wrap at 6
 * Returns new rotation_state (0..5).
 * Named _global to distinguish from FrustumBlock-local version in fabric_wire_drain.h
 */
static inline uint8_t fabric_rotation_advance_global(void)
{
    _pogls_rotation_state = (uint8_t)((_pogls_rotation_state + 1u)
                                      % (uint8_t)POGLS_LANE_GROUPS);
    return _pogls_rotation_state;
}

/* fabric_rotation_reset() — for test harness only */
static inline void fabric_rotation_reset(void)
{
    _pogls_rotation_state = 0u;
}

/* ══════════════════════════════════════════════════════════════
   EXTENDED 1440 SLOT (Task #4)
   Adds rot field to Pogls1440Slot — backward-compatible extension.
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t idx720;   /* 0..719 — base cycle index                  */
    uint8_t  phase;    /* 0 or 1 — which layer                       */
    uint8_t  bit6;     /* Switch Gate = (trit^slope^phase^rot_xor)&1 */
    uint8_t  rot;      /* rotation_state at time of dispatch  0..5   */
    uint8_t  lane_group; /* pogls_lane_group(rot) = rot%6            */
    uint8_t  _pad[2];
} Pogls1440SlotR;      /* R = rotation-aware variant                 */

/* ══════════════════════════════════════════════════════════════
   PHASE-AWARE DISPATCH WITH ROTATION
   ══════════════════════════════════════════════════════════════
 *
 * bit6 formula:
 *   phase=0: (trit ^ slope ^ 0   ^ 0  ) & 1  — base, rot inactive
 *   phase=1: (trit ^ slope ^ 1   ^ rot) & 1  — temporal, rot active
 *
 * Why rot only in phase=1:
 *   phase=0 is spatial ground truth — must stay stable (720 invariant)
 *   phase=1 is temporal interpretation — rotation drives lane group
 *   Separation: spatial ⊥ temporal ✓
 */
static inline uint8_t tgw_dispatch_rot(uint16_t idx720,
                                        uint8_t  phase,
                                        uint8_t  rot)
{
    uint8_t trit  = _p1440_trit(idx720);
    uint8_t slope = _p1440_slope(idx720);
    /* rot_xor: rot contributes only in phase=1 */
    uint8_t rot_xor = (uint8_t)(phase & rot & 1u);  /* phase×rot — one bit */
    return (uint8_t)((trit ^ slope ^ phase ^ rot_xor) & 1u);
}

/* ══════════════════════════════════════════════════════════════
   CORE: pogls_dispatch_1440_rot
   Drop-in replacement for pogls_dispatch_1440().
   Advances rotation_state at phase boundary (phase=1, idx720=0).
   ══════════════════════════════════════════════════════════════ */

/*
 * pogls_dispatch_1440_rot(idx)
 *
 * Phase boundary rule:
 *   idx == POGLS_BASE_CYCLE (720) → first slot of phase 1 → advance rotation
 *   All other slots: rotation unchanged during dispatch
 *
 * Non-breaking:
 *   idx < 720: identical to pogls_dispatch_1440() when rot=0 on first cycle
 *   bit6 formula with phase=0 XOR rot_xor=0 → (trit^slope)&1 ✓
 */
static inline Pogls1440SlotR pogls_dispatch_1440_rot(uint16_t idx)
{
    Pogls1440SlotR s;
    s._pad[0] = s._pad[1] = 0u;

    s.idx720 = (uint16_t)(idx % POGLS_BASE_CYCLE);
    s.phase  = (uint8_t)(idx / POGLS_BASE_CYCLE);   /* 0 or 1 */

    /* ── ROTATION ADVANCE at phase boundary ──────────────────
     * Fires exactly once per 1440 cycle: when phase flips 0→1.
     * Condition: idx == POGLS_BASE_CYCLE (idx720=0 AND phase=1)
     * Use raw idx comparison — avoids double-advance on idx720=0 in phase=0
     */
    if (idx == POGLS_BASE_CYCLE) {
        fabric_rotation_advance_global();
    }

    s.rot        = fabric_rotation_state();
    s.lane_group = pogls_lane_group(s.rot);   /* rot % 6 */
    s.bit6       = tgw_dispatch_rot(s.idx720, s.phase, s.rot);

    return s;
}

/* ══════════════════════════════════════════════════════════════
   MULTI-CYCLE DRIVER
   Run N complete 1440 cycles, returns final rotation_state.
   Useful for sequence testing and atomic reshape simulation.
   ══════════════════════════════════════════════════════════════ */
static inline uint8_t pogls_run_cycles(uint32_t n_cycles)
{
    for (uint32_t c = 0u; c < n_cycles; c++) {
        for (uint16_t i = 0u; i < POGLS_EXT_CYCLE; i++) {
            pogls_dispatch_1440_rot(i);
        }
    }
    return fabric_rotation_state();
}

/* ══════════════════════════════════════════════════════════════
   ROTATION → LANE QUERIES
   ══════════════════════════════════════════════════════════════ */

/* lane_group from current rotation (0..5) */
static inline uint8_t pogls_current_lane_group(void)
{
    return pogls_lane_group(_pogls_rotation_state);
}

/* absolute lane [0..53] from current rotation + local offset */
static inline uint8_t pogls_current_lane(uint8_t local_idx)
{
    return pogls_lane_index(_pogls_rotation_state, local_idx);
}

/* slots-per-group: 720 / 6 = 120 spatial, 1440 / 6 = 240 temporal */
#define POGLS_SLOTS_PER_ROT_SPATIAL   120u   /* 720/6  */
#define POGLS_SLOTS_PER_ROT_TEMPORAL  240u   /* 1440/6 */

/*
 * Task #5 hook point (heptagon fence):
 *   fence_key = rot * 2 + phase   → 0..11
 *   12 distinct values = 12 pentagon drains (POGLS_TRING_FACES)
 *   geometry: each (rot, phase) pair owns exactly one drain gate
 *
 *   fence_key == 0 (rot=0, phase=0) → neutral state, no gating
 */
static inline uint8_t pogls_fence_key(uint8_t rot, uint8_t phase)
{
    return (uint8_t)((rot % POGLS_LANE_GROUPS) * 2u + (phase & 1u));
}

#endif /* POGLS_ROTATION_H */

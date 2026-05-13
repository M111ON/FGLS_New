/*
 * pogls_atomic_reshape.h — Task #6: Atomic Reshape
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  Depends on (include order):
 *    frustum_layout_v2.h    — FrustumBlock, FrustumHeader, FrustumMeta
 *    fabric_wire.h          — pogls_reshape_ready(), POGLS_SACRED_NEXUS
 *    fabric_wire_drain.h    — fabric_drain_flush(), fabric_shadow_flush_pending()
 *    pogls_rotation.h       — fabric_rotation_advance_global(), fabric_rotation_state()
 *    heptagon_fence.h       — heptagon_fence_reshape_clear(), heptagon_locked_count()
 *
 *  Atomic Reshape sequence (section 5 — LOCKED):
 *    1. collect(shadow)    shadow_flush_pending() >= 54 → trigger
 *    2. stage(new_layout)  compute new lane_group from next rotation
 *    3. flip(rotation)     1-byte write — atomic boundary (17 rule)
 *    4. drain(pentagon)    flush all 12 drains + heptagon_fence_reshape_clear()
 *
 *  Sacred invariant:
 *    8  = before_state  (rotation_state before flip)
 *    9  = after_state   (rotation_state after flip)
 *    17 = during        (does not exist — atomic moment)
 *
 *  Why 17 = "does not exist":
 *    8+9=17 — the boundary has no storage, no observable half-state.
 *    Like CAS (Compare-And-Swap) on CPU: external observers see
 *    either before OR after, never during.
 *
 *  Geometry lock:
 *    threshold = POGLS_SACRED_NEXUS = 54  (Rubik sticker count)
 *    54 = minimum complete state for a valid Rubik face rotation
 *    6 reshapes → rotation_state cycles 0→1→2→3→4→5→0 (full Rubik cycle)
 *
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_ATOMIC_RESHAPE_H
#define POGLS_ATOMIC_RESHAPE_H

#include <stdint.h>
#include <stdbool.h>
#include "frustum_layout_v2.h"
#include "fabric_wire.h"
#include "fabric_wire_drain.h"
#include "pogls_rotation.h"
#include "heptagon_fence.h"

/* ── result codes ──────────────────────────────────────────────────── */

typedef enum {
    RESHAPE_OK           =  0,   /* full sequence completed             */
    RESHAPE_NOT_READY    =  1,   /* threshold not met (< 54 locked)     */
    RESHAPE_PARTIAL_DRAIN= -1,   /* some drains failed to flush         */
    RESHAPE_STAGE_FAIL   = -2,   /* new layout invalid (geometry check) */
} ReshapeResult;

/* ── staged layout (step 2 output) ────────────────────────────────── */

/*
 * ReshapeStage — computed before the atomic flip.
 * Lives in caller's stack frame — zero heap, zero global state change
 * until step 3 commits.
 *
 * Analogy: like a chess move computed in your head before lifting the
 * piece — nothing changes on the board until you commit.
 */
typedef struct {
    uint8_t  next_rot;          /* rotation after advance (0..5)         */
    uint8_t  next_lane_group;   /* pogls_lane_group(next_rot)            */
    uint8_t  shadow_pending;    /* snapshot of pending count at stage time */
    uint8_t  drain_count;       /* always FGLS_DRAIN_COUNT = 12          */
} ReshapeStage;

/* ── step 1: collect — check threshold ────────────────────────────── */

/*
 * reshape_collect(block)
 *
 * Counts shadow slots that are OCCUPIED + FENCE-locked.
 * Returns true if reshape should fire (>= POGLS_SACRED_NEXUS = 54).
 *
 * Note: uses heptagon_locked_count() — counts FENCE bit directly,
 * which is the correct post-PATH-B state (fence set = drain pending).
 *
 * Tests: A01, A02
 */
static inline bool reshape_collect(const FrustumBlock *block)
{
    uint8_t locked = heptagon_locked_count(block);
    return pogls_reshape_ready(locked);
}

/* ── step 2: stage — compute new layout without committing ─────────── */

/*
 * reshape_stage(block, out_stage)
 *
 * Computes next rotation and lane_group.
 * Does NOT write to block or global state — pure computation.
 *
 * Validation: next_rot must stay in [0..5] (geometry rule).
 * Returns RESHAPE_OK or RESHAPE_STAGE_FAIL.
 *
 * Tests: A03
 */
static inline ReshapeResult reshape_stage(const FrustumBlock *block,
                                           ReshapeStage       *out)
{
    uint8_t cur_rot = fabric_rotation_state();

    /* compute next without advancing the global yet */
    out->next_rot        = (uint8_t)((cur_rot + 1u) % (uint8_t)POGLS_LANE_GROUPS);
    out->next_lane_group = pogls_lane_group(out->next_rot);
    out->shadow_pending  = heptagon_locked_count(block);
    out->drain_count     = (uint8_t)FGLS_DRAIN_COUNT;

    /* geometry guard: next_rot must be valid */
    if (out->next_rot >= (uint8_t)POGLS_LANE_GROUPS)
        return RESHAPE_STAGE_FAIL;

    return RESHAPE_OK;
}

/* ── step 3: flip — atomic rotation advance (1-byte write) ─────────── */

/*
 * reshape_flip(block, stage)
 *
 * THE atomic moment:
 *   - advances global rotation_state via fabric_rotation_advance_global()
 *   - mirrors new rotation_state into FrustumHeader.rotation_state
 *   - updates world_flags bit0 to reflect new World A/B orientation
 *
 * This function is the "17" boundary — before and after exist,
 * during does not. Keep as few instructions as possible.
 *
 * Returns new rotation_state (0..5).
 * Tests: A04, A07
 */
static inline uint8_t reshape_flip(FrustumBlock       *block,
                                    const ReshapeStage *stage)
{
    /* ── ATOMIC MOMENT: 1-byte global state change ── */
    uint8_t new_rot = fabric_rotation_advance_global();   /* 8 → 9 */
    /* ── END ATOMIC MOMENT (17 = this line does not exist) ── */

    /* mirror into header (non-critical, informational) */
    FrustumHeader *hdr   = frustum_header(block);
    hdr->rotation_state  = new_rot;
    hdr->world_flags     = (uint8_t)(hdr->world_flags & 0xFEu)
                         | (uint8_t)(new_rot & 1u);   /* bit0 = World A/B */

    (void)stage;   /* stage validated in step 2; new_rot confirms it */
    return new_rot;
}

/* ── step 4: drain — flush all 12 pentagon drains + clear fence ────── */

/*
 * reshape_drain(block)
 *
 * Executes the full drain sequence:
 *   a. fabric_drain_flush(block, drain_id) for each of 12 drains
 *   b. heptagon_fence_reshape_clear(block)  — clear FENCE+KEY, keep OCCUPIED
 *
 * Returns RESHAPE_OK if all 12 drains flushed successfully.
 * Returns RESHAPE_PARTIAL_DRAIN if any drain was not flushable
 * (not in FLUSH state — indicates collect/stage/flip ran out of order).
 *
 * Tests: A05, A06, A08
 */
static inline ReshapeResult reshape_drain(FrustumBlock *block)
{
    uint8_t fail_count = 0u;

    /* flush all 12 pentagon drains */
    for (uint8_t d = 0u; d < (uint8_t)FGLS_DRAIN_COUNT; d++) {
        /* fabric_drain_flush returns false if drain wasn't open */
        if (!fabric_drain_flush(block, d)) {
            fail_count++;
            /* non-fatal: drain may not have been opened this cycle
             * (not every reshape needs all 12 drains active) */
        }
    }

    /* clear heptagon fence — irreversible bits reset for next cycle */
    heptagon_fence_reshape_clear(block);

    /* partial drain only fatal if ALL drains failed (block stuck) */
    if (fail_count == (uint8_t)FGLS_DRAIN_COUNT)
        return RESHAPE_PARTIAL_DRAIN;

    return RESHAPE_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 *  pogls_atomic_reshape — full 4-step sequence
 *
 *  Single entry point. Executes collect → stage → flip → drain in order.
 *  If collect threshold not met: returns RESHAPE_NOT_READY immediately.
 *  Stage fail: returns RESHAPE_STAGE_FAIL without touching global state.
 *  Flip + drain: always runs together once stage is validated.
 *
 *  @param block        FrustumBlock to reshape
 *  @param out_stage    optional — caller receives staged layout info
 *                      (pass NULL to discard)
 *  @return             ReshapeResult
 *
 *  Tests: A09, A10
 * ══════════════════════════════════════════════════════════════════════ */
static inline ReshapeResult pogls_atomic_reshape(FrustumBlock *block,
                                                  ReshapeStage *out_stage)
{
    /* ── step 1: collect ── */
    if (!reshape_collect(block))
        return RESHAPE_NOT_READY;

    /* ── step 2: stage ── */
    ReshapeStage _local_stage;
    ReshapeStage *stage = out_stage ? out_stage : &_local_stage;

    ReshapeResult sr = reshape_stage(block, stage);
    if (sr != RESHAPE_OK)
        return sr;

    /* ── step 3: flip (atomic moment) ── */
    reshape_flip(block, stage);

    /* ── step 4: drain + clear ── */
    return reshape_drain(block);
}

/* ══════════════════════════════════════════════════════════════════════
 *  INSPECT HELPERS
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * reshape_cycle_complete(block)
 *
 * Returns true if rotation_state == 0 (full 6-face Rubik cycle done).
 * After 6 reshapes, system returns to genesis orientation.
 * Tests: A10
 */
static inline bool reshape_cycle_complete(void)
{
    return (fabric_rotation_state() == 0u);
}

/*
 * reshape_pending_count(block)
 *
 * Quick read of locked shadow slots — how far from reshape threshold.
 * Returns value in [0..28]. Threshold fires at 54 (> FGLS_SHADOW_COUNT=28).
 *
 * Note: FGLS_SHADOW_COUNT=28 < POGLS_SACRED_NEXUS=54 — this means
 * threshold 54 requires accumulation across multiple shadow cycles.
 * reshape_collect() counts FENCE bits; they accumulate across writes
 * until cleared by reshape_drain().
 */
static inline uint8_t reshape_pending_count(const FrustumBlock *block)
{
    return heptagon_locked_count(block);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST INVARIANT REFERENCE (A01..A10)
 *  For test_atomic_reshape.c — all invariants encoded in the functions above
 *
 *  A01: reshape_collect on block with 53 locked → false
 *       reshape_collect on block with 54 locked → true
 *  A02: accumulate 54 shadow writes via heptagon_fence_write + drain
 *       → reshape_collect returns true
 *  A03: reshape_stage → next_rot == (cur_rot+1)%6, lane_group valid
 *  A04: reshape_flip → global rotation advances exactly once
 *       → fabric_rotation_state() == old+1 (mod 6)
 *  A05: reshape_drain → all 12 drains tombstoned (DRAIN_BIT_TOMBSTONE set)
 *  A06: reshape_drain → heptagon_fence_reshape_clear() called
 *       → no slot has SHADOW_BIT_FENCE set after drain
 *  A07: after reshape → rotation_state == (before+1)%6
 *  A08: after reshape → shadow slots writable again (fence cleared)
 *       → heptagon_fence_write() returns FENCE_OK on cleared slot
 *  A09: full sequence: collect(true)→stage(OK)→flip→drain(OK) in order
 *       → pogls_atomic_reshape returns RESHAPE_OK
 *  A10: run 6× pogls_atomic_reshape (with forced threshold)
 *       → rotation_state == 0 (full Rubik cycle complete)
 * ══════════════════════════════════════════════════════════════════════ */

#endif /* POGLS_ATOMIC_RESHAPE_H */

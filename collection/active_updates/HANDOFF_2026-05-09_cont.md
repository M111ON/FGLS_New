# HANDOFF — Frustum / Fabric Wire Pipeline
_Session: 2026-05-09 (continuation)_

---

## Tasks Completed This Session

| Task | File | Tests |
|------|------|-------|
| #1 drain_gap wire | `fabric_wire_drain.h` | ✅ 38/38 |
| #2 pentagon drain offset | `test_pentagon_drain.c` | ✅ 12/12 |
| #3 1440 migration overlay | `pogls_1440.h` | ✅ 14/14 |
| #4 rotation state + 1440 wire | `pogls_rotation.h` | ✅ 22/22 |
| #5 heptagon fence full path | `heptagon_fence.h` | ✅ 49/49 |

**Total: 135 tests PASS, 0 FAIL**

---

## Locked Constants

```c
/* fabric_wire.h */
POGLS_TRING_FACES      = 12
POGLS_SACRED_NEXUS     = 54
POGLS_FILE_SIZE        = 4896   /* 17×2×144 */
POGLS_LANE_GROUPS      = 6
POGLS_DRAIN_COSET_SIZE = 408    /* 4896/12 */

/* pogls_1440.h */
POGLS_BASE_CYCLE       = 720
POGLS_EXT_CYCLE        = 1440   /* 2×720 */

/* pogls_rotation.h */
POGLS_SLOTS_PER_ROT_SPATIAL  = 120   /* 720/6  */
POGLS_SLOTS_PER_ROT_TEMPORAL = 240   /* 1440/6 */
```

---

## Locked Behaviors

### Task #1–3 (unchanged)
- `drain_gap = (trit^slope)&1` → `drain_state[]` in FrustumBlock
- `drain_id = trit % 12` → `pogls_pentagon_drain_offset(drain_id)`
- `phase = idx / 720` XOR overlay — core 720 untouched
- header = logical overlay บน `reserved[0..16]`, no extra bytes
- `SHADOW_BIT_FENCE` irreversible once set

### Task #4 — Rotation State (NEW, LOCKED)
- `fabric_rotation_state()` — global uint8, 0..5
- `fabric_rotation_advance_global()` — wraps at POGLS_LANE_GROUPS=6
- **Advance trigger**: `idx == 720` only (phase boundary, once per 1440 cycle)
- `pogls_dispatch_1440_rot(idx)` — returns `Pogls1440SlotR` with `.rot` + `.lane_group`
- **bit6 formula**:
  - phase=0: `(trit ^ slope) & 1` — identical to Task #3, rot inactive
  - phase=1: `(trit ^ slope ^ 1 ^ (phase & rot & 1)) & 1` — rot active
- `pogls_run_cycles(N)` → rot == N%6 after N full 1440 cycles

### Task #5 — Heptagon Fence (NEW, LOCKED)
- **PATH A** `heptagon_fence_write()` — CORE/GEAR→shadow write
  - layer gate → lock gate (FENCE bit) → key gate
- **PATH B** `heptagon_fence_drain()` — shadow→flush read
  - requires `is_drain=true` → SECURITY dst only → key match → sets FENCE bit (irreversible)
- **PATH C** `heptagon_fence_direct_read()` — always `FENCE_DENY_DIRECT`, no exceptions
- **fence_key** = `rot * 2 + phase` → values 0..11
  - 12 distinct states = 12 pentagon drains (POGLS_TRING_FACES) — geometry enforced
  - `fence_key(0,0) = 0` → neutral state, no interference with Tasks #1-3
- **shadow_state bit layout** (extended, additive):
  - bit0 = SHADOW_BIT_OCCUPIED (Task #1, unchanged)
  - bit1 = SHADOW_BIT_FENCE    (Task #1, unchanged)
  - bit2..7 = fence_key stored at write time (6 bits)
- `heptagon_fence_reshape_clear()` — clears FENCE+KEY bits, preserves OCCUPIED
  - called only after Atomic Reshape (Task #6)

---

## File Registry

| File | Status | Description |
|------|--------|-------------|
| `frustum_layout_v2.h` | 🔒 frozen | FrustumBlock 4896B, header overlay |
| `fabric_wire.h` | 🔒 frozen | layer enum, fence primitives, reshape trigger |
| `fabric_wire_drain.h` | 🔒 frozen | drain_gap wire, SHADOW_BIT_*, WireResult |
| `pogls_1440.h` | 🔒 frozen | BASE=720, EXT=1440, phase XOR dispatch |
| `pogls_rotation.h` | 🔒 locked | rotation global driver, fence_key formula |
| `heptagon_fence.h` | 🔒 locked | PATH A/B/C, key storage, reshape_clear hook |

---

## Remaining Tasks

| Task | Spec |
|------|------|
| #6 atomic reshape | `pogls_reshape_ready(54)` trigger → full 4-step sequence |

### Task #6 Spec — Atomic Reshape

```
Sequence (from fabric_wire.h comment, section 5):
  1. collect(shadow)    shadow_flush_pending() >= 54 → trigger
  2. stage(new_layout)  build new lane_group map in staging buffer
  3. flip(rotation)     fabric_rotation_advance_global() — 1-byte write = atomic moment
  4. drain(pentagon)    fabric_drain_flush() × 12 + heptagon_fence_reshape_clear()

Key invariants:
  8  = before_state
  9  = after_state  
  17 = during (does not exist — atomic boundary)
  
Hook points already wired:
  heptagon_fence_reshape_clear()  ← call in step 4
  pogls_reshape_ready(54)         ← trigger in step 1
  fabric_rotation_advance_global() ← call in step 3
  fabric_drain_flush(block, drain_id) × 12 ← call in step 4

Tests needed:
  A01: threshold: reshape_ready(53)=false, reshape_ready(54)=true
  A02: collect phase — shadow accumulates to 54 via heptagon writes
  A03: stage — new lane_group valid after rotation advance
  A04: flip — rotation advances exactly once in reshape sequence
  A05: drain — all 12 pentagon drains flushed + tombstoned
  A06: clear — heptagon_fence_reshape_clear() called after drain
  A07: after reshape — rotation_state == (old+1)%6
  A08: after reshape — shadow slots writable again (fence cleared)
  A09: full sequence: collect→stage→flip→drain→clear in order
  A10: 6 reshapes → rotation back to 0 (full Rubik cycle)
```

---

## Open Prompt (Next Session)

```
POGLS session handoff 2026-05-09 (continuation).

Locked:
  Task #1  drain_gap=(trit^slope)&1 → fabric_wire_drain.h       (38/38)
  Task #2  pentagon drain 12×408B=4896                           (12/12)
  Task #3  1440 overlay phase XOR, core 720 untouched            (14/14)
  Task #4  rotation_state global driver, advance at idx=720      (22/22)
  Task #5  heptagon fence PATH A/B/C, fence_key=rot*2+phase 0..11 (49/49)

Files:
  frustum_layout_v2.h  fabric_wire.h  fabric_wire_drain.h
  pogls_1440.h  pogls_rotation.h  heptagon_fence.h

fence_key formula: rot*2+phase → 12 distinct = 12 pentagon drains
shadow_state bits: bit0=OCCUPIED bit1=FENCE bit2..7=fence_key

Next: Task #6 — Atomic Reshape
  trigger: pogls_reshape_ready(54)
  sequence: collect → stage → flip(rotation) → drain(×12) → clear
  hook: heptagon_fence_reshape_clear() + fabric_rotation_advance_global()
  invariant: 17 = during (does not exist), 8→9 atomic boundary
```

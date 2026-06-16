# Research: Phase 2 — Timeline Mapping

**Date:** 2026-06-16
**Status:** Complete

## Summary

Phase 2 maps the TRing positions produced by Phase 1's 12-face capture to `geo_frame_seek.h` timeline positions (DualFrame). The mapping is a direct 1:1 relationship: TRing position (0..1439) = enc (0..1439), resolved via `frame_at(enc)`. The implementation is minimal — add a `DualFrame` field to the Phase 1 orchestrator output and populate it in one line.

---

## Current Implementation State

### Already Implemented

| Component | File | Lines | Status |
|-----------|------|-------|--------|
| `frame_at(enc)` → DualFrame O(1) | `collection/geo_frame_seek.h` | 98-120 | Stable, tested |
| `tw_tring_to_frame_enc(tring_pos)` | `collection/tw_face_bridge.h` | 597-599 | Stable |
| `tw_face_to_frame(cap)` → DualFrame | `collection/tw_face_bridge.h` | 605-608 | Stable |
| `TWFaceBridgeResult.frame` (DualFrame) | `collection/tw_face_bridge.h` | 746 | Struct field exists |
| T6 test (frame seek integration) | `collection/tests/test_tw_face_bridge.c` | 323-356 | Passing |
| `geo_frame_seek_verify()` self-check | `collection/geo_frame_seek.h` | 163-214 | Passing |

### Phase 1 Orchestrator (To Exist After Phase 1 Execution)

| Component | File | Status |
|-----------|------|--------|
| `TWCapture12FaceResult` struct | `collection/tw_face_bridge.h` | Planned (Plan 01) |
| `tw_capture_tensor_12face()` | `collection/tw_face_bridge.h` | Planned (Plan 01) |
| `tw_capture_tensor_12face_batch()` | `collection/tw_face_bridge.h` | Planned (Plan 01) |

**Key gap:** `TWCapture12FaceResult` currently has no `DualFrame` field. The orchestrator `tw_capture_tensor_12face()` computes `best` (TWFaceCapture) but does not call `frame_at(best.tring_pos)` to populate the DualFrame.

### What Phase 2 Adds

1. Add `DualFrame df` field to `TWCapture12FaceResult`
2. Populate `df` in `tw_capture_tensor_12face()` after getting `best`:
   ```c
   out->df = frame_at(best.tring_pos);
   ```
3. Ensure `tw_capture_tensor_12face_batch()` also has `df` populated through the single-tensor call
4. Integration test: verify round-trip face+zone+slot → TRing → DualFrame → face/slot match

---

## Architecture

### Data Flow

```
Phase 1 pipeline:
  RawBridge (.qdat) → tw_capture_tensor_12face() → TWCapture12FaceResult
      [sig_x, sig_y]     [best TWFaceCapture]        [rewind, freeze_log, n_tensors]

Phase 2 addition (inside same function):
  AFTER tw_capture_priority returns best:
    out->df = frame_at(best.tring_pos);
    // DualFrame fields: face, slot, enc, ico_idx, phase, h.*, p.*
```

### Mapping Formula

```
TRing position (0..1439) → frame enc (0..1439):
  enc = tring_pos  (direct 1:1 mapping, World A)

DualFrame decomposition:
  face = enc / 120              (0..11)
  slot = enc % 120              (0..119)
  phase = (enc / 12) % 12       (0..11 iteration counter)
  ico_idx = enc % 162           (0..161 icosphere address)

  Hilbert:
    group = face % 3            (0..2)
    edge = enc % 3              (0..2)
    is_skip = (enc % 12) >= 9   (0/1)

  Peano:
    step = (enc / 3) % 4        (0..3)
    sub = enc % 3               (0..2)
    hilbert_group = (enc / 12) % 3  (0..2)
```

### Key Property: Model-Agnostic

The frame timeline is purely arithmetic — no dependency on layer count, tensor shapes, or model architecture. Any TRing position from any model maps identically.

---

## Integration Points

| Integration | File | What Connects |
|-------------|------|---------------|
| `TWCapture12FaceResult.df` | `collection/tw_face_bridge.h` | DualFrame alongside rewind/freeze data |
| `frame_at(enc)` call | Inside `tw_capture_tensor_12face()` | Single line after `best` is resolved |
| Round-trip test | `tests/test_tw_face_bridge.c` | New test: generate signature → capture → verify DualFrame face/slot match |

---

## Validation Architecture

### Dimension 1: Unit Tests
- T6 already validates TRing → DualFrame mapping (0..719 hex tring)
- Add T10 to validate Phase 2 integration with orchestrator

### Dimension 2: Round-Trip Test
- Generate synthetic signature (vx, vy)
- Run `tw_capture_tensor_12face()` or simulate with `tw_capture_priority()`
- Verify `out->df.face` and `out->df.slot` match the original capture coordinates
- Verify `out->df.enc == best.tring_pos`

### Dimension 3: Determinism
- Run twice with same inputs → same DualFrame output
- Already guaranteed by O(1) stateless `frame_at()`

---

## Files to Modify

| File | Change |
|------|--------|
| `collection/tw_face_bridge.h` | Add `DualFrame df` to `TWCapture12FaceResult` + populate in orchestrator |
| `collection/tests/test_tw_face_bridge.c` | Add round-trip integration test |

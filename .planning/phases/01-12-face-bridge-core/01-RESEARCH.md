# Research: Phase 1 — 12-Face Bridge Core

**Date:** 2026-06-16
**Status:** Complete

## Summary

Phase 1 wires existing 12-face bridge algorithms into a proper pipeline: real tensor data → TW 2D signature extraction → 12-face priority iteration → TRing 1440 output → rewind buffer + freeze wallet. All core algorithms are already implemented with passing tests (T1–T7). This phase is about orchestration and integration testing.

---

## Current Implementation State

### Existing (Ready to Use)

| Component | File | Lines | Status |
|-----------|------|-------|--------|
| 12-face iteration | `tw_face_bridge.h` | 809 | Fully implemented |
| Priority capture (7 faces) | `tw_face_bridge.h` | 493-530 | Tested, 99.7% coverage |
| Single face capture | `tw_face_bridge.h` | 337-376 | Tested |
| 24-direction capture | `tw_face_bridge.h` | 442-463 | Deferred |
| TRing mapping (0..1439) | `tw_face_bridge.h` | 240-269 | Tested |
| Rewind buffer (1440 slots) | `tw_face_bridge.h` | 66-100 | Tested (T1) |
| Freeze wallet | `tw_face_bridge.h` | 102-221 | Tested (T2) |
| Frame seek integration | `tw_face_bridge.h` | 582-608 | Tested (T6) |
| TW capture (centroid grid) | `tw_capture_int.h` | 359 | Stable |
| Tensor dequant → 2D sig | `tw_tensor_capture.h` | 206 | Stable |
| RawBridge (.qdat loader) | `geom_raw_bridge.h` | 496 | Stable |

### What's Missing

1. **Orchestrator function** — No single function wraps `tw_tensor_capture` + `tw_capture_priority` + rewind store + freeze wallet in one call (D-04, D-05)
2. **Proper T8 integration test** — T8 uses simplified first-8-bytes signature instead of proper `tw_capture_tensor_by_name()` (noted in CONTEXT.md §code_context)
3. **Multi-architecture validation** — Only SmolLM2 data exists; need at least one other architecture (D-07)
4. **Benchmark** — 2K t/s target for 12-face vs 26K t/s baseline (success criterion #5)

---

## Architecture

### Pipeline Flow

```
RawBridge (.qdat files)
    ↓ rb_load / rb_get
Tensor bytes (Q8_0 / F32)
    ↓ tw_capture_tensor_raw
TWTensorCapture { sig_x, sig_y, cap }
    ↓ tw_capture_priority(vx, vy, &best_out, 7)
TWFaceCapture { face, zone, slot, tring_pos, resid, drain }
    ↓ tw_face_rewind_store          tw_face_freeze_entry
TWFaceRewind (1440 slots)          TWFreezeEntry array
```

### New Orchestrator Signature

```c
typedef struct {
    TWFaceRewind    rewind;         /* populated rewind buffer     */
    TWFreezeEntry  *freeze_log;     /* malloc'd freeze entries     */
    uint32_t        n_frozen;       /* number of frozen entries    */
    TWFaceCapture   best;           /* best overall capture        */
    uint32_t        n_tensors;      /* tensors captured            */
} TWCapture12FaceResult;

int tw_capture_tensor_12face(RawBridge *rb, const char *tensor_name,
                              uint32_t tick, uint8_t layer,
                              TWCapture12FaceResult *out);
```

Returns 0 on success, -1 on error (tensor not found, malloc failure).

### Priority Order

`TW_FACE_PRIORITY = {0, 3, 5, 6, 2, 1, 4}` — fixed per D-03, 99.7% coverage per benchmark.

### Memory

- `freeze_log` is heap-allocated (up to 7 entries per tensor — 7 faces max frozen)
- Caller must call `tw_capture_12face_free()` to release allocations
- Rewind buffer is stack-allocated (1440 × 8 = 11520 bytes) — no malloc

---

## Test Data

### SmolLM2-360M

- `build/smollm2_tensors_raw/` — 580 files (290 .qdat + 290 .qtype)
- 32 layers, ~290 tensors
- dtype=8 (Q8_0) for all tensors
- `build/smollm2_tensors_raw/token_embd.weight.qdat` is largest (50MB)

### Multi-Architecture

- `build/smolvlm_tensors_raw/` — SmolVLM data (different architecture from SmolLM2)
- Should produce valid TW captures despite different layer counts/weight shapes
- Architecture-agnostic: TW capture reads first row of first weight column as 2D signature

### Capture Rate Baseline

- Single-face: 26K t/s (reported in PROJECT.md §Validated)
- 12-face: estimated 26K/12 ≈ 2.2K t/s (before overhead of rotation + 7 faces)
- Target: >2K t/s (success criterion #5)
- Overhead sources: 7 face rotations (int64), 7 sector lookups, centroid centroid computation
- All integer/fixed-point — no float overhead

---

## Integration Points

| Integration | File | What Connects |
|-------------|------|---------------|
| tw_tensor_capture → new orchestrator | `tw_face_bridge.h` | `TWTensorCapture.sig_x, sig_y` → `tw_capture_priority()` |
| Orchestrator → rewind buffer | `tw_face_bridge.h` | `tw_face_rewind_store()` per face |
| Orchestrator → freeze wallet | `tw_face_bridge.h` | `tw_face_freeze_entry()` → `TWFreezeEntry[]` |
| Test → real tensor data | `tests/test_tw_face_bridge.c` | `rb_load()` → `tw_capture_tensor_by_name()` |

---

## Validation Architecture

### Dimension 1: Unit Tests (T1–T7)
Already passing. No changes needed.

### Dimension 2: Orchestrator Integration Test
New test that:
1. Loads RawBridge from `build/smollm2_tensors_raw/`
2. Iterates first 10 tensors through `tw_capture_tensor_12face()`
3. Verifies rewind buffer is populated
4. Verifies freeze entries are consistent
5. Repeats on `build/smolvlm_tensors_raw/` for multi-architecture

### Dimension 3: Lossless Roundtrip
Verify that captured TRing positions + resid → reconstruction matches original.

### Dimension 4: Benchmark
Time `tw_capture_tensor_12face()` over 10+ tensors, compute t/s. Verify >2K t/s.

---

## Files to Modify

| File | Change |
|------|--------|
| `collection/tw_face_bridge.h` | Add orchestrator function + result type + free function |
| `collection/tests/test_tw_face_bridge.c` | Upgrade T8, add orchestrator test, add benchmark |

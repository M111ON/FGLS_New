# Pattern Map: Phase 1 — 12-Face Bridge Core

---

## Existing Patterns

### Pattern 1: `static inline` Header-Only Library

Every file in `collection/` is a `.h` file with `static inline` functions. No `.c` compilation units, no linking. The pattern is:

```c
// tw_face_bridge.h
#ifndef GUARD_H
#define GUARD_H
#include <stdint.h>
// ... static inline implementations ...
#endif
```

**Analog:** All headers in collection/ — `tw_capture_int.h`, `tw_face_bridge.h`, `tw_tensor_capture.h`, `geom_raw_bridge.h`

**Rule:** New orchestrator follows this pattern. No separate .c file.

### Pattern 2: Return Values via Out-Parameters

Functions return `void` or `int` (error code) and write results via pointer parameters:

```c
// tw_capture_int.h
static inline void tw_capture_int(int64_t vx, int64_t vy, TWCaptureInt *out);

// tw_face_bridge.h
static inline void tw_iterate_faces(int64_t vx, int64_t vy, TWFaceIterResult *result);
```

**Analog:** `tw_capture_int`, `tw_capture_face`, `tw_iterate_faces`, `tw_capture_priority`

**Rule:** New `tw_capture_tensor_12face()` follows same pattern: `int tw_capture_tensor_12face(RawBridge *rb, const char *name, uint32_t tick, uint8_t layer, TWCapture12FaceResult *out)`

### Pattern 3: Integer-Only Fixed-Point (`TW_SCALE = 207360`)

All geometry uses `int64_t` coordinates scaled by `TW_SCALE = 207360 = 12^4 * 10`. No float in the capture path. Rotation uses pre-computed `_TW_ROT_COS[12]` / `_TW_ROT_SIN[12]` LUTs.

**Analog:** `_tw_centroid_to_face0()`, `tw_capture_face()` rotation

**Rule:** Orchestrator uses existing `vx, vy` from `TWTensorCapture` (already scaled to TW_SCALE). No new rotation math needed.

### Pattern 4: No malloc in Hot Path

All hot data structures (TWFaceRewind, TWCaptureInt, TWTensorCapture) are stack-allocated. Malloc is only used for freeze wallet entry arrays (sparse, controlled allocation).

**Analog:** `tw_freeze_wallet_read()` uses malloc; `tw_freeze_wallet_write()` does not.

**Rule:** Orchestrator uses stack-allocated `TWFaceCapture caps[7]` for priority capture. Freeze log entries are malloc'd only if `n_frozen > 0`.

### Pattern 5: Test Framework — Simple ASSERT macros

Tests use custom `ASSERT()`, `ASSERT_EQ()`, and `CHECK()` macros with pass/fail counters. No external test framework.

**Analog:** `tests/test_tw_face_bridge.c`

**Rule:** New tests follow same pattern.

---

## File Classification

| File | Role | Data Flow |
|------|------|-----------|
| `collection/tw_face_bridge.h` | Orchestration | Contains all 12-face bridge logic + rewind buffer + freeze wallet |
| `collection/tw_capture_int.h` | Computation | Pure integer centroid capture on hex/tri grids |
| `collection/tw_tensor_capture.h` | Tensor I/O | Dequant + 2D signature from raw bytes |
| `collection/geom_raw_bridge.h` | Tensor I/O | RawBridge .qdat loader |
| `collection/tests/test_tw_face_bridge.c` | Test | All bridge tests (T1–T8) |

---

## Data Flow (Target State)

```
geom_raw_bridge.h           tw_tensor_capture.h         tw_face_bridge.h
┌─────────────────┐        ┌───────────────────┐       ┌────────────────────────────┐
│ RawBridge        │───────→│ TWTensorCapture    │──────→│ TWCapture12FaceResult      │
│ rb_load()        │        │ tw_capture_tensor_ │       │   .rewind (TWFaceRewind)   │
│ rb_get()         │        │   raw() / by_name()│       │   .freeze_log (entries[])  │
│ .qdat files      │        │ sig_x, sig_y       │       │   .best (TWFaceCapture)    │
└─────────────────┘        └───────────────────┘       │   .n_tensors              │
                                                        └────────────────────────────┘
                                                                       │
                                                ┌──────────────────────┐│
                                                │ tw_capture_priority() ││
                                                │ (7 faces, 99.7%)    ││
                                                └──────────────────────┘│
                                                                       │
                                          ┌──────────────────┐  ┌──────────────┐
                                          │ tw_face_rewind   │  │ freeze_entry │
                                          │   _store()       │  │   array      │
                                          │ (1440 slots)     │  │ (wallet)     │
                                          └──────────────────┘  └──────────────┘
```

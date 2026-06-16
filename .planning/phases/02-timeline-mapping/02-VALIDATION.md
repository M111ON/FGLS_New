---
phase: 02
slug: timeline-mapping
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-06-16
---

# Phase 2 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Custom ASSERT macros (header-only, no framework) |
| **Config file** | none — single-file test binary |
| **Quick run command** | `gcc -DGEO_JUMP_INLINE -I. -I../geo_jump_module/include -I../src -o tests/test_tw_face_bridge.exe tests/test_tw_face_bridge.c -lm && ./tests/test_tw_face_bridge.exe` |
| **Full suite command** | Same as quick run (all tests in one binary) |
| **Estimated runtime** | ~2 seconds |

---

## Sampling Rate

- **After every task commit:** Run `gcc ... && ./test_tw_face_bridge.exe | grep -E "FAIL|PASS|T[0-9]"` 
- **After every plan wave:** Full suite green
- **Before `/gsd-verify-work`:** Full suite must be green
- **Max feedback latency:** 5 seconds

---

## Per-task Verification Map

| task ID | Plan | Wave | Requirement | Threat Ref | Secure Behavior | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|------------|-----------------|-----------|-------------------|-------------|--------|
| 02-01-00 | 01 | 1 | TIME-01 | N/A | N/A | unit | grep check: `DualFrame df` in `TWCapture12FaceResult` | ❌ W0 | ⬜ pending |
| 02-01-01 | 01 | 1 | TIME-01 | N/A | N/A | unit | grep: `frame_at` call in `tw_capture_tensor_12face()` | ❌ W0 | ⬜ pending |
| 02-02-00 | 02 | 2 | TIME-01 | N/A | N/A | integration | `./test_tw_face_bridge.exe` T10 passes | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `collection/tests/test_tw_face_bridge.c` — T10 integration test stub

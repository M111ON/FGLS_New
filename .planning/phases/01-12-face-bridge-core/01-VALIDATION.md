---
phase: 01
slug: 12-face-bridge-core
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-06-16
---

# Phase 01 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Custom (C, gcc assertions via ASSERT/ASSERT_EQ macros) |
| **Config file** | `runner/Makefile` |
| **Quick run command** | `cd collection && gcc -DGEO_JUMP_INLINE -I. -I../geo_jump_module/include -I../src -o tests/test_tw_face_bridge.exe tests/test_tw_face_bridge.c -lm && tests/test_tw_face_bridge.exe` |
| **Full suite command** | Same (single test binary) |
| **Estimated runtime** | ~5 seconds (without tensor data) / ~30 seconds (with real tensors) |

---

## Sampling Rate

- **After every task commit:** Run `test_tw_face_bridge.exe` with existing test data
- **After every plan wave:** Run full suite (T1–T8 + new orchestrator tests)
- **Before `/gsd-verify-work`:** Full suite must be green on both SmolLM2 and SmolVLM

---

## Per-task Verification Map

| task ID | Plan | Wave | Requirement | Test Type | Command | Status |
|---------|------|------|-------------|-----------|---------|--------|
| 01-01-01 | 01 | 1 | MFACE-01 | unit | `gcc ... -DTEST_WITH_TENSORS && ./test_tw_face_bridge.exe build/smollm2_tensors_raw` | ⬜ pending |
| 01-01-02 | 01 | 1 | MFACE-01 | unit | `./test_tw_face_bridge.exe build/smolvlm_tensors_raw` | ⬜ pending |
| 01-02-01 | 02 | 1 | MFACE-01 | benchmark | `gcc ... -DBENCHMARK && ./test_tw_face_bridge.exe` | ⬜ pending |

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements. No new test framework needed.

---

## Manual-Only Verifications

All phase behaviors have automated verification.

---

## Validation Sign-Off

- [ ] All tasks have automated verify commands
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending

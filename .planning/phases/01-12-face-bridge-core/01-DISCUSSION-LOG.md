# Phase 1: 12-Face Bridge Core - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-06-16
**Phase:** 1-12-Face-Bridge-Core
**Areas discussed:** TRing resolution, Iteration strategy, Pipeline orchestration, Model-agnostic testing

---

## TRing Resolution

| Option | Description | Selected |
|--------|-------------|----------|
| 720 (hex only) | เฟส 2 แค่ 720 ก็พอ — tri grid เก็บไว้ทีหลัง | |
| 1440 (hex+tri) | 1440 เต็มตั้งแต่เฟส 1 — จะได้ไม่ต้องกลับมาเพิ่มทีหลัง | ✓ |

**User's choice:** 1440 (hex+tri)
**Notes:** Full resolution from the start. Code already supports both.

---

## Iteration Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Full 12-face | 12 faces เต็ม — pipeline ต้อง deterministic ครบทุก face | |
| Priority 7 faces | 7 faces แบบ priority (0,3,5,6,2,1,4) = 99.7% coverage | ✓ |
| Priority 4 faces | 4 faces พอ = 91% coverage เร็วที่สุด แต่อาจพลาดบางรายการ | |

**User's choice:** Priority 7 faces
**Notes:** 99.7% coverage, saves ~40% compute vs full 12-face.

---

## Pipeline Orchestration

| Option | Description | Selected |
|--------|-------------|----------|
| New orchestrator function | ฟังก์ชันใหม่อย่าง tw_capture_tensor_12face() | ✓ |
| Manual two-step call | ให้ tw_tensor_capture return sig_x/sig_y แล้ว caller เป็นคนเรียก tw_iterate_faces เอง | |
| Macro wrapper | macro wrapper ที่ inline รวมทั้งสองขั้นตอน | |

**User's choice:** New orchestrator function
**Notes:** Single clean interface wrapping tw_tensor_capture + tw_iterate_faces.

---

## Model-Agnostic Testing

| Option | Description | Selected |
|--------|-------------|----------|
| Use existing .qdat test data | ใช้ .qdat + .qtype จาก build_smollm2_store.py สำหรับ 2-3 architecture | ✓ |
| Wait for Phase 3 runner | รอให้ Phase 3 integration แล้วค่อย test กับ GGUF จริง | |
| Synthetic tensor tests | เขียน synthetic tensor data ที่ mimic shapes ต่างๆ | |

**User's choice:** Use existing .qdat test data
**Notes:** Minimum SmolLM2 + one more architecture (Qwen or Llama variant).

---

## OpenCode's Discretion

No areas deferred to OpenCode — all decisions discussed explicitly.

## Deferred Ideas

- None — discussion stayed within phase scope.

---

*Phase: 1-12-Face-Bridge-Core*
*Discussion logged: 2026-06-16*

# Phase 2: Timeline Mapping - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-06-16
**Phase:** 2-Timeline-Mapping
**Areas discussed:** Mapping approach

---

## Mapping Approach

| Option | Description | Selected |
|--------|-------------|----------|
| Separate wrapper | ฟังก์ชัน tw_tring_to_frame(tring_pos) แยก เรียก frame_at ภายใน | |
| Integrate into orchestrator | ฝัง frame_at ไว้ใน orchestrator จาก Phase 1 เสร็จใน call เดียว | ✓ |
| Direct call | macro หรือ inline direct call ของ frame_at | |

**User's choice:** Integrate into orchestrator
**Notes:** O(1) call with no cost, TRing position already available in orchestrator. Single-pass design.

---

## OpenCode's Discretion

No areas deferred to OpenCode — decision discussed explicitly.

## Deferred Ideas

- Timeline walk semantics (stride-37) — not relevant for TRing → enc mapping.
- geo_rewind.h integration — deferred to v2 (TIME-02).

---

*Phase: 2-Timeline-Mapping*
*Discussion logged: 2026-06-16*

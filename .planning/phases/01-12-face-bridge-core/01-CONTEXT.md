# Phase 1: 12-Face Bridge Core - Context

**Gathered:** 2026-06-16
**Status:** Ready for planning

<domain>
## Phase Boundary

Wire the existing 12-face bridge core algorithms (in `collection/tw_face_bridge.h`) into a proper pipeline where real tensor data → TW 2D signature extraction → 12-face iteration → TRing 1440 output. The core algorithms (`tw_iterate_faces`, `tw_capture_face`, `tw_capture_priority`, `tw_iterate_faces_24`) are already implemented with passing tests — this phase is about orchestration, not algorithm development.

</domain>

<decisions>
## Implementation Decisions

### TRing Resolution
- **D-01:** Output TRing 1440 (hex + tri combined grids) from the start. Phase 2 will consume full 1440-position timeline. The code already supports both resolutions (`TW_TRING_FULL = 720`, `TW_TRING_1440 = 1440`) — use the full one.

### Iteration Strategy
- **D-02:** Use priority-based iteration with 7 faces in order [0, 3, 5, 6, 2, 1, 4] = 99.7% coverage per benchmark. This saves ~40% compute vs full 12-face iteration while maintaining near-complete coverage.
- **D-03:** The priority order is fixed (from `TW_FACE_PRIORITY` in `tw_face_bridge.h`). Do not change without re-benchmarking.

### Pipeline Orchestration
- **D-04:** Create a new orchestrator function (e.g., `tw_capture_tensor_12face`) that wraps both steps: `tw_tensor_capture` (2D signature extraction from dequantized tensor data) → `tw_capture_priority` (12-face bridge iteration). Single call, clean interface.
- **D-05:** The orchestrator should output to both the rewind buffer (`TWFaceRewind`) and freeze wallet (`TWFreezeEntry` array) in one pass.

### Model-Agnostic Testing
- **D-06:** Validate on multiple architectures using existing `.qdat` + `.qtype` test data from `build_smollm2_store.py`. The `.qdat` format tensor data can be generated for any GGUF model, not just SmolLM2.
- **D-07:** Minimum test coverage: SmolLM2-360M + at least one other architecture (Qwen or Llama variant) with real quantized data.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Milestone & Requirements
- `.planning/ROADMAP.md` §Phase 1 — Phase goal, success criteria, dependencies
- `.planning/REQUIREMENTS.md` §Multi-Face Capture — MFACE-01: TW capture across all 12 faces → TRing 720
- `.planning/PROJECT.md` — Overall project context, constraints, key decisions

### Existing Implementation (must read)
- `collection/tw_face_bridge.h` — 12-face bridge core: `tw_iterate_faces`, `tw_capture_face`, `tw_capture_priority`, `tw_capture_int_to_face`, `tw_face_to_tring`, rewind buffer, freeze wallet
- `collection/tw_capture_int.h` — Integer-only TW capture: `tw_capture_int`, `tw_capture_int_combined`, `tw_capture_int_tri`, sector/slot centroid tables
- `collection/tw_tensor_capture.h` — Tensor data dequant → 2D signature: `TWTensorCapture`, `tw_capture_tensor_by_name`, `tw_capture_tensor_raw`
- `collection/tests/test_tw_face_bridge.c` — Existing test suite for all face bridge components (T1-T8)

### Supporting
- `collection/geom_raw_bridge.h` — RawBridge tensor data loader (used by T8 pipeline test)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `tw_iterate_faces(vx, vy, &result)` — Captures all 12 faces from a face-0 signature pair (fully implemented)
- `tw_capture_priority(vx, vy, &best_out, max_faces)` — Priority-based capture returning best match (D-02: use 7 faces)
- `tw_capture_face(vx, vy, f, &out)` — Single face capture with rotation + resid in face-0 frame
- `tw_face_to_tring(face, zone, slot, is_tri)` → TRing position 0..1439
- `TWFaceRewind` / `tw_face_rewind_store` — Circular rewind buffer (1440 slots)
- `tw_freeze_wallet_write` / `tw_freeze_wallet_read` — Binary freeze wallet format
- `tw_tensor_capture_tensor_by_name` / `tw_capture_tensor_raw` — Tensor → TWTensorCapture (sig_x, sig_y)

### Established Patterns
- Integer-only fixed-point arithmetic (`TW_SCALE = 207360`) — no float in capture path
- All functions are `static inline` — header-only library style
- Vx/Vy signature in face-0 frame, rotation to face-f internally

### Integration Points
- `tw_tensor_capture.h` (TWTensorCapture.sig_x, sig_y) → new orchestrator → `tw_capture_priority(vx, vy, &best_out, 7)` → rewind store + freeze wallet
- Existing T8 test in `test_tw_face_bridge.c` shows the wiring pattern (simplified signature) — replace simplified sig with proper `tw_tensor_capture` call

</code_context>

<specifics>
## Specific Ideas

No specific references — open to standard approaches within the patterns above. The existing T8 test (`test_full_pipeline` in `test_tw_face_bridge.c`) demonstrates the expected end-to-end flow, though it uses a simplified signature (first 8 bytes as int32) rather than the proper dequant → 2D signature pipeline.

</specifics>

<deferred>
## Deferred Ideas

- **Full 12-face iteration** (not priority) — Can be enabled later if benchmark shows >0.3% information loss. Not in scope for Phase 1.
- **24-direction capture** (`tw_iterate_faces_24`) — Stored as separate data for future analysis. Not wired into the main pipeline.
- **TRing 720 (hex only)** — The code supports it via `TW_TRING_FULL` but D-01 chooses 1440. Not needed unless Phase 2 timeline imposes a 720-slot constraint.

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 1-12-Face-Bridge-Core*
*Context gathered: 2026-06-16*

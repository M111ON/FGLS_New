# Phase 2: Timeline Mapping - Context

**Gathered:** 2026-06-16
**Status:** Ready for planning

<domain>
## Phase Boundary

Map the TRing positions produced by Phase 1's 12-face capture to `geo_frame_seek.h` timeline positions (DualFrame). The mapping is a direct 1:1 relationship: TRing position (0..1439) = enc (0..1439), resolved via `frame_at(enc)`. This phase integrates the mapping into Phase 1's orchestrator rather than creating a separate post-processing step.

</domain>

<carry_forward>

## Carrying Forward from Phase 1 (D-01)

- TRing resolution is 1440 (hex+tri) — aligns with FRAME_CYCLE = 1440
- Priority 7-face iteration [0,3,5,6,2,1,4] produces TRing positions fed into this mapping

</carry_forward>

<decisions>
## Implementation Decisions

### Mapping Integration
- **D-08:** Timeline mapping (`frame_at(tring_pos)`) is integrated into Phase 1's orchestrator (`tw_capture_tensor_12face`), not a separate function call. The orchestrator already computes TRing position — calling `frame_at()` is O(1) with no state, eliminates an extra pass, and produces DualFrame alongside TWFaceCapture in one pass.
- **D-09:** No wrapper function needed — direct call to `frame_at(tring_pos)` from `collection/geo_frame_seek.h`. The header is already included by `tw_face_bridge.h`.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Milestone & Requirements
- `.planning/ROADMAP.md` §Phase 2 — Phase goal, success criteria
- `.planning/REQUIREMENTS.md` §Timeline Integration — TIME-01: face+zone+slot → geo_frame_seek.h timeline position

### Existing Implementation (must read)
- `collection/geo_frame_seek.h` — Frame timeline: `frame_at(enc)`, `frame_enc(t)`, `DualFrame`, `FRAME_CYCLE=1440`
- `collection/tw_face_bridge.h` — 12-face bridge: `TWFaceCapture`, `tw_face_to_tring`, `tw_capture_priority` (produces TRing positions fed to frame_at)
- `.planning/phases/01-12-face-bridge-core/01-CONTEXT.md` — Phase 1 decisions (D-01 through D-07)

### Tests
- `collection/tests/test_tw_face_bridge.c` §T6 — Existing frame seek integration test

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `frame_at(enc)` — O(1) decomposes enc (0..1439) → DualFrame {face, slot, Hilbert, Peano, ico_idx, phase}
- `DualFrame` — Already contains `face` and `slot` fields that align with TW capture coordinates
- `TWFaceCapture.tring_pos` — TRing position (0..1439) from Phase 1 orchestrator, fed directly to `frame_at()`
- Existing T6 test (`test_frame_seek`) already validates frame seek integration

### Established Patterns
- O(1) stateless computation — no state or memory allocation
- Face (0..11) derived directly from position: `face = enc / 120`

### Integration Points
- Phase 1 orchestrator output struct → add DualFrame field alongside TWFaceCapture
- Single call inside orchestrator: `out->df = frame_at(out->tring_pos);`

</code_context>

<specifics>
## Specific Ideas

No specific requirements — mapping is deterministic and well-defined by `geo_frame_seek.h`. The integration pattern follows Phase 1's single-pass design philosophy.

</specifics>

<deferred>
## Deferred Ideas

- **Timeline walk semantics** (`frame_enc(t) = t×37%1440`) — The stride-37 walk is for temporal navigation across time steps. TRing position maps directly to `enc`, not to time `t`. Not relevant for this phase.
- **geo_rewind.h integration** (TIME-02) — Deferred to v2. This phase only maps to frame timeline, not to the rewind buffer.

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 2-Timeline-Mapping*
*Context gathered: 2026-06-16*

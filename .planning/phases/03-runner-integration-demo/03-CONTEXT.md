# Phase 3: Runner Integration & Demo - Context

**Gathered:** 2026-06-16
**Status:** Ready for planning

<domain>
## Phase Boundary

Integrate the full 12-face capture pipeline (orchestrator from Phase 1 + timeline mapping from Phase 2) into the SID runner (`llama_pogls_runner_sid_v2.c`). Add a `--capture` CLI flag that triggers capture after decode, writes output to both freeze wallet (.tw) and .gsten store, and performs lossless verification against original tensor data.

</domain>

<carry_forward>

## Carrying Forward from Prior Phases

### Phase 1 (D-03, D-04, D-05)
- Priority 7-face iteration with orchestrator `tw_capture_tensor_12face()`
- Output to rewind buffer + freeze wallet in one pass
- TRing 1440 resolution

### Phase 2 (D-08, D-09)
- Timeline mapping (`frame_at(tring_pos)`) integrated into orchestrator
- Direct call to `frame_at()` from `geo_frame_seek.h`

</carry_forward>

<decisions>
## Implementation Decisions

### CLI Flag & Capture Timing
- **D-10:** `--capture <outdir>` flag — specifies output directory for store files. When present, the pipeline runs after the initial batch decode (prompt ingestion) and optionally per-token during generation.
- **D-11:** Capture triggers after the initial `llama_decode` (prompt eval). Per-token capture during generation is deferred (opt-in via `--capture-every-token`).

### Output Format
- **D-12:** Write both freeze wallet (.tw binary) AND .gsten store on each capture. Freeze wallet via existing `tw_freeze_wallet_write()`. .gsten via `gb_load/gb_decode_*` format.
- **D-13:** Output directory structure: `{outdir}/capture-{timestamp}.tw` and `{outdir}/store-{timestamp}.gsten`.

### Verification
- **D-14:** Lossless verification: after capture, reconstruct from stored data and compare against original tensor data byte-by-byte. Report: `lossless: ✓ (N tensors verified)` or `lossless: ✗ (first mismatch at tensor X)`.
- **D-15:** The verification is part of the runner's `--capture` mode, not a separate tool. Show a one-line summary at the end.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Milestone & Requirements
- `.planning/ROADMAP.md` §Phase 3 — Phase goal, success criteria
- `.planning/REQUIREMENTS.md` §Runner Demo — DEMO-01: E2E pipeline demo from runner decode → capture → store, model-agnostic

### Runner Implementation (must read)
- `runner/llama_pogls_runner_sid_v2.c` — SID runner with decode loops, SID swap, CLI argument parsing
- `runner/sid_cache.h` — SID cache management
- `runner/sid_loader.h` — GGUF tensor loader

### Collection Pipeline (must read)
- `collection/tw_face_bridge.h` — 12-face bridge: `tw_capture_priority`, `tw_face_to_tring`, rewind buffer
- `collection/tw_capture_int.h` — TW capture integer core
- `collection/tw_tensor_capture.h` — Tensor data dequant → 2D signature
- `collection/geo_frame_seek.h` — Timeline mapping: `frame_at(enc)`, `DualFrame`
- `collection/tests/test_tw_face_bridge.c` — Existing pipeline test (T8)

### Prior Context
- `.planning/phases/01-12-face-bridge-core/01-CONTEXT.md` — Phase 1 decisions (D-01 through D-07)
- `.planning/phases/02-timeline-mapping/02-CONTEXT.md` — Phase 2 decisions (D-08, D-09)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- Full 12-face capture pipeline from Phase 1 + Phase 2 (orchestrator + timeline mapping)
- Existing decode loops (chat + prompt) with SID swap pattern — capture hooks inserted after `llama_decode()`
- `sid_swap_apply()` / `sid_swap_restore()` — proven per-decode swap pattern
- `gguf_idx_open` / `gguf_idx_close` — GGUF index for tensor name resolution
- `tw_freeze_wallet_write()` — binary freeze wallet serialization

### Established Patterns
- Simple `if/else` argument parsing — `--capture <outdir>` follows existing flag pattern
- SID swap: `apply → decode → restore` around each `llama_decode()` call
- Tensor data accessible via `tensor->data` pointers after model load

### Integration Points
- After `llama_decode(lctx, batch)` in chat loop (line ~910) — capture prompt tensors
- After `llama_decode(lctx, pb)` in prompt mode (line ~942) — capture prompt tensors
- New `if (opt_capture)` blocks after each decode call in both paths
- Output dir created via `mkdir()` if not exists

</code_context>

<specifics>
## Specific Ideas

The existing T8 test in `test_tw_face_bridge.c` (simplified signature pipeline) serves as a reference for the orchestration flow.

</specifics>

<deferred>
## Deferred Ideas

- **Per-token capture** (`--capture-every-token`) — Captures after each generated token during autoregressive decoding. Useful for timeline analysis but high overhead. Deferred to post-demo.
- **Real-time visualization** — Out of scope for this milestone (documented in PROJECT.md Out of Scope).

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 3-Runner-Integration-Demo*
*Context gathered: 2026-06-16*

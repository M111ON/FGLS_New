# Roadmap: v1.0 — 12-Face Bridge Pipeline

**Milestone:** v1.0 — 12-Face Bridge Pipeline
**Defined:** 2026-06-16
**Total phases:** 3
**Total requirements:** 3
**Coverage:** 3/3 ✓

## Phase 1: 12-Face Bridge Core

**Goal:** Extend single-face TW capture to iterate across all 12 dodecahedron faces, producing a complete TRing 720 coordinate set per tensor.

**Requirements:** MFACE-01

**Success criteria:**
1. TW capture loop iterates across all 12 face transformations for each tensor
2. Output includes face index (0-11) alongside existing zone/slot/resid coordinates
3. TRing 720 positions computed correctly for all 12 faces
4. Phase 1 pipeline runs on SmolLM2-360M and at least one other GGUF architecture
5. Capture rate stays above 2K t/s for 12-face iteration (vs 26K t/s single-face)

**Dependencies:** None

## Phase 2: Timeline Mapping

**Goal:** Map face+zone+slot coordinates to geo_frame_seek.h timeline positions, implementing the bridge between geometric capture and timeline navigation.

**Requirements:** TIME-01

**Success criteria:**
1. geo_frame_seek.h timeline position derived from (face, zone, slot) triplet
2. Timeline mapping is model-agnostic (works for any GGUF architecture)
3. Integration test verifies round-trip: face+zone+slot → timeline position
4. Position distribution is deterministic and consistent across runs

**Dependencies:** Phase 1

## Phase 3: Runner Integration & Demo

**Goal:** Wire the full 12-face capture pipeline into the SID runner (llama_pogls_runner_sid_v2.c) and demonstrate end-to-end capture from runner decode → capture → store on any GGUF model.

**Requirements:** DEMO-01

**Success criteria:**
1. SID runner has a `--capture` flag or mode that triggers 12-face pipeline after decode
2. Pipeline output written to .gsten or .qdat store file
3. Runner demo runs successfully on SmolLM2-360M and one additional GGUF model
4. Captured data can be verified (re-read and compared against original)
5. No changes to llama.cpp itself — only collection/ headers and runner code

**Dependencies:** Phase 1

---

## Coverage Map

| Requirement | Phase | Status |
|-------------|-------|--------|
| MFACE-01 | Phase 1 | Pending |
| TIME-01 | Phase 2 | Pending |
| DEMO-01 | Phase 3 | Pending |

**Coverage:**
- v1 requirements: 3 total
- Mapped to phases: 3
- Unmapped: 0 ✓

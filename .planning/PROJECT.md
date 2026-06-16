# FGLS (Geometric File & Library System)

## What This Is

A geometric storage architecture for LLM tensor data, built on POGLS/FGLS/LC principles. Captures tensor weight data from GGUF models into a geometric coordinate system (TW signatures → zones/slots/resid), bridges across dodecahedron faces into TRing 720, and provides a rewind buffer for timeline-based navigation. Integrates with the SID runtime for live tensor injection during llama.cpp inference.

## Core Value

Lossless geometric capture and reconstruction of LLM tensor data — every tensor that goes in comes back out bit-identical, with a discoverable geometric addressing layer on top.

## Requirements

### Validated

<!-- Shipped and confirmed valuable. -->

- ✓ **TW single-face capture**: Q8_0/F32 dequant → 2D signature → (zone, slot, resid) for single face — verified on SmolLM2-360M, 290/290 lossless, 26K t/s
- ✓ **SID per-decode swap**: tensor->data swap between llama_decode calls works on CPU backend — proven with logit diff (151935/151936 differ, max_diff=0.358)
- ✓ **291/291 tensor discovery**: GGUF index + model struct scan finds all tensors via VirtualQuery memory-safe scanning
- ✓ **Bond discovery + hotness prediction**: 2320 bonds across 290 tensors, cardioid express + Metatron topology + hotness propagation
- ✓ **Delta ring time travel**: Pointer-only journaling for rewind/fast-forward of SID swap state
- ✓ **Full system audit**: 50+ headers catalogued, 12 identified as wired-end-to-end, 12 identified as defined-but-unwired

### Active

<!-- Current scope. Building toward these. -->

- [ ] **FACE-01**: TW capture iterates across all 12 dodecahedron faces → complete TRing 720
- [ ] **FACE-02**: Face+zone+slot maps to geo_frame_seek.h timeline position
- [ ] **FACE-03**: Rewind buffer (geo_rewind.h, 972-slot) connected to frame seek
- [ ] **FACE-04**: Wallet serialize writes .pogwallet disk file from bridge output
- [ ] **FACE-05**: .gsten reader (gb_load/gb_decode_*) wired to production caller
- [ ] **FACE-06**: End-to-end pipeline demo from runner decode → capture → store, model-agnostic

### Out of Scope

<!-- Explicit boundaries. Includes reasoning to prevent re-adding. -->

- Real-time visualization — Geopixel/hex tile rendering of tensor geometry is separate from capture pipeline
- GPU tensor capture — CPU backend proven; GPU (Vulkan/CUDA) tensor discovery deferred
- Compression optimization — Q8_0 at entropy limit; routing-layer compression (74 KB from 367 MB) is sufficient
- Production GGUF writing — .qdat/.gsten is test format; GGUF source is the real store
- Multi-GPU inference support — SID swap works with any backend, but GPU-specific tensor layout adaptation deferred

## Context

- Built on top of llama.cpp (ggml backend) — tensor struct layout known, per-decode swap proven
- TW capture pipeline: RawBridge → TW capture → (zone, slot, resid) → Shell 1+2+3 bridge → frozen → wallet genesis
- Bond discovery: cardioid express (layer-position based), Metatron topology (orbital/chiral/cross)
- SID runner (llama_pogls_runner_sid_v2.c) has --bond, --sid-face, time travel flags
- 50+ C headers in collection/, 12 wired end-to-end, 12 defined but unwired
- Build system: Makefile-based (runner/), tests with gcc/msvc

## Constraints

- **Model format**: GGUF with Q8_0/F32 tensors — must handle any architecture (SmolLM2, Qwen, Llama, etc.)
- **Data integrity**: Lossless reconstruction is non-negotiable — every capture must reconstruct bit-identical
- **Performance**: Target 10K+ t/s capture rate (current single-face baseline: 26K t/s)
- **Memory**: No process-wide scans, no SEH — VirtualQuery for memory safety only
- **Compatibility**: Must work with llama.cpp's existing tensor struct layout — no fork or patch needed

## Key Decisions

<!-- Decisions that constrain future work. Add throughout project lifecycle. -->

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| int64_t for vx/vy/resid in TW capture | sig × TW_SCALE (207360) exceeds int32 range for Q8_0 | ✓ Good |
| Q8_0 stored raw, no hex_tile | hex_tile EXPANDS Q8_0 (85% EDGE tiles) | ✓ Good |
| CPU backend first | Proven per-decode swap; GPU deferred | ✓ Good |
| 12-face bridge is next priority | Solves core disconnect: single face → full TRing 720 | — Pending |
| Model-agnostic GGUF reading | gguf_idx_open reads tensor names from any GGUF | ✓ Good |

## Current Milestone: v1.0 12-Face Bridge Pipeline

**Goal:** Build end-to-end pipeline from GGUF model → 12-face TW capture → TRing 720 → rewind buffer → wallet file, fully wired through collection + runner demo.

**Target features:**
- 12-face bridge — iterate TW capture across all 12 dodecahedron faces → TRing 720
- Map face+zone+slot → geo_frame_seek.h timeline position
- Connect to geo_rewind.h 972-slot buffer
- Wire wallet serialize → .pogwallet disk file
- Wire .gsten reader via gb_load()
- Runner integration: end-to-end demo on any GGUF model

## Evolution

This document evolves at phase transitions and milestone boundaries.

**After each phase transition** (via `/gsd-transition`):
1. Requirements invalidated? → Move to Out of Scope with reason
2. Requirements validated? → Move to Validated with phase reference
3. New requirements emerged? → Add to Active
4. Decisions to log? → Add to Key Decisions
5. "What This Is" still accurate? → Update if drifted

**After each milestone** (via `/gsd-complete-milestone`):
1. Full review of all sections
2. Core Value check — still the right priority?
3. Audit Out of Scope — reasons still valid?
4. Update Context with current state

---

*Last updated: 2026-06-16 after milestone v1.0 initialization*

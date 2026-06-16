# Requirements: FGLS (Geometric File & Library System)

**Defined:** 2026-06-16
**Core Value:** Lossless geometric capture and reconstruction of LLM tensor data — every tensor that goes in comes back out bit-identical, with a discoverable geometric addressing layer on top.

## v1 Requirements

Requirements for milestone v1.0. Each maps to roadmap phases.

### Multi-Face Capture

- [ ] **MFACE-01**: TW capture iterates across all 12 dodecahedron faces → complete TRing 720

### Timeline Integration

- [ ] **TIME-01**: Face+zone+slot maps to geo_frame_seek.h timeline position

### Runner Demo

- [ ] **DEMO-01**: End-to-end pipeline demo from runner decode → capture → store, model-agnostic

## v2 Requirements

Deferred to future milestone. Tracked but not in current roadmap.

### Timeline Integration

- **TIME-02**: geo_rewind.h 972-slot buffer connected to frame seek

### Wallet

- **WALL-01**: Wallet serialize writes .pogwallet disk file from bridge output

### Store Reader

- **STORE-01**: .gsten reader (gb_load/gb_decode_*) wired to production caller

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Real-time visualization | Geopixel/hex tile rendering of tensor geometry is separate from capture pipeline |
| GPU tensor capture | CPU backend proven; GPU (Vulkan/CUDA) tensor discovery deferred |
| Compression optimization | Q8_0 at entropy limit; hex_tile expands data, ZSTD saves only 4% |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| MFACE-01 | Phase 1 | Pending |
| TIME-01 | Phase 2 | Pending |
| DEMO-01 | Phase 3 | Pending |

**Coverage:**
- v1 requirements: 3 total
- Mapped to phases: 3
- Unmapped: 0 ✓

---

*Requirements defined: 2026-06-16*
*Last updated: 2026-06-16 after initial definition*

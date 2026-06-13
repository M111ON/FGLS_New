# Triangle Wheel Specsheet

## Single Edge → All Geometry

Base edge (1 เส้น) + 60° → chain equilateral Δ → 6 Δ = virtual hexagon. 36° core for pentagon. No absolute coordinates needed — angle rules only.

## 3-Shell Architecture

| Shell | Name | Mechanism | Access |
|-------|------|-----------|--------|
| **1** | Sector + Slot | cross-product sign test → nearest centroid | Active routing |
| **2** | Residual | int64 diff: v - slot_centroid | Position within slot, readable |
| **3** | Frozen / Time-stopped | drain (boundary drop), high-entropy cutoff | POGLS address only, no mutation |

## Shell 1: Sector + Slot (Active)

- **10 sectors** (pentagon-pair, 36° each) — boundary direction sign test
- **6 slots per sector** (hex-cast, 60° each) = 60 total centroids
- SCALE = 12^5 = 248832 (dodecahedron-aligned fixed-point)
- Integer cross-product: `cross(boundary, v)` sign → O(10) tests, zero float
- No atan2, no trig, no approximate

## Shell 2: Residual (Metadata)

- `resid_x, resid_y` = exact int64: `v - slot_centroid`
- Lossless reconstruction confirmed: capture → `tw_reconstruct_int` = exact roundtrip
- Carries high-precision identity even when multiple tensors share same zone+slot
- Not "lost" — just stored as diff from nearest centroid

## Shell 3: Frozen (P5H Barrier Integration)

- Drain = near sector boundary (`|cross| < margin`, ~0.5°) → dual activation
- But what actually FALLS INTO shell 3:
  - High-entropy data (residual exceeds threshold)
  - Data too complex for current sector/slot resolution
  - Dropped boundary crossings
- Mechanism: P5H flower window (10-phase) + barrier (tick 12)
  - tick 1-10: data streams, active capture
  - tick 11: inactive (retains)
  - tick 12: barrier freeze → frozen data assigned POGLS address → wallet
- No mutation after freeze. Address = permanent reference.

## Test Results (10 simulated tensors)

| Metric | Goldberg GT2 | TW Capture |
|--------|-------------|------------|
| Avg shift | 0.3550 | N/A (no blend) |
| Coverage | 12 faces / 12 | 1 zone, 4 slots, 10 unique residuals |
| Reconstruction | Lossy (weighted avg) | Exact (int64 roundtrip) |
| Float ops | Y3 encode, cos/sin for rotation | None — pure int32/int64 |

## Key Properties

- **Lossless**: reconstruction exact — no quantization, no approximation
- **Deterministic**: same (vx,vy) → same (zone,slot,resid) always
- **Data-driven**: tensor "drags" to its position — no encoder weight tuning
- **Hierarchical**: coarse (sector) → fine (slot) → exact (residual)
- **Integer-only**: zero float at runtime (even sector directions are precomputed ints)
- **Selective**: specific triangles activate where data lands — not all 70 blended

## Implemented Files

| File | Status | Role |
|------|--------|------|
| `ctd_triwheel.h` | Prototype | Float version, centroid select (ทำเอง, deprecate) |
| `tw_capture_int.h` | ✅ Production-ready | Integer-only, hierarchical, lossless capture |
| `tests/test_tw_capture.c` | ✅ Verified | 10 tensors, reconstruction exact |
| `test_triwheel.c` | ✅ Built | TW vs GT2 comparison |

## Files That Need Update

| File | Change |
|------|--------|
| `lc_tantrix.h` | Wire Shell 1 capture as routing input |
| `p5h_ribcage.h` | Shell 3 freeze mechanism → address assignment |
| `fibo_spine.h` | P5H integration point for shell 3 |
| `face_scan.py` | Point to tw_capture_int.h instead of encoder weight tuning |

## Next

- Shell 3: what triggers drain → freeze? entropy threshold specification
- Wallet format for frozen data: POGLS address + residual + timestamp
- Multi-layer resolution: what if 60 slots aren't enough? sub-slot at finer SCALE

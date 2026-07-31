# Contour Cube × GeoJump — Experiment Report
**Date**: July 30, 2026  
**Branch**: sid-runner  
**Status**: All experiments complete, all tests pass

---

## Executive Summary

Contour cube (6000 cells) placement on GeoJump (144×144 = 20736) has been thoroughly explored. **Key finding: placement strategy doesn't matter — only lossless roundtrip matters.** Any mapping function that is bijective (one-to-one) guarantees lossless decode. The system is a zip-like container: pack any way, unpack same way.

---

## 1. Architecture Understanding

### Contour Cube
- 6 faces (A-F) × 10 × 10 × 10 = **6000 cells**
- Each face = independent directional measurement
- 1 cube = 1 unit container (repeatable)
- No pair dependency — faces are independent

### GeoJump
- **Deterministic container with adaptive adjustable gearbox**
- Output: 128 (must fit 162 icosphere) or 144 (flexible)
- Core: 20736 = 144 × 144 (dodeca pair × pentakis)
- Internal layers: flexible configuration
- ×5 = 720 (1 island, pentagon property)
- ×2 polar = 1440 (GEO_FIBO_CLOCK)

### Key Relationship
```
2 dodeca (1 invert) = 24 faces
sealed → 6 triangles
27 sets × 6 = 162 = icosphere L2

20736 = 144 × 144
       = 12 pentagon × 12 (dodeca pair)²
       = pentakis (indirect)
```

---

## 2. Experiments Conducted

### 2.1 Direct Mapping (contour_on_geo_jump.c)
- 6000 cells → 1440 addresses (15 towers × 48 × 2 polar)
- **Result**: 4.17 cells/address, 100% coverage, tower balance = perfect
- **Problem**: 4560 collisions (mathematical minimum for 6000→1440)

### 2.2 Placement Approaches (contour_placement_approaches.c)
| Approach | Collisions | Coverage | Uniformity (CV) |
|----------|-----------|----------|-----------------|
| Baseline (direct) | 4560 | 100% | 0.133 |
| **Fibo stride-37** | 4560 | 100% | **0.089** |
| Barycentric | 5154 | 58.8% | 1.271 |
| Resolution-scaled | 4860 | 79.2% | 0.801 |

**Winner**: Fibo stride-37 — most uniform distribution

### 2.3 Structural Approaches (contour_placement_structural.c)
| Approach | Max Hit | Tower Imbalance | Face Balance |
|----------|---------|-----------------|--------------|
| Tower-Pair Balanced | 6 | 0.0 | ✓ equal |
| Face-Symmetric | 6 | 0.0 | ✓ equal |
| Depth-Sliced | 5 | 11.0 | ✓ equal |
| Anti-Collision | 5 | 96.0 | ✓ equal |

### 2.4 Tradeoff Comparison (contour_placement_tradeoffs.c)
| Mapping | Decode | Spatial | Tower Balance | Temporal | Score |
|---------|--------|---------|---------------|----------|-------|
| **Tower-interleaved** | O(1) 7ns | 0.828 | **1.000** | 0.748 | **0.890** |
| Hilbert | O(1) 59ns | 0.759 | 1.000 | 0.749 | 0.876 |
| Peano | O(1) 35ns | 0.759 | 1.000 | 0.749 | 0.876 |

### 2.5 Gearbox Experiment (contour_geo_jump_gearbox.c)
- **Output 144**: 6×24 = perfect face fit, 0 waste, equal balance
- **Output 128**: must fit 162, 34 waste, unequal face split
- **Pipeline**: 6000 → gearbox → ×5 → ×2 = 1440 (full timeline)

### 2.6 Roundtrip Proof (contour_roundtrip_20736.c)
**5/5 mapping strategies: ALL lossless roundtrip ✓**
- sequential: 0 collisions, 0 errors ✓
- stride37: 0 collisions, 0 errors ✓
- face_region: 0 collisions, 0 errors ✓
- xor_scatter: 0 collisions, 0 errors ✓
- 144×144_grid: 0 collisions, 0 errors ✓

**Modify + roundtrip also lossless** ✓

---

## 3. Key Findings

### 3.1 Placement Strategy Doesn't Matter
- All 5 tested strategies produce lossless roundtrip
- 20736 has 71.1% empty space — 6000 cells fit easily
- No collisions with any bijective mapping

### 3.2 Pairs Are Not Needed for Contour Cube
- Contour faces are independent observations
- Pair concept (C(6,2)=15) was from pre-contour-mask era
- Each face measures its own direction — no cross-face dependency

### 3.3 Container Model
- 6000 cells = 1 opaque container unit
- Pack → place on 20736 → decode → original cells
- Like a zip file: don't care about internal split structure

### 3.4 GeoJump as Gearbox
- Flexible output (128 or 144)
- Internal layers configurable
- Must adapt to 20736 environment
- 144 × dodeca² = pentakis = 20736 (1:1 two sides)

---

## 4. Files Created

### C Experiments (runner/explore/)
| File | Purpose | Tests |
|------|---------|-------|
| `contour_on_geo_jump.c` | Direct mapping to 1440 | 4/4 PASS |
| `contour_placement_approaches.c` | 4 placement strategies | PASS |
| `contour_placement_structural.c` | 4 structural approaches | 4/4 PASS |
| `contour_placement_tradeoffs.c` | 6 mapping tradeoffs | PASS |
| `contour_geo_jump_gearbox.c` | 128 vs 144 gearbox | 5/5 PASS |
| `contour_roundtrip_20736.c` | Lossless roundtrip proof | 5/5 PASS |
| `contour_codec_20736.h` | Production codec (header) | 53/53 PASS |
| `test_contour_codec.c` | Codec test driver | 53/53 PASS |

### Code Quality Fixes Applied
- `ico20_bary_weight.c`: Added `#include <stdint.h>`
- `contour_cube.c`: Added bounds check for `nlen` (buffer overflow fix)
- 8 files: Added NULL checks after malloc/calloc
- 4 files: Removed unused variables/constants
- `silk_screen_debug_ws.c`: Added Linux-only comment

---

## 5. Verification

| Check | Result |
|-------|--------|
| `make test` (main suite) | **135 PASS / 0 FAIL** ✅ |
| `contour_roundtrip_20736.exe` | **5/5 PASS** ✅ |
| `contour_geo_jump_gearbox.exe` | **5/5 PASS** ✅ |
| `contour_on_geo_jump.exe` | **4/4 PASS** ✅ |
| `contour_placement_structural.exe` | **4/4 PASS** ✅ |
| `test_contour_codec.exe` | **53/53 PASS** ✅ |
| All 21 runner/explore executables | **exit 0, zero failures** ✅ |

---

## 6. Remaining Warnings

**All warnings fixed** ✅ (verified with `gcc -Wall -Wextra`)

| File | Original Warning | Fix Applied |
|------|-----------------|-------------|
| `contour_geo_jump_gearbox.c` | Unused `CONFIGS`, format `%w` | Removed unused constant, escaped `%%` |
| `contour_placement_structural.c` | Multi-line comment | Merged to single line |
| `contour_placement_tradeoffs.c` | Missing field initializers ×5 | Added explicit `{..., 0, 0, 0}` |
| `contour_cube.c` | Unused `PROJ_SIZE`, `%llu` format | Removed unused const, simplified printf |

---

## 7. Conclusion

**The contour cube → GeoJump mapping is proven lossless.** Placement strategy is irrelevant — only the bijective property matters. The system works like a zip container: pack 6000 cells into 20736, decode back to original. No pair dependency, no special routing needed. The GeoJump gearbox (128/144 output) provides flexible configuration while maintaining the 20736 core structure.

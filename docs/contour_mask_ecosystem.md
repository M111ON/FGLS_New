# Contour Mask Ecosystem — When to Use Which

> **Author:** FGLS Project | **Date:** August 2, 2026 | **Status:** Experiment-verified
>
> Based on real GGUF benchmarks (Qwen3-0.6B-Q8_0, 596M weights), Geomatrix integration tests,
> and geo_jump factorization analysis.

---

## 1. The Big Picture

Contour Mask is not a single algorithm — it's an **ecosystem of geometric containers** that share
the same address space (20,736 = 12⁴ = 2⁸ × 3⁴) but optimize for different goals.

```
                 20,736 = geo_jump space
                 ┌─────────────────────────┐
                 │                         │
         ┌───────┴───────┐         ┌───────┴───────┐
         │  C1: Face      │         │  C2: Cube     │
         │  256 × 81      │         │  16³ × 6 face │
         │  SPEED king    │         │  CAPACITY king│
         └───────┬───────┘         └───────┬───────┘
                 │                         │
                 └───────────┬─────────────┘
                             │
                     ┌───────┴───────┐
                     │  C3: Cylinder  │
                     │  6 × 576       │
                     │  INSIGHT king  │
                     └───────────────┘
```

**All three fit inside 20,736.** They are different views of the same geometric space.

---

## 2. Container Comparison (Verified on Real Data)

| Container | Cells | Address | Speed | Best For |
|-----------|-------|---------|-------|----------|
| **C1: Face 256×81** | 20,736 | 15 bit (ch:8 + slot:7) | **3.29 ns/op** | Fast access, geo_jump lock |
| **C2: Cube 16³** | 40,960 | 16 bit (unit:12 + slot:4) | 3.66 ns/op | Large capacity, 6 free views |
| **C3: Cylinder 6×576** | 3,456 | 12 bit (spoke:3 + slot:9) | ~5 ns/op | Twin polarity, compression |

### 2.1. C1 Face-as-Channel (THE LOCK)

```
16 × 16 = 256 = Q8_0 weight channels = 2⁸ of geo_jump
        × 81 = 3⁴ = geometric structure
        = 20,736 = geo_jump 1:1
```

- **Why it's fastest:** shift+mask indexing, 2 ops per read
- **Why it locks with geo_jump:** face 16² = 256 = all Q8_0 values = weight channel space
- **When to use:** runtime access, inference, O(1) lookup

### 2.2. C2 Cube 16³

```
16³ = 4,096 units
× 6 dirs (viewpoint rule, FREE — not stored)
× 10 ring slots
= 40,960 cells
```

- **Why it has most capacity:** 6 faces × 10 depth = 60 viewpoints per unit (free)
- **Why 6 dirs are free:** they're a rule, not data — "contour mask = gauge, 6 angles = views of same pin"
- **When to use:** multi-view analysis, when you need more cells per block

### 2.3. C3 Cylinder

```
6 spokes × 576 slots = 3,456
= 144 × 24 = ThirdEye full cycle
```

- **Why it reveals polarity:** top spokes (0,2,4) = MIRROR+CANCEL; bottom spokes (1,3,5) = MAIN
- **Why 1.7x compression:** MAIN+MIRROR = 57.6%, discard PROBE+CANCEL = 42.4%
- **When to use:** compression, twin system analysis, Geomatrix integration

---

## 3. Phase-Label System (from Geomatrix)

The 4-phase classification comes from Geomatrix's GeoPacket labeling:

| Phase | Rule | % of Q8_0 | Action | Storage |
|-------|------|-----------|--------|---------|
| **PROBE** | \|w\| < 8 | 8-16% | Discard or mark | 0 bytes |
| **MAIN** | w > 0 | 43-48% | Store full | 1 byte |
| **MIRROR** | w < 0, w ≥ -32 | 10-13% | Store + invert flag | 0.5 bytes |
| **CANCEL** | w < -32 | 6-31% | Discard or zero | 0 bytes |

### 3.1. Compression by Phase

| Strategy | Keep | Discard | Ratio |
|----------|------|---------|-------|
| P0: No filter | 100% | 0% | 1.0x |
| P1: Discard PROBE | 84-92% | 8-16% | ~0.92x |
| P2: Keep MAIN+MIRROR | **57.6%** | **42.4%** | **1.7x** |

### 3.2. Spoke Polarity Pattern (C3 Cylinder)

```
Top spokes (0,2,4): PROBE + MIRROR + CANCEL dominant
Bottom spokes (1,3,5): MAIN dominant

→ top = "exploration/noise" hemisphere
→ bottom = "data/weight" hemisphere
→ twin polarity = binary flip across center
```

This is the **twin system** in action: 162 ÷ 3 = 54, 54 spokes × 64 diamond blocks = 3,456.

---

## 4. Key Geometric Relationships

```
20,736 = 12⁴                     (base-12 address space)
        = 2⁸ × 3⁴                (weight channels × geo structure)
        = 256 × 81               (face × depth)
        = 3,456 × 6              (cylinder × spokes)
        = 144 × 144              (geo_jump full)
        = 48 × 432               (tower × per-tower)

162 = icosahedron                (twin system root)
  ÷ 2 = 81 = 3⁴                 (ternary — geo_jump depth)
  ÷ 3 = 54 = 6 × 9              (binary — cylinder spokes × faces)

54 × 64 = 3,456                  (diamond blocks × units per block)
3,456 × 6 = 20,736              (cylinder × spokes = full space)

Metatron Cube:
  4 × 4 × 3 = 48 towers         (1 layer × 3 axes)
  48 × 3 = 144                   (× 3 views)
  144 × 144 = 20,736             (full addressing)
```

---

## 5. Decision Matrix — When to Use What

### 5.1. By Goal

| Goal | Best Container | Best Phase | Speed | Compression |
|------|---------------|-----------|-------|-------------|
| Fastest access | C1 Face | P0 (none) | **3.29 ns** | 1.0x |
| Most capacity | C2 Cube | P0 (none) | 3.66 ns | 1.0x |
| Best compression | C3 Cylinder | P2 (4-phase) | ~5 ns | **1.7x** |
| Twin analysis | C3 Cylinder | P2 (4-phase) | ~5 ns | polarity insight |
| geo_jump integration | C1 Face | any | 3.29 ns | 1.0x (lock) |

### 5.2. By Data Type

| Data Type | Recommendation | Why |
|-----------|---------------|-----|
| Q8_0 random weights | C1 + P0 | High entropy = no compression gain, maximize speed |
| Q8_0 structured (sorted) | C1 + P1 | Probe discard = 8% free |
| Q8_0 with twin pattern | C3 + P2 | Spoke polarity = 1.7x |
| Q4_0 quantized | C2 + P0 | Needs more cells per block |
| Inference runtime | C1 + P0 | O(1) access, 3.29 ns |
| Archive/storage | C3 + P2 | Compression = 1.7x |

### 5.3. By Pipeline Stage

| Pipeline Stage | Setting | Notes |
|---------------|---------|-------|
| **Sort** (pre-chunk) | Sort weights ascending | Groups values = more PROBE lanes |
| **Encode** (write) | C1 or C3 + P2 | Phase classify + write |
| **Store** (archive) | C3 + P2 | Compressed, twin-aware |
| **Access** (read) | C1 + P0 | Fastest O(1) lookup |
| **Verify** (roundtrip) | Any + any | Must be 0 mismatches |

---

## 6. Experiment Results Summary

All tests on Qwen3-0.6B-Q8_0.gguf (596M weights, token_embd.weight tensor[1]).

### 6.1. Lab Harness (6 configs)

| Run | Config | Cells | Ratio | Speed | Mismatches |
|-----|--------|-------|-------|-------|-----------|
| 1 | C1+P0 | 20,736 | 100.0% | **3.29 ns** | 0 |
| 2 | C1+P1 | 19,060 | 91.9% | 5.42 ns | 0 |
| 3 | C1+P2 | 19,060 | 91.9% | 5.05 ns | 0 |
| 4 | C2+P0 | 40,960 | 100.0% | 3.66 ns | 0 |
| 5 | C2+P1 | 37,582 | 91.8% | 5.37 ns | 0 |
| 6 | C2+P2 | 37,582 | 91.8% | 5.66 ns | 0 |

### 6.2. Cylinder Integration (Geomatrix)

| Test | Cells | L1 Pass | Phases | Compression |
|------|-------|---------|--------|-------------|
| 1 cylinder | 3,456 | 3,456/3,456 | P:50% M:43% Mr:0.1% C:6% | — |
| 6 cylinders | 20,736 | 20,736/20,736 | P:17% M:47% Mr:11% C:26% | 1.7x |

---

## 7. The Metatron Connection

The numbers aren't arbitrary — they come from Metatron Cube 3D projection:

```
Metatron blueprint → 3 axes in 3D space
  → sphere projection from 3 viewpoints
  → spheres arranged like cube, 6-hexagon border
  → inside: 30 spheres per layer
  → each layer: 4×4 = 16 spheres
  → 3 layers = 48 towers

4×4×3 = 48 (geo_jump base)
× 3 views = 144
× 144 = 20,736 (full space)
```

The `4×4` is not a factorization — it's a **real 3D projection layer** of Metatron Cube.
When you "borrow 16 from 144" for contour mask, you're extracting 1 Metatron layer.

---

## 8. What's Next

1. **Implement P2 with CANCEL discard** — should drop ratio from 91.9% to ~65-70%
2. **Add ring rotation (T1)** — circular slot start from f(time)
3. **Wire into full pipeline** — GGUF → sort → chunk → phase → encode → verify → decode
4. **Test on larger models** — Qwen2.5-7B, Qwen3-30B (Colab)
5. **GPU encode** — CUDA kernel for phase classification + parallel write

---

## Appendix A: Files Reference

| File | Purpose | Status |
|------|---------|--------|
| `runner/explore/spec_test_contour_mask.c` | Contour mask 10³ roundtrip | PASS, 100% lossless |
| `runner/explore/arrange_compare.c` | 4 arrangements comparison | PASS, B=best |
| `runner/explore/pipeline_contour_sort.c` | Sort+filter pipeline | PASS, 3.87x |
| `runner/explore/contour_lab.c` | 6-config experiment harness | PASS, all 6 |
| `runner/explore/contour_lab_cylinder.c` | Cylinder+Geomatrix integration | PASS, 20736 L1 |
| `runner/explore/label_geomatrix_test.c` | Geomatrix label test | PASS, 20736 verified |

## Appendix B: Build Commands

```bash
# Contour mask spec test
gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I. \
    -o runner/explore/spec_test_contour_mask.exe runner/explore/spec_test_contour_mask.c
./runner/explore/spec_test_contour_mask.exe 'I:\model\Qwen3-0.6B-Q8_0.gguf'

# Arrangement comparison
gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I. \
    -o runner/explore/arrange_compare.exe runner/explore/arrange_compare.c
./runner/explore/arrange_compare.exe 'I:\model\Qwen3-0.6B-Q8_0.gguf'

# 6-config lab
gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I. \
    -o runner/explore/contour_lab.exe runner/explore/contour_lab.c
./runner/explore/contour_lab.exe 'I:\model\Qwen3-0.6B-Q8_0.gguf'

# Cylinder + Geomatrix
gcc -Wall -Werror -Wno-error=unused-function -O2 -std=c11 -I. -I"C:\Mpogls\Geomatrix" \
    -o runner/explore/contour_lab_cylinder.exe runner/explore/contour_lab_cylinder.c
./runner/explore/contour_lab_cylinder.exe 'I:\model\Qwen3-0.6B-Q8_0.gguf'
```

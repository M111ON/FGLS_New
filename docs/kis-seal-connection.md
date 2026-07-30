# Kis-Seal × Existing Codebase Connection
### Mapping the Recursive Addressing Blueprint to Current Implementation

**Date:** 2026-07-30
**Source:** Icosa-Dodeca Recursive Kis-Seal Report + beam_addressing/ codebase

---

## 1. Core Proof (from Kis-Seal Report)

The report proves numerically (error 1e-16):

| Property | Status | Evidence |
|----------|--------|----------|
| Center invariant | ✅ | Σnᵢ = 0 (20 faces) |
| Sealing sync | ✅ | h_seal = constant across all 12 vertices |
| Scale ratio | ✅ | R_dual/R_icosa = 1/φ² = 0.381966 (exact) |
| Generation clock | ✅ | R_n bijective, invertible via log |

**Key insight:** The recursive structure Iₙ₊₁ = T(Iₙ) is self-similar with fixed ratio 1/φ². Each generation is a scaled copy of the previous one.

---

## 2. Addressing Blueprint (from Kis-Seal Report)

```
Address = (n, k)
  n = generation index (from log(Rₙ)) — anti-aliasing across scales
  k = face/vertex ID within layer (0-19 for icosahedron, 0-11 for dodecahedron)

Key_new = (n, 3-char key) → weight → collapse → Hilbert wire (384-bit)
```

**Analogy:**
- n = exponent (floating point) — prevents address collision across scales
- k = mantissa/direction — identifies position within a layer

---

## 3. Existing Implementation (beam_addressing/)

### 3.1 geo_jump Structure (20736 nodes)

From `beam_hilbert_icosahedron.c`:
```c
#define GEO_FULL        20736u    // = 12 faces × 12 shells × 144 locals
#define GEO_PENTAGONS   12u       // dodecahedron faces
#define GEO_TOWER       144u      // addresses per tower
#define GEO_SHELL_TICK  12u       // shells per face
#define GEO_FIBO_CLOCK  1440u     // = 12 × 120 = 15 towers × 96?

// Tessellation: node = face × 1728 + shell × 144 + local
// 12 × 1728 = 20736
```

**This is ONE LAYER of the recursive structure** — generation n=0.

### 3.2 Hilbert × Icosahedron = 20736

From `beam_hilbert_icosahedron.c`:
```c
#define HILBERT_CELLS   64u     // 4×4×4 = 64 positions
#define ICOSAHEDRON_V   162u    // frequency-4 icosahedron vertices
#define ICOSAHEDRON_X2  2u      // × 2 for direction/sign

// Verify: (64 × 2) × 162 = 20736
```

**Mapping:**
- Weight → tessellation node (20736 grid)
- Tessellation node → Hilbert 4×4×4 position (64 positions)
- Hilbert position → icosahedron vertex (162 vertices)
- Icosahedron vertex → frame_seek tile (1440)

### 3.3 15 Towers = C(6,2) = 15 Pairs

From `geo_jump_x_silk.py`:
```python
TOWER_ADDR = 48        # addresses per tower
TOWERS_PER_ISLAND = 3  # 3 towers per island
FACES = 5              # LetterCube faces
POLARS = 2             # +polar, -polar

# 15 towers × 48 addr × 2 polar = 1440 addresses
# 15 towers = C(6,2) = 15 pairs (silk screen)
```

**Connection to Kis-Seal:**
- 15 towers = 15 pairs of icosahedron faces (C(6,2))
- Each tower = 48 addresses = 48 weight values per pair
- 2 polar = complementary views (+polar, -polar)

---

## 4. Mapping: Kis-Seal → Existing Code

### 4.1 Generation Index (n)

**Kis-Seal:** n = generation index from log(Rₙ)
**Existing code:** Not explicitly implemented

**Gap:** The existing 20736 grid is generation n=0. To support multiple generations, we need:
- Generation counter (n) as part of the address
- Scale factor (1/φ²)^n for each generation
- Anti-aliasing: same direction at different scales → different addresses

### 4.2 Face/Vertex ID (k)

**Kis-Seal:** k = 0-19 (icosahedron faces) or 0-11 (dodecahedron vertices)
**Existing code:** 
- `tess_face(node)` returns 0-11 (dodecahedron faces)
- `tess_shell(node)` returns 0-11 (shells per face)
- `tess_local(node)` returns 0-143 (local address within tower)

**Connection:** k = (face, shell, local) composite index

### 4.3 Sealing Height (h_seal)

**Kis-Seal:** h_seal = constant across all vertices, ratio 1/φ²
**Existing code:** Not explicitly used

**Potential use:**
- h_seal could determine resolution per generation
- Sealing = discretization (continuous → discrete)
- Connection to h-depth: open = h→0 (infinite), sealed = h→R (finite)

### 4.4 Recursive Structure

**Kis-Seal:** Iₙ₊₁ = T(Iₙ), self-similar ratio 1/φ²
**Existing code:** Single layer (n=0) with 20736 nodes

**Extension needed:**
- Multiple layers: generation 0, 1, 2, ...
- Each layer scaled by (1/φ²)^n
- Address format: (n, face, shell, local)

---

## 5. Unified Address Format

Combining Kis-Seal blueprint with existing code:

```
┌─────────────────────────────────────────────────────────────┐
│                    UNIFIED ADDRESS FORMAT                     │
├───────────────┬───────────────┬───────────────┬─────────────┤
│  n (generation)│ face (0-11)  │ shell (0-11)  │ local (0-143)│
│  = scale index │ = dodeca face│ = radial depth│ = tower pos  │
│  from log(Rₙ)  │ 12 faces     │ 12 shells     │ 144 locals   │
├───────────────┼───────────────┼───────────────┼─────────────┤
│  Anti-aliasing │ Layer ID      │ Resolution    │ Position     │
│  across scales │ within layer  │ within face   │ within shell │
└───────────────┴───────────────┴───────────────┴─────────────┘

Total capacity per generation: 12 × 12 × 144 = 20,736
Total capacity N generations: N × 20,736
```

**Bit allocation (example):**
- n: 8 bits (256 generations)
- face: 4 bits (16 faces, 12 used)
- shell: 4 bits (16 shells, 12 used)
- local: 8 bits (256 locals, 144 used)
- Total: 24 bits = 16M addresses (128 generations × 20736)

---

## 6. Connection to Silk Screen / Contour Mask

### 6.1 15 Towers = 15 Pairs

From `geo_jump_x_silk.py`:
- 15 towers (geo_jump) = 15 pairs (silk screen)
- 48 addr/tower = 48 weight values per pair
- 2 polar = +polar, -polar (complementary)

**Kis-Seal connection:**
- 15 pairs = C(6,2) combinations of 6 faces
- Each pair provides a different "view" of the same data
- All pairs independent (no cancellation)

### 6.2 Contour Mask Architecture

From `geometric-weight-storage` skill:
- 6 faces (A, B, C, D, E, F) = 6 read heads
- Filter = displacement (weight → position from 0)
- Lossless: XOR(pos, 0) = pos = weight
- Clock = simple counter (0, 1, 2, ...)

**Kis-Seal connection:**
- 6 faces of contour mask = 6 faces of icosahedron
- Each face provides independent view
- Recursive structure allows multiple scales (generations)

---

## 7. Gaps to Fill

### 7.1 Generation Counter (n)
- [ ] Add n field to address format
- [ ] Implement scale factor (1/φ²)^n
- [ ] Test bijectivity: n = log(Rₙ/R₀) / log(1+α)

### 7.2 Multi-Layer Support
- [ ] Extend 20736 grid to support N generations
- [ ] Implement layer selection based on n
- [ ] Test recursive structure: Iₙ₊₁ = T(Iₙ)

### 7.3 Sealing Height Integration
- [ ] Use h_seal to determine resolution per generation
- [ ] Connect to h-depth (variable resolution scaling)
- [ ] Test: open spike (h→0) vs sealed spike (h→R)

### 7.4 Geomatrix Keygen Extension
- [ ] Extend key format: (n, 3-char key)
- [ ] Test: weight → collapse → Hilbert wire (384-bit)
- [ ] Verify: no conflict with existing Rubik cube (384-bit) or Delta Lane (54 lanes)

---

## 8. Pitfall from Kis-Seal Report

**Hardcoding coordinates from separate conventions gives 40% correct, 60% wrong silently.**

From Section 5 of the report:
> ตอนแรก hardcode พิกัด dodecahedron แยกจากสูตรมาตรฐาน — ได้ผลตรงแค่ 8/20 หน้า อีก 12 หน้าคลาดเคลื่อน ~29°

**Lesson:** Always derive dual solid from face-normal of source solid directly (`dual_vertex = normal_i × scale`), not from hardcoded coordinates.

**Impact on POGLS:** If Geomatrix hardcodes icosa/dodeca coordinates separately, check orientation convention alignment. Silent misalignment = 40% correct, 60% wrong with no error message.

---

## 9. Next Steps

1. **Prototype (n, k) addressing** — extend beam_hilbert_icosahedron.c with generation counter
2. **Test recursive structure** — implement Iₙ₊₁ = T(Iₙ) in C
3. **Connect to contour mask** — use generation index for multi-scale observation
4. **Verify no conflicts** — ensure new addressing doesn't break existing 20736 grid

---

*Document created from Kis-Seal report analysis + beam_addressing/ codebase review*

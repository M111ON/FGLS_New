# Agent Task Delegation Plan: Cube Capture × Geo Jump × Kis Timeline
### Experiment Design — 5 Sections, 5 Agents (Parallel)

**Date:** 2026-07-31
**Status:** Design Phase — Structure Setup

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    LAYER 3: KIS-SEAL TIMELINE                      │
│  Recursive Icosa↔Dodeca, ratio 1/φ², sealed vertices             │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐                      │
│  │ n=0       │→│ n=1       │→│ n=2      │→...                    │
│  │ R=1.0    │  │ R=1.382  │  │ R=1.910  │                       │
│  │ h_seal   │  │ h_seal   │  │ h_seal   │                       │
│  │ =const   │  │ =const   │  │ =const   │                       │
│  └────┬─────┘   └────┬─────┘   └────┬─────┘                       │
│       │              │              │                            │
│  gap = residual space (version control, timeless)                │
├─────────────────────────────────────────────────────────────────┤
│                    LAYER 2: GEO_JUMP ROUTING                       │
│  20736 nodes: 12 × 12 × 144                                      │
│                                                                  │
│  15 towers = C(6,2) = 15 view pairs                              │
│  Jump types: Hilbert, Peano, Pentagon, Mod, Invert, Capo         │
│  48 addr/tower × 15 towers × 2 polar = 1440                     │
├─────────────────────────────────────────────────────────────────┤
│                    LAYER 1: CUBE WEIGHT CAPTURE                  │
│  10×10×10 × 6(12) faces → contour mask → displacement            │
│  XOR(weight, 0) = weight → LOSSLESS O(1)                         │
│                                                                  │
│  Faces: 4-8 pairs (6 faces) or 12 faces (dodeca)                │
│  Cube: 10×10×10 = 1000 cells × faces = 6000/12000               │
└─────────────────────────────────────────────────────────────────┘

         │              │            │
    Section 1      Section 5    Section 4
    Cube × Jump    Timeline     Residual
                   
         Section 2         Section 3
         12-face/var      Gen counter
```

---

## Section 1: 6-Dir Cube × Geo Jump Transformation

**Goal**: หาว่า 6-dir cube (A,B,C,D,E,F) สามารถ match กับ geo_jump tower structure ได้แบบไหน

**Questions to resolve**:
1. 6 dirs × 15 pairs (C(6,2)) = 90 combinatorial views × 144 slots = 12,960 slots — เกิน 1440 geo_jump addresses, เลือกยังไง?
2. Invert (D, E, F) = negative polar, map กับ geo_jump invert jump ได้ไหม?
3. Cylinder projection: 6 faces → 1 cylinder unwrap = 6 views (X,Y,Z,XY,ZX,YR) → 1440 timeline
4. Angular rotation: 0°, 60°, 120°, 180°, 240°, 300° = 6 rotations → × 1440 = 8640 possible readings
5. XYZ decomposition: A=X, C=Y, E=Z → 3-axis rotation, 6 readings per axis = 18 readings × 80

**Experiment tasks**:
- [ ] Task 1A: Map 6 dirs to geo_jump tower/pairs — which dir maps to which pair?
- [ ] Task 1B: How many unique views from 90 combinations actually needed? (information theory)
- [ ] Task 1C: Prototype cylinder projection: cube → normalized radius → rotation → angle
- [ ] Task 1D: Test angular rotation: 6 rotations × 3 moments = 18 per cube
- [ ] Task 1E: Implement invert matching: (A→A', B→B', C→C') vs geo_jump invert route

**Prototype file**: `experiments/section1_cube_geojump.py`
** Status**: Ready to assign

---

## Section 2: 12-face Dodeca × Variable Cube Size

**Goal**: ทดสอบ system เมื่อเปลี่ยน 6→12 faces, cube 10×10×10→N×N×N

**Questions to resolve**:
1. 12 faces → 12×12×12 = 192 12-sided directional splits → 330 views (C(12,2))
2. 1440 split across 12 faces = 120 per face — or 15 pairs = C(12,2) = 330 pairs
3. Cube 5×5×5 vs 50×50×50 = size_ratio 1000x — capacity vs resolution
4. 10×10×10 fix (48 tower) vs variable (¿) — when do 48 towers break?
5. 12-face = dodecahedron = 2× ico density compared to 6-face cube

**Experiment tasks**:
- [ ] Task 2A: 12-face directional capacity formula: C(12,2) = 330 pairs vs 1440 geo_jump — match?
- [ ] Task 2B: 5×5×5 cube: 125 cells × 12 = 1500 — how to map to 1440?
- [ ] Task 2C: 50×50×50 cube: 1250 cells × 12 = 15000 — ratio 10.4x over 1440 → capacity overflow strategies
- [ ] Task 2D: Flex point — cube size where(6→12 faces becomes beneficial or detrimental)
- [ ] Task 2E: Orthogonality test: 6-face cube projection vs 12-face dodeca projection

**Prototype**: `C:\section2_12face_variable_cube.py`
**Status**: TEST ASSIGN

---

## Section 3: Gen Counter as Multi-Role Timeline

**Goal**: Gen (n) ไม่ใช่แค่ generation — เป็นได้ทั้งรอบ, ความยาว, เฟรม, layer

**How many meanings can one counter have?**:
```
n = frame_id                     → 1 frame = 1 cube capture = static
n = cycle_id                     → 1 cycle = 1 pass of all towers (1440)
n = length_id                     → 1 length = circumference step (linear)
n = layer_id                     → 1 layer = layer depth (radial)
n = time_id                      → 1 time = 1 step = clock
n = version_id                    → 1 version = 1 generation = (Kis-Seal)
```

**Experiment tasks**:
- [ ] Task 3A: Implement multi-meaning counter: 1 counter, 6 interpretations
- [ ] Task 3B: Test each meaning independently — verify
- [ ] Task 3C: Multi-meaning overlap: where meanings intersect (amplify errors)
- [ ] Task 3D: Mnemonics for meaning — how to tell which meaning is active without extra bits

**Prototype**: `C:\section3_gen_counter_multi.py`
**Status**: TEST ASSIGN

---

## Section 4: Seal Height = Fixed + Residual Space

**Goal**: h_seal = constant, gap between shapes = residual/version control — no forced time

```
KIS SEAL STRUCTURE:
  Icosa (R=1.0)
   ├── h_seal = fixed (0.882113) = deterministic
   │
   ├── gap_positive = extra space outward
   ├── gap_negative = extra space inward
   │
   ├── residual space = gap_positive + gap_negative
   │   = version control buffer
   │   = no clock, no time — all parallel
   └── → Dodeca (R=0.3819)

EXPERIMENT:
  - Generate icosahedron vertices
  - Spike kis (20 dirs, fixed height)
  - Seal vertex: vertex disappears, face forms
  - Gap = difference between actual vs predicted
  - Gap is residual = version-controlled
    - Can be modified without changing base shape
    - Multiple versions can coexist
```

**Experiment tasks**:
- [ ] Task 4A: Measure gap exactly — numerical experiment on regular icosa
- [ ] Task 4B: Gap volume (inward + outward) → version capacity formula
- [ ] Task 4C: Store version bit in gap (modulation) — encode/decode
- [ ] Task 4D: Verify timeless: gap operations never touch clock/counter

**Prototype**: `C:\section4_seal_residual.py`
**Status**: TEST ASSIGN

---

## Section 5: START/END = INFINITY + Uni-Frame Representation

**Goal**: Start/end positions are undefined until measured — like ruler where 0,0 = " where you put it"

**Key insight**: No absolute 0,0 in space, only relative to measurement target.

**What this means**:
- Before measurement: position is undefined (∞, INF)
- After measurement: measurement creates (0,0) anchor point — ruler starts here
- Module pointer: (0,0) = anchor = start, (N,N) = anchor + N measurements
- Bijective mapping between ∞ and finite — classic infinite→finite (like Hilbert soport)

**Experiment tasks**:
- [ ] Task 5A: Implementation: ∞ anchor position → measure target → 0,0 relative → finite readings
- [ ] Task 5B: Reverse: finite reading → ∞ → realignment to different anchor
- [ ] Task 5C: Multiple anchors (threaded) — same weight can have multiple (0,0) positions
- [ ] Task 5D: Verify: no absolute coordinate, all relative — cycle through 3 anchors, all readings correct

**Prototype**: `C:\section5_infinite_anchor.py`
**Status**: TEST ASSIGN (combined into Section 5)

---

## Section 6: Cube on Kis Timeline → Weight Random Access

**Goal**: ว่า cube contours วางบน kis timeline เป็นขั้นตอน — ใช้ f(time) ดึงน้ำหนัก

**How to implement**:
```
KIS TIMELINE (ตีศอก):
  Timeline = sequence of steps (n)
  Each (n, k) = 1 cube capture

  Cube sits at step position:
    Cube[n][k] = (face,  x, y, z) = weight
    
    f(time):
      n = get_n_from_time(t)
      k = get_k_from_n_and_offset(n, off)
      weight = cube[n][k].displacement
      return weight (XOR = 0 = weight)
    
    Decoding (fast random access):
      weights[i] = f(i * step)
```

**Experiment tasks**:
- [ ] Task 6.1: Implement — cube encodes at each kis step
- [ ] Task 6.2: Implement random access reader — f(time) → cube position
- [ ] Task 6.3: Test: random vs sequential reads — 1440 accesses, 0 errors
- [ ] Task 6.4: Matricized read — use array instead of 1-by-1 — mer cerule

**Actual integrated prototype**: `C:\experiments\cube_on_kis.py`
**Status**: NEEDS CODING

---

## HOW TO EXECUTE— AGENT MAP

| Section | Title | Quick Check | Full Build | Agent |
|---------|-------|-------------|----------|--------|
| 1 | Cube x Geo Jump | map 3 meas. | Implement mapping + 1440 | Agent A |
| 2 | 12-face Variable Cube | Write 3 configs | Implement flexible architecture | Agent B |
| 3 | Gen Counter Multi-role | Prototype 3 meanings | Implement 5-meaning counter | Agent C |
| 4 | Seal Height + Residual | Numerical measurement | Version encoding/decoding | Agent D |
| 5 | Endpoint ∞ (Cube V5) | Anchor test | f(time) access | Agent E |
| 6 | Cube on Kis Timeline | Distribution | Full integration | Agent F(overlaps) |

---

**Finned Work**: 5 independent experiment sections
**Next Step**: Select 1 to implement, or split agents simultaneously — **user should tell me what to assign.**

---

## SYSTEM DESIGN: Integrating Cube × Kis Timeline as One Function

เมื่อ execute ทั้ง 5 sections จบ เราจะมี:
- Section 1 → Cube go throu geo_jump/angular mapping
- Section 2 → 12-face variable cube architecture
- Section 3 → Gen (n) multi-meaning counter
- Section 4 → Seal = fixed + residual space (no time)
- Section 5 →∞ entry/exit = relative measurement
- Section 6 → Full kitchen: cube on kis timeline → weights via f(time)

------
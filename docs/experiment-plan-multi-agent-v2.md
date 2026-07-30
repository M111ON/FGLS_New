# Agent Task Delegation Plan — Revised (2026-08-01)
### Architecture: Cube Weight Capture on Kis Timeline as Ribcage Store

**Revision:** 2026-08-01 (original: 2026-07-31)
**Change:** User clarified 3 key architecture principles that change the design

---

## Architecture Principle

```
FISHBONE (Kis-Seal) = structure before universe (timeline, scale)
    │
    ▼
RIBCAGE = (our cube data, stored at each rib) = address store
    │
    ▼
POINTER = access mechanism to ribcage data = random access
```

**Kis-Seal is the Fishbone** — contains structure: Icosa↔Dodeca recursion, time, scale
**We are the Ribcage** — contains what the fishbone form: weight data × cubes × addresses
**Pointers access through Ribcage** — O(1) random access to any rib at any time

**Reference systems** (all exist in codebase):
- `FiboSpine` — 1728 pipes × 12 ticks → ribcage timing
- `P5H Ribcage` — 10-phase window per pipe, barrier sync
- `Bermuda Shadow` = residual space (HOT/COLD classify, shadow ring 144)
- `GearLock` — CPU↔GPU passoff at rib junctions
- `Jet Bridge` — cross-rib data transfer at tick 11
- `Rail Sync` — barrier synchronization at P12

---

## Design Refinement Based on 3 Clarifications

### 1. Gen (n) = Free Unit — like cm, mm, kg, Mb

Gen (n) is NOT a special index. It's a flexible measurement unit:

```
n = 0          → 1 step on timeline
n = 0 .. 11    → 1 frame (12 steps)
n = 0 .. 143   → 1 chunk (144 steps) 
n = 0 .. 719   → 1 layer (720 steps)
n = 0 .. 20735 → 1 cycle (20736 steps)
n = 0 .. N     → any unit (any interval)
```

**Implication**: The counter itself is neutral — the interpretation is what matters.

**Question for implementation**: How do we tag which meaning is active? 
- Option A: Counter + mode flag (1 byte) to indicate unit
- Option B: Counter + interval config (start, end, periodicity)

### 2. h_seal ↔ "Residual Space" (Bermuda Hardware Already Exists)

h_seal = fixed constant (1/φ²). Gap between sealed surfaces = **residual space = Bermuda system**.

Residual = bermuda, shadow_delta, temporal already in code:

```
bermuda_shadow.h:  HOT/COLD classifier → shadow ring (144 slots)
shadow_zone.h:     ShadowZone structure with bond_key, tick, temperature
p5h_ribcage.h:     10-phase pipe room (alternating inner/outer edge routing)
bermuda_export.h:  Geometry traverse, gear snap, Hilbert bijection
geom_shadow_pipe.h: Pipeline glue — shadow classify + tile decode
```

**Residual = version control space = timeless**: gap doesn't age, doesn't tick.

**Implication**: When Cube data is projected onto Dodecahedron, the "gaps" between polygon faces are NOT voids — they ARE the Bermuda shadow domain. This IS the residual/version control space.

### 3. Fishbone + Ribcage Architecture

The **system is a spine (ribcage) of cubes placed on the fishbone (Kis-Seal framework)**:

```
FISHBONE (Kis-Seal):
  Icosa(n) → spiked → Dodeca(n+1) → spiked → Icosa(n+2) → ...
  Scale: 1/φ² = constant
  Time: n in ℤ (relative, no origin)
  Shape: fixed by geometry — not adjustable

RIBUCAGE (weight cube data × addresses):
  At each rib (n, k):
    Cube face × time = weight displacement
    Address = (n, k) = access key
    
  Access pattern:
    → Linear: step n, k=0..20 (icosa faces)
    → Rotational: angular projection (cylinder→rotation→access)
    → F(time): random access at any rib
```

**Implication**: TheCube and Fishbone are NOT the same system — the storage is RIBUCAGE, the cross-rib pattern is FiboSpine.

---

## Can the Existing 20736 Grid Serve as Ribcage?

**YES.** The existing 20736 grid (1728 pipes × 12 ticks) is already structured as a ribcage:

```
CURRENT P5H / FIBOSPINE GRID:
  Pipe 0:   tick 0─1─2─3─4─5─6─7─8─9─10─11    [cycle 12]
            │  RIB  │  rib rib rib                                  │
  Pipe 1:   │ axis  │ ...............                              │
            │ adj    ├──────────────∣                              │
  Pipe 2:   │ r 0    │ RIB 1 ribs  |  RIB 2 ribs                  │
            │        │              │                               │
  ...
  Pipe 1727: tick .... ribs all distributed
```

**Architecture**: Each pipe = single rib, each tick = rib state

**RIBCAGE = FIBOSPINE:**
- Pipe (0..1727) = Rib ID
- Tick (0..11) = Rib position
- Cycle_RCUTION_BACK = 12-step cycle per rib

---

## Revised Agent Tasks (Ribcage Approach)

### Section A: Gen(n) Interpretation Layer (Free Unit)

**Task Context**: Write gen(n) interpretation layer that accepts a counter nibble + mode selector

**Key observations**:
- The counter is multiplied by the interpretation — no meaning is stored, only when needed
- Mode mapping: (0=step, 1=frame, 2=cycle, 3=layer, 4=full)

**Implementation approach**:
1. struct GenInterpret { interval, stride, mode, origin }
2. gen_interpret(n, mode): transform raw n into unit
3. Verify all modes produce unique intervals (0 bijection)

---

### Task B: H_Seal Residual Space Integration

**Status**: NOT a new feature — Bermuda system already implements residual/version control.

**Action**: Connect existing residual space (Bermuda) to scale ratio:

```
H Seal (a) = fixed = 1/(phi)^2 = 0.381966
Gap = Icosa_radius - Dodeca_radius = 0.618034

Gap (0.618034 unity) = Bermuda shadow ring capacity
  → ShadowPtr = gap × pyramid_factor
     = 0.618 * 10^6 ≈ 618k shadow slots
  → Matches rocp 144 (144*12=1728) pattern
```

**Why there's no anger**: The gap = `1 - 0.3819` = `0.618` which is `1/φ`. Compact: 7-lobed, gap 12% × 144 = exploit 17 shadow tamp per unit.

---

### Task: RIB-X: Access Methods for Cube Data on Ribcage

Now each rib is a cubeStore (Cubic weight array), we need access:

1. **Linear access**: step forward one rib at a time (rib0 → rib1 → ...)
2. **Sequential access**: along x cosine projection: cube → angle → rib index  
3. **(Rotary access**: Hilbert projection → mini rotation → rib ID
4. **Random access**: f(time, ribs) → cube_index

**Implementation question**: Max rib access rate? (latency vs bandwidth balancer)

---

## Implement Plan — 4 Agents

```
Agent A:  Gen ZERO counter — test free unit protocol
          Files: experiments/gen_zero_counter.py
          Deliverables: unit management + mode-moded test report

Agent B:  residual_space_integration.md
          Files: existing code → report only (already in code)
          Deliverables: how to connect golf_brain to residual + which files

Agent C:  Access method array → linear, sequential, rotatory, random
          Files: experiments/rib_cube_access.py
          Deliverables: perf metrics comparison (both vectors)

Agent D:  Architecture overview write-up (FISH=>+ RIBGATE → POINTERS)
          Files: docs/architecture_fishbone_ribcage_2026.md
          Deliverables: master map for all agents to consult
```

---

## States (updated)

- **Current**: RFarom; architecture planning phase; replittulating for cube on fishbone
- **Next**: 4 parallel agents execute (25 min estimated)
- **Then**: evaluation of each access strategy for cubec data → select best CD

---

End of plan (revised). Agents: A(gen zero), B(residual), C(access), D(architecture)
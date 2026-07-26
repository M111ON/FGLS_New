# Geometric Beam Encoding — System Architecture

## Core Concept

**Weight = Position in geometric field. Beam length = distance from center.**

```
weight = position - center
beam_length = |position - center| = |weight|
sign = (position > center) ? + : -
```

No metadata. No lookup tables. No hash. Just geometry.

---

## 1. The Field

The field is a 1D line mapped to a 2D Icosahedron structure.

```
Field size: 20736 slots (12⁴ = full cycle)
Center:     10368 (zero point)
Ceiling:    20735 (max positive)
Floor:      0 (max negative)

     CEILING (20735)
          │
          │  ← positive weights
          │
     CENTER (10368) ← zero
          │
          │  ← negative weights
          │
     FLOOR (0)
```

**Key insight:** The field size is adjustable. 20736 is the full cycle for base-12 computation. For storage, we only need enough slots for the weight range.

---

## 2. Weight ↔ Position Mapping

### Encoding (weight → position)

```c
position = CENTER + weight

// Q8 weights (-128..+127):
// position range: 10240..10495
// only 256 slots used out of 20736
```

### Decoding (position → weight)

```c
weight = position - CENTER

// If position > CENTER → positive (beam hits ceiling)
// If position < CENTER → negative (beam hits floor)
// If position = CENTER → zero
```

### Beam Length

```c
beam_length = |position - CENTER| = |weight|

// No need to store beam_length!
// It's implicit in the position.
```

### Sign

```c
sign = (position > CENTER) ? +1 : -1

// No need to store sign!
// It's implicit in the direction from center.
```

---

## 3. Storage vs Runtime

### Two Separate Concerns

| Concern | What | How |
|---------|------|-----|
| **Storage** | What to save to disk | Coordinate only |
| **Runtime** | How to compute | 20736 slots (full cycle) |

### Storage Layer

```
Store:  coordinate (position in field)
Weight: calculated from coordinate (not stored)

Q8:  coordinate = 8-9 bits
Q4:  coordinate = 5-6 bits
F32: coordinate = 11-12 bits
```

### Runtime Layer

```
Use: 20736 slots (12⁴ = full cycle)
     for beam computation

Adjustable cycle:
     - Full: 20736 (12⁴)
     - Half: 10368 (12³ × 6)
     - Quarter: 5184 (12² × 3)
```

---

## 4. Icosahedron Coordinate System

### Structure

```
Icosahedron:
  12 vertices
  20 faces (triangles)
  30 edges

Field mapping:
  20736 slots ÷ 12 vertices = 1728 slots/vertex
  1728 = 12³ (base-12 structure)
```

### Coordinate Format

```
coordinate = (zone, position_in_zone)

zone:           0-11  (Icosahedron vertex)
position_in_zone: 0-1727 (within vertex neighborhood)

total: 4 bits + 11 bits = 15 bits
```

### Hierarchical Decomposition

```
Level 0: zone (0-11)          = 4 bits
Level 1: cluster (0-11)       = 4 bits  within zone
Level 2: pipe (0-11)          = 4 bits  within cluster
Level 3: tick (0-11)          = 4 bits  within pipe

Total: 4 × 4 = 16 bits (full coordinate)
```

---

## 5. Runtime Computation

### Beam Timer

```c
// Base-12 timer: 12⁴ = 20736 ticks
// Each tick = one slot in the field

uint32_t beam_timer_tick(uint32_t timer) {
    timer++;
    if (timer >= 20736) timer = 0;  // full cycle
    return timer;
}
```

### Frame Seek

```c
// Frame seek: 1440 frames (12² × 10)
// Stride-37 pattern for geometric traversal

uint32_t frame_seek(uint32_t frame) {
    return (frame * 37) % 1440;
}
```

### Combined Runtime

```
Runtime = beam_timer (20736) + frame_seek (1440)
        = full geometric computation

Storage = coordinate only
        = weight is derived
```

---

## 6. DRAMTile Integration

### Unified Memory

```
DRAMTile: RAM = Disk, Disk = RAM
          ↓
On-demand loading via coordinate

weight at position X:
  if X in RAM → access directly (O(1))
  if X on Disk → load to RAM → access (O(1))
```

### Model Routing

```
Small model (hot weights):
  → handles simple queries
  → routes complex queries to big model

Big model (cold weights):
  → stays on DRAMTile (disk)
  → load only needed coordinates
  → RAM = only active weights
```

---

## 7. Performance Metrics

### Current Implementation

| Metric | Value |
|--------|-------|
| Field size | 20736 slots |
| Center | 10368 |
| Q8 range | -128..+127 |
| Position range | 10240..10495 |
| Tests | 22/22 PASS |

### Access Speed

```
weight → position:  O(1) (addition)
position → weight:  O(1) (subtraction)
beam_length:        O(1) (absolute value)
sign:               O(1) (comparison)
```

---

## 8. Comparison with GGUF

| Format | Bytes/Param | Storage (0.5B) | Access |
|--------|-------------|----------------|--------|
| GGUF F32 | 4 bytes | 1884 MB | O(1) via metadata |
| GGUF Q8_0 | 1 byte | 471 MB | O(1) via metadata |
| Beam (old) | 4.125 bytes | 1943 MB | O(1) direct |
| Beam (geometric) | 0 bytes* | 0 MB* | O(1) direct |

*Beam geometric stores coordinate, weight is derived.

---

## 9. Open Challenges

### Challenge 1: Coordinate Size

Current coordinate = 15 bits (zone + position_in_zone)

**Problem:** 15 bits > GGUF Q8_0 (8 bits)

**Goal:** Make coordinate ≤ 8 bits for Q8 weights

### Challenge 2: Angular Map

Icosahedron has 20 faces. Can we map 256 Q8 positions to 20 faces efficiently?

**Approach:** 2^n angular map with binary subdivision

### Challenge 3: Delta Encoding

Consecutive weights are often similar. Can we store differences instead of absolute positions?

**Approach:** Variable-length delta encoding

---

## Next Task

### Make Coordinates Smaller

**Goal:** Reduce coordinate size from 15 bits to ≤ 8 bits for Q8 weights

**Approaches to investigate:**

1. **Angular Map (2^n)**
   - Map 256 Q8 positions to Icosahedron faces
   - Binary subdivision within faces
   - Target: 8-9 bits per coordinate

2. **Delta Encoding**
   - Store difference between consecutive weights
   - Variable-length encoding for small deltas
   - Target: 4-6 bits per weight (if deltas are small)

3. **Sparse Representation**
   - Only store non-zero weights
   - Position = weight index
   - Target: variable (depends on sparsity)

4. **Block Quantization**
   - Quantize in blocks (like GGUF)
   - Store block scale + relative positions
   - Target: 6-8 bits per weight

**Success criteria:**
- Coordinate ≤ 8 bits for Q8 weights
- O(1) access preserved
- No metadata overhead
- Roundtrip: weight → coordinate → weight = exact

**Files to investigate:**
- `beam_addressing/beam_value.c` — current weight→coord implementation
- `core/beam_entropy_container.h` — container for beam storage
- `pipeline/test_beam_geometric.c` — geometric encoding tests

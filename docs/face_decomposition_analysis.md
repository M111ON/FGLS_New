# Infinity Kis Timeline — Face Decomposition Analysis
## Date: 2026-08-01

## The Two Paths

```
Face = 144 × 144 = 20736

Path 1 (USER CHOOSES): 128 × 162 = 20736
Path 2 (alternative):  256 × 81 = 20736

Scale: 16² × 9² = 12⁴ = 20736
```

## What This Means

### Path 1: 128 × 162
- **162** = full icosahedral vertex set
- **128** = 2⁷ (half of 256)
- Face organized as: 162 groups × 128 slots each

### Path 2: 256 × 81
- **81** = 3⁴ (ternary World A)
- **256** = 2⁸ (binary World B)
- Face organized as: 256 binary positions × 81 ternary positions

### Why Path 1 (128 × 162)?

**162 is the GEOMETRY** — the full icosahedral vertex set
**128 is the SLOT** — each vertex has 128 positions on the face

This means:
- 162 icosa vertices define the STRUCTURE
- 128 slots per vertex define the DATA positions

## Connection to 3³ Traversal

```
3³ = 27 positions
27 × 6 = 162 (full traversal = all icosa vertices)

Each position has 128 slots:
  162 × 128 = 20736 = face size
```

**The traversal visits 162 vertices, each with 128 slots**

## World AB Split (from 3³ prototype)

```
81 = 3⁴ = ternary World A (where to go)
54 = 2 × 3³ = binary World B (what to do)

162 = 81 + 54 + 27 (base positions)
```

**The 81 ternary positions = World A (CPU routing)**
**The 54 binary positions = World B (GPU batch)**
**The 27 base positions = traversal foundation**

## Scale Relationship

```
16² × 9² = 12⁴ = 20736

16² = 256 = 2⁸ (binary space)
9² = 81 = 3⁴ (ternary space)
12⁴ = (2² × 3)⁴ = 2⁸ × 3⁴ = 256 × 81
```

**This is the fundamental decomposition:**
- 2⁸ = 256 binary positions
- 3⁴ = 81 ternary positions
- 2⁸ × 3⁴ = 20736 = face size

## Infinity Kis Timeline Architecture

```
Face = 162 × 128 (Path 1)

162 icosa vertices (geometry):
  - Each vertex = one position in 3D space
  - Traversal: 3³ × 6 = 162 (full coverage)
  - CPU routes between vertices

128 slots per vertex (data):
  - Each slot = one weight position
  - 128 = 2⁷ (half of 256)
  - GPU batches within slots

The traversal:
  CPU: "Visit vertex X"
  GPU: "Compute 128 weights for vertex X"
  CPU: "Move to next vertex"
  GPU: "Compute next 128 weights"
  ...
  After 162 steps: full face covered
```

## Why 128 (not 256)?

**128 = 2⁷ = half of 256**

This suggests:
- Full binary space = 256 = 2⁸
- Half = 128 = 2⁷
- The face uses HALF the binary space

**Possible meaning:**
- 256 positions, but only 128 are "active" (the other 128 are mirrors/negatives)
- Or: 128 slots × 2 (positive/negative) = 256 total
- The "pos/neg" split: dodeca(pos) uses 128, icosa(neg) uses 128

## The Spike Conversion

```
dodeca(pos) → spike → icosa(neg) → spike → dodeca(pos)

Each spike:
  - Converts 128 dodeca slots → 128 icosa slots
  - Preserves the 162 vertex structure
  - Total: 128 × 162 = 20736 per geometry
```

## CPU/GPU Split

**CPU (dodeca pos):**
- Routes between 162 vertices
- Decides which 128 slots to process
- Manages traversal (3³ pattern)

**GPU (icosa neg):**
- Batches 128 weights per vertex
- Parallel computation
- Returns results to CPU

**The handoff:**
```
CPU: "Process vertex 42, slots 0-127"
GPU: [batch compute 128 weights]
CPU: "Move to vertex 43, slots 0-127"
GPU: [batch compute 128 weights]
...
After 162 vertices: full face processed
```

## Storage Implications

**Traditional:** store 568 MB of weights
**Infinity Kis:** store 0 bytes — generate on-the-fly

**The encoder is the traversal:**
```
weight = encoder(vertex, slot, seed, codec)
       = traverse_kis_timeline(vertex, slot)
```

**Storage:**
- Seed: initial vertex + slot (few bytes)
- Codec: traversal rules (few bytes)
- Total: ~16 bytes

**Reconstruction:**
- CPU routes through 162 vertices
- GPU computes 128 weights per vertex
- No memory bandwidth bottleneck

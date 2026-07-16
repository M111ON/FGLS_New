# Hex-19 vs Goldberg — Impact Analysis

## Current System (Goldberg GP(4,0))

```
Address Space:  20,736 = 144² = 128 × 162
Faces:          162 (12 pentagons + 150 hexagons)
Per Block:      54 DiamondBlocks × 64B = 3456B data
FrustumBlock:   4896B (17B header + 3456B data + 1440B meta)
Block Count:    ~384 blocks to cover full sphere
Pentagon Stride: 1728 nodes per pentagon sector
Clock Cycle:    144 ticks
Capo Stride:    12
```

## Proposed: PFC + Hex-19 as Internal Generator

```
Face Count:     12 (PFC contract — fixed)
Cells/Face:     19 (Hex cluster: 1 + 6 + 12)
Total Cells:    12 × 19 = 228
Core Structure: 13 (Center + Ring + 6 Outer Anchors)
Residual:       36 triangles per face = 432 total
Expansion:      6 orange modules per face = 72 total
```

## Direct Comparison

| Property | Goldberg GP(4,0) | PFC + Hex-19 |
|---|---|---|
| **Face count** | 162 | 12 |
| **Cells per face** | 1 (Goldberg tile) | 19 (hex cluster) |
| **Total slots** | 162 × 54 = 8748 | 12 × 19 = 228 |
| **Granularity** | Fine (54 diamonds/block) | Coarse (19 cells/face) |
| **Adjacency** | Custom graph lookup | Standard hex neighbor |
| **Expansion points** | 12 drains + 28 shadows | 6 orange modules + 36 triangles |
| **Residual space** | None (all 54 slots used) | 36 triangles/face (scratch) |
| **GPU thread mapping** | 54 = 6×9 (non-power-of-2) | 19 = prime (worse) |
| **Address computation** | FNV-1a hash + RDH | Direct face/cell index |

## What Would Actually Change

### 1. Address Space (CRITICAL)

Goldberg gives 20736 unique addresses. Hex-19 gives only 228.

This is a **99% reduction** in address space. For LLM tensor mapping where we need to address 251+ tensors × multiple chunks each, 228 slots is far too small.

**Verdict:** Hex-19 alone cannot replace the current address space. It would need subdivision within each face (Goldberg or similar) to reach the same capacity.

### 2. FrustumBlock Structure

Current: 54 diamonds per block, 3 cosets of 18.
Hex-19: 19 cells per face, no natural coset partition.

54 = 6 × 9 (good for GPU warps of 32, partial utilization)
19 = prime (bad for GPU, no even split)

**Verdict:** 54 diamonds is better for GPU alignment. Hex-19 would require rethinking the block structure.

### 3. Adjacency Routing

Current: `geo_jump()` with 7 jump types, custom graph traversal.
Hex-19: Standard hex neighbor (6 directions, O(1) lookup via precomputed table).

Hex-19 adjacency is simpler and more natural. But the current `geo_jump()` already works and is tested.

**Verdict:** Hex-19 is conceptually cleaner, but current system already works.

### 4. Residual Space (36 triangles)

This is the **one genuine advantage**. 36 residual triangles per face = 432 total slots that could be used for:
- ECC parity (error correction)
- Scratch space for in-place compression
- Delta storage for SID face swaps
- Shadow zone expansion

Current system has no equivalent — all 54 diamonds are used for data.

**Verdict:** Genuinely useful if we need scratch space per face.

### 5. Expansion Modules (6 orange)

These map naturally to the 6 hex neighbors of each face. Could replace the current drain system:
- Current: 12 drains (pentagon-based)
- Hex-19: 6 expansion modules (face-based, per face)

This would change the flush/routing model from pentagon-centric to face-centric.

**Verdict:** Cleaner model, but requires rewriting drain/shadow logic.

## The Real Question

The PFC + Hex-19 is a **different level of abstraction**, not a replacement:

```
PFC (Topology)     ← describes face graph (12 faces, 5 neighbors)
  └─ Internal Generator  ← what goes INSIDE each face
       ├─ Goldberg (current: fine-grained, 54 diamonds/block)
       ├─ Hex-19 (proposed: coarse, 19 cells + 36 scratch)
       ├─ Triangle Fan
       ├─ Quad Mesh
       └─ Adaptive
```

The Goldberg system operates at a different resolution — it tiles the entire sphere with 162+ faces. The PFC with Hex-19 operates at the face level with 19 internal cells.

**They are complementary, not competing.**

## Concrete Experiment: Hybrid Approach

What if we use:
- **PFC** for the top-level face graph (12 faces, 5 neighbors)
- **Goldberg** for subdivision within each face (maintaining 20736 address space)
- **Hex-19** as a structural blueprint for the 13-node core within each face
- **36 residual triangles** as per-face scratch/ECC space

This gives us:
1. Stable topology (PFC)
2. Fine-grained addressing (Goldberg)
3. Natural hex adjacency for routing (Hex-19)
4. Scratch space for compression (residual triangles)

## Recommendation

**Don't replace Goldberg. Layer PFC on top.**

1. Document PFC as the topology specification (already done)
2. Use Hex-19 as a structural guide for face-internal layout
3. Keep Goldberg for fine-grained addressing
4. Consider repurposing 36 residual triangles as per-face scratch

The current 20736 address space is deeply embedded in:
- `geo_jump.h` (routing)
- `addr_space.h` (tensor mapping)
- `goldberg_sid.h` (bond graph)
- `frustum_layout_v2.h` (block structure)
- All runner code

Changing the address space would require rewriting all of these. The benefit (simpler adjacency) does not justify the cost.

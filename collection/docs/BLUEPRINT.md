# Blueprint Architecture — Zone Visibility from Base Edge

## Core Shift
**Old**: encode(features) → centroid → shift (centroid encodes position)
**New**: tensor drags base edge → angles open zone → detect (position irrelevant)

## Single Base Edge
One line segment between two vertices:
- a[i] = pentA vertex @ 72°×i
- b[j] = pentB vertex @ 72°×j + 36°

This edge is the only absolute geometry needed.

## Angle Rules
```
60° from edge  → hexagon face   (5× = full hex ring)
54° from edge  → pentagon face  (base of 54-54-72 triangle)
72° at apex    → tip pivot      (top of 54-54-72 triangle)
Intersection   → overlap zone   (pentA + pentB tiling)
```

## Zone Visibility
No need to compute centroid position:
1. Tensor drags the edge (direction + magnitude)
2. 60°/54° rule opens the corresponding zone
3. Intersection between pentA and pentB zones = active region
4. Observer at intersection point = frame-agnostic

Because we don't need absolute position, only relative topology + tensor.

## GT2 as Proof
GT2's tri+tip split is the first pass of this:
- tri = 2 base directions (edge of pentagon)
- tip = free vertex (hex vertex, "already there")
- avg shift 0.3550 vs old GB 0.6587

## Next: Blueprint Engine
Instead of computing centroids:
```
edge ← tensor
  ↓
60° → hex zone (5-fold)
54° → pent zone (5-fold)
72° → tip intersection
  ↓
zone overlap → active region
  ↓
detection (no position needed)
```

## Files
- `ctd_goldberg.h` — current implementation (GT2 tri+tip)
- `docs/BLUEPRINT.md` — this document

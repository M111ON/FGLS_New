# Encoder History

## Phase 1: 3Y → 6Y
- 3Y: 3 direction vectors → encode 3 features
- 6Y: 2×Y3 rotated 60° → encode 6 features
- Shift: 1.1396 avg

## Phase 2: P5H / P5Color
- Pentagon interleaved: 12/12 faces, 100% cross-model
- P5Color: 5 trapezoid sectors → same consistency
- Shift: 0.8326 avg

## Phase 3: Goldberg GP(1,1)
- 1 pentagon + 5 hex radial
- Weighted centroid pent×5 + hex×1 → /10
- Shift: 0.6587 avg, wins 1/10

## Phase 4: Shell Cross
- 2 pentagons (36° offset), cross-connect offset=3
- 5 edge midpoints + 2 pentagon centers
- Shift: 0.6323 avg, wins 1/10

## Phase 5: GT2 tri+tip split ← CURRENT DEFAULT
- 1 pentagon + 5 hex, but each hex = tri(2dirs) + tip(1dir)
- tri = dir[0]*f3 + dir[1]*f4 (triangle face against pentagon edge)
- tip = dir[2]*f5 (free vertex, already computed)
- Weight: pent×5 + (tri+tip)×1 per hex = /15
- **Shift: 0.3550 avg, wins 6/10**
- Vision tensor mlp.fc2: 0.727 vs GB 3.119 (4× better)

## Blueprint Concept (Proposed)
See `docs/BLUEPRINT.md` — next phase after GT2.

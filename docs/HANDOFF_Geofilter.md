# GEO Filter Codec — Test Session Handoff

Source spec: `geo-filter-codec-visual.md` (FILTER → MAP → XOR DELTA → STORE, 10×10×10 cube demo)

## Files (run in this order to reproduce)
1. `test_codec.py` — baseline: verify spec's own lossless/lossy claim (3-axis project+AND reconstruct)
2. `test_ghost_fix.py` — XOR(orig, recon) exception-list correction, cost vs density curve
3. `test_6dir.py` — add 6 diagonal/shear projections (x+y, x-y, y+z, y-z, x+z, x-z), ghost reduction
4. `test_full.py` — add 7th triple-diag (x+y+z), fine-grained threshold search
5. `test_scale.py` — same tests at DIM=20, DIM=32 (scaling behavior)
6. `test_rotate.py` — **proves axis-rotation adds ZERO new info** (rotation ≠ shear)
7. `test_shared_map.py` — 36-chunk shared-map hypothesis, overlap vs offset placement
8. `test_origin_label.py` — proves origin/rotation label alone can't fix identity collision
9. `test_delta_flag.py` — sparse per-cell ownership delta (only tag collision cells)
10. `test_lossy_tolerance.py` — full 10-value cube reconstruction, real error % if lossy tolerated

## Key findings (in order discovered)

**1. Spec's core claim is correct but narrow.**
3-axis project→AND reconstruct is lossless only for line/single-point patterns.
Random scatter → ghost cells (false positives from AND of 3 projections).

**2. Ghost rate vs density (DIM=10, single value):**
- n<20 pts: lossless
- n=80-300 (10-30% density): **death zone** — codec costs MORE than raw dense storage
- n=700+ (near-full): recovers (sparse from the "0" side)

**3. Adding 6 diagonal/shear projections (u=x+y, v=x-y, w=y+z, s=y-z, t=x+z, r=x-z) genuinely helps.**
Pushes lossless threshold from n~20 → n~50 (DIM=10). This works because shear angles
add real new constraints (like tomography angles) — NOT the same as rotating the axes.

**4. CRITICAL: rotation (axis permutation) ≠ shear (diagonal skew).**
Proven empirically in `test_rotate.py`: combining 6 "rotated views" of the same cube
gives IDENTICAL ghost count to plain 3-axis. OR-projection is direction-independent,
so rotation just relabels the same 3 maps — zero new information. Any "batch 36 via
rotation" idea is a dead end mathematically. Only shear/skew adds information.

**5. Threshold scales with cube size and map count, roughly:**
- 3axis: ~0.15-1% density threshold, shrinks as DIM grows
- 9map (3axis+6diag): ~0.5-5% density threshold
- 15map: ~0.9-1.9% density threshold
- Each extra map costs O(DIM²) bits — no free lunch, must budget per use case.

**6. "36 chunks share one map" — tested both interpretations:**
- Overlapping same coords (literal share): density explodes (52%), ghost=481/519,
  AND individual chunk identity is unrecoverable (OR erases source info).
- Offset into disjoint regions of bigger virtual cube: near-lossless (ghost=1/720),
  but total map storage cost is IDENTICAL to storing 36 separate small maps
  (~32,400 bits either way). No savings — same info-theoretic cost, different shape.

**7. Origin/rotation labels can't fix identity collision.**
Tested: knowing which "rotation" a chunk used lets you correctly recover its 20 true
points, BUT can't reject the ~980 points that belong to other chunks (avg false-positive
per chunk). Rotation is a per-chunk label, not a per-cell label — doesn't carry enough
information to disambiguate individual overlapping cells.

**8. Sparse ownership-delta DOES save real bits (this was Po's refinement — works).**
Instead of dense per-cell tags, only flag cells with 2+ owners (~30% of active cells
in the 36-chunk/20pt test). Honest cost (dense union bitmap + owner-tags only on
collision cells): 5,320 bits vs 10,800 bits for 36 separate maps = **2.03x real,
verified-lossless savings**. Ratio should improve further at lower n_per_chunk
(fewer collisions) — not yet swept.

**9. Real-world worst-case test: full 10-value weight cube (uniform random 0-9).**
This matches "10% density per value, all 10 values compete" — the actual death-zone
scenario for real weight tensors.
- 3axis: 59.6% of cells end up wrong value (unusable)
- 9map: 3.8% of cells wrong, MAE=0.13 (out of 0-9 range)
- Verdict given: **3.8% error NOT acceptable if this mask is the primary value store**
  (breaks POGLS's exact-recall goal). Acceptable ONLY if this mask is a structural/
  routing/index layer with a separate lossless value store underneath (e.g. DiamondBlock,
  RewindBuffer) — open question, unresolved, needs Po's input on which layer this
  mask actually belongs to.

## Open questions for next session
1. Does the GEO filter/mask layer sit as the *primary value store* or as a
   *structural index* pointing to values stored losslessly elsewhere? This determines
   whether the 3.8% error rate is disqualifying or acceptable.
2. Sweep n_per_chunk lower (5-10 pts) in the 36-chunk shared-map test to find where
   ownership-delta savings ratio peaks.
3. Test against real POGLS weight tensor data (not uniform-random) — real weights
   likely cluster/structure more, so error should be lower than the 3.8% worst-case.
4. If pursuing shear-based maps further: closed-form threshold formula as function
   of (DIM, n_maps) was requested but not derived — still open.

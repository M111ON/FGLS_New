# Cylinder Intersection Experiment (Aug 4, 2026)

## Objective
探索 two helices on a cylinder → intersection pattern → weight encoding potential

## Setup
- Cylinder: circumference = 6 (faces), height = 3456
- Unroll → rectangle 6 × 3456
- Helix = straight line + wrap: `z = (α × θ) mod 3456`
- Two helices at angles α₁, α₂ intersect at points determined by `gcd(Δangle, 3456)`

## Key Findings

### 1. Intersection Count = `gcd(Δangle, 3456)`
- 3456 = 2⁷ × 3³
- gcd determines intersection count, NOT the angle values themselves
- Δangle=5 → gcd(5,3456)=1 → 1 intersection (sparse)
- Δangle=36 → gcd(36,3456)=36 → 36 intersections (dense)

### 2. NOT Bijective
- Multiple weight pairs produce same gcd → same intersection count
- w=0 vs w=128 → gcd=36
- w=1 vs w=129 → gcd=36 (SAME)
- Cannot uniquely identify weight pair from intersection count alone

### 3. Distribution for H=3456
- 31.6% of angle diffs have gcd=1 (sparse intersections)
- 0.9% have gcd=36 (dense intersections)
- Distribution follows factor structure of 3456 = 2⁷ × 3³

## Conclusions
- **NOT suitable for lossless encoding** (not bijective)
- **Potential uses**: error detection, clustering, geometric fingerprint
- **Cylinder wrapping** is essential — without wrap, just straight lines (no periodicity)

## Related Work
- v5 codec: value-based mapping → collision (99.66% mismatch)
- v6 codec: index-based mapping → lossless 100%, ratio 1.12x
- Torus walk: sequential walk visits only 1.67% of positions (256/15360)

## Files
- `runner/explore/cylinder_intersect.c` — the experiment
- `core/kis_codec_v5.h` — angular wavelet (collision bug)
- `core/kis_codec_v6.h` — index-based (lossless, 1.12x)
- `runner/explore/torus_walk_test.c` — torus geometry exploration

# Full Cycle Cube (20736³) — Coordinate Compression Analysis
## Date: 2026-08-01

## The Proposal
Instead of storing weights, use 20736³ as an ADDRESS SPACE:
- Weights are "parked" at coordinates (x, y, z) in the cube
- Store ONLY: compressed coordinates + seed + encoder
- Encoder reconstructs weights from coordinates

## Key Numbers
- 20736³ = 8,916,100,448,256 positions (8.9 trillion)
- Q8_0 weights: 595,984,384 (596M)
- Utilization: 0.0067% (very sparse)
- Raw Q8_0: 568.4 MB

## Coordinate Compressibility by Mapping Strategy

### Strategy 0: Naive Reshape
- x = i % 20736, y = (i/20736) % 20736, z = i/(20736²)
- Delta bits: x=1.00, y=1.00 → **0.5 MB for 2M coords**
- BUT: this is trivial — coordinates are implicit from ordering, no real compression

### Strategy 1: Stride-37 (Geometric Jump)
- x = (i × 37) % 20736, y = layer
- Delta bits: x=6.05, y=1.00 → **1.7 MB for 2M coords**
- Extrapolated: 596M × 6 bits = **447 MB** — still large!
- Why: stride-37 is linear, so delta is constant (37), but face-crossing deltas are large

### Strategy 2: Fibonacci Spiral
- Spiral positions on 144×144 face, depth = layer
- Delta bits: x=22.69, y=1.00 → **5.6 MB for 2M coords**
- Extrapolated: **1.67 GB** — WORSE than raw!
- Why: spiral creates non-sequential x positions, high entropy

### Strategy 3: Icosahedral Orbit ★ BEST
- Group weights into orbits under 60-element symmetry group
- orbit_representative + group_element per weight
- Delta bits: rep=1.00, ge=1.52 → **0.6 MB for 2M coords**
- Extrapolated: **596M × ~1 bit = 75 MB** — 7.5× better than raw!
- Why: orbit structure creates periodic, sequential coordinates

## The Missing Piece: Encoder Function

The coordinates (75 MB) tell WHERE weights live, but not WHAT they are.

The encoder must generate weight values from coordinates:
```
weight = encoder(orbit_representative, group_element, seed)
```

Possible encoder designs:

### Option A: Lookup Table (trivial)
- Store (orbit, ge) → weight mappings
- Storage: same as raw weights — defeats the purpose

### Option B: Separable Function
- If w(orbit, ge) = f(orbit) × g(ge)
- Store f (9.9M values × 8 bits = 9.9 MB) + g (60 values × 8 bits)
- Total: 75 MB coords + 10 MB f + tiny g ≈ **85 MB** (7× compression)
- Requires: weight separability assumption

### Option C: Neural Network Encoder
- Small network: input (orbit, ge) → output weight
- Network size: few MB (e.g., 2-layer MLP with 128 hidden)
- Total: 75 MB coords + 5 MB network ≈ **80 MB** (7× compression)
- Requires: training the network to approximate weights

### Option D: Hybrid (Most Promising)
- Separate weight types: embedding, attention, MLP
- Each type gets its own encoder (different structure)
- Shared orbit structure across types
- Total: 75 MB coords + per-type encoders ≈ **100 MB** (6× compression)

## Tradeoff Analysis

| Approach | Storage | Quality | Reconstruction |
|----------|---------|---------|----------------|
| Raw Q8_0 | 568 MB | 100% | None (identity) |
| Bake (drop 11%) | 507 MB | PPL 23.1 | Zero small weights |
| Cube coords (orbit) | 75 MB | ? | Encoder generates |
| Cube + encoder | ~100 MB | ? | Network approximates |

The cube approach sacrifices QUALITY for SIZE:
- If encoder is perfect: lossless 7× compression
- If encoder is approximate: lossy but potentially better than bake

## Research Questions
1. Can weights be factored as f(orbit) × g(ge)? (test on real weights)
2. How small can the encoder network be while maintaining quality?
3. Does the orbit structure align with weight tensor structure?
4. Can we train the encoder end-to-end with the model?

## Connection to Existing Work
- geo_jump: maps (face, x, y) → weight index (reverse of coordinate mapping)
- geo_frame_seek: maps time → face position (stride-37)
- stride-37: the core temporal mapping
- 20736: contour mask size (2D face of geometry)
- The cube extends this to 3D: (face, layer, time)

## Next Steps
1. Test separability: can weights be factored as f(orbit) × g(ge)?
2. Design minimal encoder network (test on Qwen3-0.6B)
3. Compare with bake approach (quality vs size)
4. Prototype full pipeline: weights → orbit coords → encoder → reconstruction

# GEOM_BLUEPRINT Specification

## Overview

GEOM_BLUEPRINT is a future GEOM subtype where chunk data is reconstructed
from a compact descriptor instead of being stored inline. This document
outlines the design direction — **not yet implemented**.

## GEOM Subtype Tree

```
GEOM (marker=0x04)
 ├─ INLINE     (geom_type=0x00, 65B)   ← current baseline
 ├─ BLUEPRINT  (geom_type=0x01, ≤24B)  ← this document
 ├─ PARAMETRIC (geom_type=0x02, ≤12B)  ← future
 └─ TEMPLATE   (geom_type=0x03, ≤8B)   ← future
```

## GEOM_BLUEPRINT v1 — Outline (18B payload)

```
[version: 1B = 0x01]
[rotation: 1B]     — Diamond Shell rotation index (0-5)
[seed: 8B]         — wallet_chunk_seed of original chunk
[template: 6B]     — geometric template (first 6 bytes of rotated core)
[flags: 1B]        — SPARSE/DENSE indicator + reserved bits
```

### Reconstruction Path

1. **seed → verify**: Compute seed of reconstructed chunk, compare with stored seed
2. **template → expand**: Use template as base pattern, expand to 64B via rotation
3. **rotation → inverse**: Apply inverse Diamond Shell rotation to recover original orientation

### Constraints (Must Prove Before Implementation)

- [ ] Lossless: reconstructed chunk must byte-match original
- [ ] Invertible: every GEOM_BLUEPRINT chunk must decode identically
- [ ] Faster than GEOM_INLINE: decompression must be faster than reading 64B raw
- [ ] Smaller than GEOM_INLINE: payload must be < 65B

### Open Questions

1. Can seed + template + rotation uniquely determine a 64B chunk?
   - If no → GEOM_PARAMETRIC or GEOM_TEMPLATE may be more appropriate
   - If yes → proceed with implementation

2. What is the minimum template size that guarantees unique reconstruction?
   - 6B template + 8B seed = 14B → can this represent all 2^512 possible chunks?
   - Likely not for arbitrary data — may need hybrid approach

3. Should the version byte be inside the payload or implicit from geom_type?
   - v1 inside payload allows independent versioning per subtype
   - Implicit saves 1B but couples geom_type to version

## Integration with 3-Layer Architecture

| Layer | Role |
|-------|------|
| **Layer 1 (Spec)** | GEOM category exists in skeleton decision tree — unchanged |
| **Layer 2 (Storage)** | GEOM_BLUEPRINT is a new geom_type in the wire format |
| **Layer 3 (Runtime)** | Blueprint reconstruction may need Diamond Shell rotation lookup |

## When to Implement

Implement GEOM_BLUEPRINT when:
1. A lossless reconstruction method is proven (tested on 1000+ diverse chunks)
2. Template size is minimized (currently estimated 14-18B)
3. Decompression is faster than inline read (C-DLL or numpy accelerated)
4. Compression ratio improvement justifies the complexity

**Current status: NOT IMPLEMENTED — GEOM_INLINE (65B) is the correctness baseline.**

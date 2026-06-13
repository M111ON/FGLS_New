# GeoTorus CTD Engine — Draft Spec v0.1

## Overview

Geometry-native reference frame engine built on FiboClock spine.  
Core idea: **address = data descriptor**, reconstructible from 3 equations alone.

---

## 1. Core Math (3 equations only)

```
1. Fib(n) + Fib(n+1) = Fib(n+2)          ← spine / timebase
2. ctd    = (min + max) / 2               ← reference frame anchor
3. gcd(Fib(n), Fib(n+1)) = 1             ← deadlock-free guarantee
```

Everything else is derived from these.

---

## 2. Structure

### 2.1 Spine (FiboClock)
- Global timebase — 1 tick syncs everything
- Consecutive Fibonacci → always coprime → no deadlock by math
- Runs faster than wall clock (helix, not circle)

### 2.2 Ribcage (Torus Pent-Hex Gear)
- Pentagon (n=5) + Hexagon (n=6) share 1 vertex
- gcd(5,6) = 1 → connected traversal, no dead zones
- Each rib = local FiboClock (own phase)
- Spine = sync point only, not master
- Center shape = prime → fingerprint space × n, fully connected

### 2.3 CTD (Centroid Reference Frame)
- 8-corner bounding box per tensor (abstract, no fixed position)
- ctd = centroid of 2 diagonals = invariant point
- Snapshot origin at creation → reconstruct from ctd + 1 value
- ctd lives at center hole of torus → stable by topology

---

## 3. Address Space

```
Base torus fingerprints : 360        (LCM(5,6) × 12)
× shell layers          : 12
× pentagon anchors      : 12
× tower nodes           : 144
= ~7.4M unique addresses (Globe A)
× Globe B               : ×2
= ~15M total
```

All O(1) lookup — address IS the fingerprint.

---

## 4. Fingerprint Generation

```
input tensor
→ compute ctd
→ map ctd to geo address via Hilbert/Pentagon/Peano
→ address = fingerprint (no search needed)
→ store in ZoneCard.zone_id
```

Locality preserved by Hilbert curve —  
nearby tensors → nearby fingerprints → cluster without training.

---

## 5. Multi-CTD Network (Phase 2)

```
single ctd   → single pointer    (Phase 1)
multi ctd    → multi pointer     (Phase 2)
network ctd  → guitar strings    (Phase 3)
             → tension between ctd nodes
             → torus = soundboard
```

Each ctd = local origin.  
All ctd → reconstructible to global origin via same formula.  
Sync = 1 FiboClock tick.

---

## 6. ZoneCard Integration

```c
ZoneCardV3.zone_id    = geo address (ctd mapped)
ZoneCardV3.flags      = HOT_PATH if pentagon anchor
ZoneCardMeta (task)   = payload detail (ext_ptr)
```

CTD stored as 3×float32 = 12 bytes in META_CUSTOM payload.

---

## 7. Key Properties

| Property | Mechanism |
|----------|-----------|
| No deadlock | gcd(Fib(n), Fib(n+1)) = 1 always |
| Full traversal | Pentagon coprime geometry |
| O(1) lookup | address = descriptor |
| Reversible | ctd snapshot + origin |
| Scale-free | Geo address chains to level N |
| Freeze-safe | Constants frozen at init, no runtime decisions |

---

## 8. What This Is NOT

- Not similarity search (use FAISS for that)
- Not ML-based clustering
- Not a traditional codec

**It is a deterministic geometric reference frame  
that makes address, fingerprint, and reconstruction  
the same operation.**

---

## 9. Open Questions

- [ ] ctd sync protocol for multi-tensor network
- [ ] center shape selection rule (prime only?)
- [ ] Hilbert vs Pentagon mapping for ctd → geo address
- [ ] TTL behavior when ctd drifts (tensor mutation)

---

*Draft v0.1 — GeoTorus CTD Engine*  
*Based on conversation: Pentagon gear, Torus topology, FiboClock spine*

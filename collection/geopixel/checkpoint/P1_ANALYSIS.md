# P1 Analysis — What Actually Needs Patching?

## The Question
"GeoPixel v21 needs raster→walk swap" — but where/why?

---

## Answer

### Two Separate TRing Systems

**System A: geo_tring_addr.h (OLD, in geopixel_v20.c)**
- Maps pixels (tx, ty, channel) → TringAddr {addr, pattern_id}
- Address space: 0..6911 (live + residual)
- Purpose: Pixel tile classification
- Loop pattern: `for i in 0..tiles: tx=i%W, ty=i/W` (raster scan)
- Status: **Self-contained, doesn't interact with L2 dispatch**

**System B: tring_route_from_enc() (NEW, in tgw_stream_dispatch.h)**
- Maps packets enc → pos (0..719) → pentagon/spoke/polarity
- Address space: 0..719 (TRing walk positions)
- Purpose: Dispatch routing (ROUTE vs GROUND)
- Math: `pos / 60 = pentagon`, `pentagon % 6 = spoke`
- Status: **Verified in P0, works perfectly**

### Why the Confusion?

The handoff doc mentioned "GeoPixel v21 needs TRing walk" but this refers to a **hypothetical future integration** where GeoPixel codec output would be routed through L2 dispatch via System B (tring_route_from_enc).

**Currently:** GeoPixel is visualization-only (not in data path).

---

## Current Data Flow (Verified)

```
TStreamPkt (L3)
  ↓
tgw_stream_dispatch() (L2) ← uses System B (tring_route_from_enc)
  ├─ pos = tring_pos(enc)           ✅ verified
  ├─ pentagon = pos / 60            ✅ verified
  ├─ spoke = pentagon % 6           ✅ verified
  └─ polarity = (pos % 60) >= 30   ✅ verified
  ↓
ROUTE/GROUND split
  ├─ ROUTE → tgw_write() → goldberg
  └─ GROUND → pl_write() direct
  ↓
goldberg_pipeline_write() (L5)
  ↓
Blueprint output (can be visualized via gbt_to_geopixel)
```

**GeoPixel v20:** Not in this path, just used for testing/visualization.

---

## What P1 Actually Needs

NOT: Patching GeoPixel v20 raster loops

YES: **Integration test** that wires:
- tgw_stream_dispatch() output
- → goldberg_pipeline_write()
- → stats consistency check

**Files to create:**
1. `test_integration_l3_l2_l5.c` — Stream → Dispatch → Goldberg
2. Verify spoke distribution preserved through full chain
3. Verify ROUTE/GROUND split doesn't corrupt data

---

## GeoPixel v21 Status

**Real question:** Where is geopixel_v21_o25.c?

If it exists separately from v20, check if it:
1. Has its own TRing encoding (System A or B)?
2. Is meant to replace v20 in the data path?
3. Or is just an experiment?

**Current recommendation:**
- **Skip GeoPixel patch for now** (not blocking L2-L3-L5 chain)
- **Focus on integration test** instead
- Add GeoPixel later if needed

---

## Status Update

✅ **P0:** Layer 2 core math verified
⏭️  **P1 (revised):** Integration test (stream→dispatch→goldberg)
⏭️  **P2:** Full pipeline stats verification

**Timeline:** P0 done, P1 next (30 min for integration test)

# Layer Testing Map — What's Done vs What's Missing

## Existing Tests (Already Written)

### Layer 2: TGW Dispatch ✅ TESTED
**test_tgw_dispatch.c**
- D1: GROUND gate (sign mismatch → pl_write direct) ✓
- D2: ROUTE path (150 writes → blueprint_ready) ✓
- D3: tgw_write_dispatch (one-call API) ✓
- D4: stats consistency (route+ground+no_bp==total) ✓
- D5: read-back (ROUTE fts vs GROUND pl_read) ✓

**Status:** L2 core dispatch logic verified

---

### Layer 2-L5 Integration: GROUND Path ✅ TESTED
**test_ground_roundtrip.c**
- T11a: lch_gate polarity → GROUND triggered ✓
- T11b: pl_write/pl_read round-trip ✓
- T11c: pl_read_rewind (hop-back) ✓
- T11d: PayloadStore capacity (864 cells) ✓
- T11e: CubeFileStore round-trip (serialize→gcfs_read→reconstruct) ✓
- T11f: full GROUND pipeline (noise→gate→PayloadStore) ✓

**Status:** L2 → L5 (via GROUND) verified through PayloadStore + CubeFileStore

---

### Layer 3: Stream Input ✅ TESTED
**test_tgw_stream.c**
- 1. slice_roundtrip (slice file → reconstruct → bytes identical) ✓
- 2. tring_ordering (chunk order via walk position) ✓
- 3. geopixel_stream (packets → valid pixels) ✓
- 4. structured_signal (variance detection) ✓
- 5. gap_detect (drop 1 packet → gap detection) ✓

**Status:** L3 stream input + TRing ordering verified

---

### Layer 2 Edge Cases: Geomatrix Verdict ✅ TESTED
**test_geomatrix_verdict.c**
- verdict logic: addr/value routing decisions
- blueprint accumulation
- FTS path correctness

**Status:** Dispatch verdict logic verified

---

### Layer 2 Edge Cases: FTS Route ✅ TESTED
**test_fts_route.c**
- FTS twin store routing
- ROUTE path (non-GROUND)
- lane distribution

**Status:** FTS routing verified

---

## What's MISSING (Chain Gaps)

### Gap 1: Stream → Dispatch (L3 → L2)
**test_tgw_stream.c** tests slice + tring_ordering
**test_tgw_dispatch.c** tests dispatch in isolation

**MISSING:** Wire them together
```c
tgw_stream_dispatch() — the new function!
  ├─ slice file (L3)
  ├─ TRing order via enc (L3)
  └─ dispatch with polarity (L2) ✓ tgw_stream_dispatch.h written
```

**Status:** tgw_stream_dispatch.h exists but **test_tgw_stream_dispatch.c hasn't been run**

---

### Gap 2: L3 GeoPixel Not in Stream Path
**test_tgw_stream.c** has:
```c
gbt_to_geopixel(const GBTRingPrint *fp) {
    // ad-hoc RGB encoding from tring_enc
}
```

**Missing:** GeoPixel v21 codec in actual stream decode
- geopixel_v21.c exists but not wired into tgw_stream
- raster indexing (i%TW) not yet swapped to TRing walk
- tile → spoke routing not tested

**Status:** Codec exists, path incomplete

---

### Gap 3: L3 → L5 (Goldberg) Direct Path
**test_tgw_stream.c** stops at GeoPixel RGB
**test_tgw_dispatch.c** verifies dispatch but not fed from stream

**Missing:** stream packets → dispatch → goldberg → blueprint
- tgw_stream doesn't call tgw_dispatch
- goldberg output not checked

**Status:** No end-to-end stream→dispatch→goldberg test

---

## The Real Situation

```
TESTED:
  L2 (dispatch logic) ✅
  L2-L5 (GROUND path) ✅
  L3 (stream slicing + ordering) ✅
  L5 (goldberg individually) ✅

NOT TESTED (chain gaps):
  L3 → L2 wiring (tgw_stream_dispatch)
  L3 → L5 (goldberg from stream)
  L3 GeoPixel in stream (v21 codec)
  full pipeline: stream → dispatch → goldberg → stats
```

---

## What Needs to Run RIGHT NOW

### 1. Compile & Run test_tgw_stream_dispatch.c
Tests SD1-SD5 (already written, never compiled)
```bash
gcc -O2 -o test_tgw_stream_dispatch test_tgw_stream_dispatch.c \
  -I./core/core \
  -I./core/pogls_engine/TPOGLS_s11/TPOGLS_s11 \
  -I./core/pogls_engine/TPOGLS_s11/TPOGLS_s11/twin_core \
  -lm

./test_tgw_stream_dispatch
```

**Expected:** SD1-SD5 all pass ✅
**Time:** 15 min (mostly waiting for compilation)

---

### 2. Verify GeoPixel v21 Code Location
Where is geopixel_v21_o25.c?
```bash
find . -name "*geopixel*v2*" -o -name "*geopixel_v21*"
```

If in geopixel_animated.zip:
```bash
unzip -q geopixel_animated.zip
```

**Action:** Locate & review decode loop
```bash
grep -n "for.*i.*TW" geopixel_v2*.c
```

---

### 3. Write Integration Test
Connects L3 stream → L2 dispatch → verify output
```c
// test_stream_to_dispatch.c
tgw_stream_dispatch(&ctx, &d, &sd, buf, size);
// Check: spoke distribution, polarity split, goldberg output
```

---

## The Flow (Current State)

```
OLD (tested separately):
┌─────────────────┐
│ L3: tgw_stream  │  ← slice, order via walk
├─────────────────┤
│ L2: tgw_dispatch│  ← verdict, ROUTE/GROUND
├─────────────────┤
│ L5: goldberg    │  ← blueprint write
└─────────────────┘
      (no wires!)

NEW (needs testing):
┌─────────────────┐
│ tgw_stream_file │  → tgw_stream_dispatch()
├─────────────────┤ ↓ (wired: L3→L2)
│ tgw_dispatch()  │  → ROUTE/GROUND split
├─────────────────┤ ↓ (already tested L2→L5)
│ goldberg_write()│  → blueprint + spoke
└─────────────────┘

+ GeoPixel codec (L3) not yet in stream
```

---

## Priority Order (What to Do First)

**P0 (TODAY):** Compile test_tgw_stream_dispatch
- Already written
- Just need working compilation
- Proves L3→L2 wiring works

**P1 (IMMEDIATE):** Find + patch GeoPixel v21
- Locate file (in geopixel_animated.zip)
- Swap raster → TRing walk (1-3 lines)
- Verify decode works

**P2 (NEXT):** Integration test
- Wire stream → dispatch → goldberg → stats
- Verify all 3 layers work together

**P3 (AFTER):** Full pipeline benchmark
- RAM usage
- Throughput (packets/sec)
- Memory layout

---

## Summary

| Layer | Tested | Status |
|-------|--------|--------|
| **L2 (dispatch)** | ✅ test_tgw_dispatch.c | Verified individually |
| **L2→L5 (GROUND)** | ✅ test_ground_roundtrip.c | Verified via PayloadStore |
| **L3 (stream)** | ✅ test_tgw_stream.c | Verified individually |
| **L3→L2 (wiring)** | ❌ test_tgw_stream_dispatch.c | Written but never run |
| **L3 GeoPixel** | ❌ geopixel_v21.c | Exists, path incomplete |
| **L3→L5 (goldberg)** | ❌ none | No test, gap exists |

**Action:** Run test_tgw_stream_dispatch first (15 min), then patch GeoPixel, then write integration test.

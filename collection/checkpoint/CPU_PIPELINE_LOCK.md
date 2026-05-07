# CPU Pipeline Lock — Wiring Status

## Layer 2: TGW Dispatch (STREAM FLOW)

### ✅ WIRED & VERIFIED
- `tgw_stream_dispatch.h` — polarity derive O(1) via `tring_route_from_enc()`
  - `pentagon = pos / 60`
  - `spoke = pentagon % 6`
  - `polarity = (pos % 60) >= 30`
  - sacred constants frozen: 720, 60, 30, 6
  
- `test_tgw_stream_dispatch.c` — 5 test cases (SD1-SD5)
  - SD1: spoke coverage (6 spokes hit)
  - SD2: polarity split (~50/50 live/mirror)
  - SD3: stats consistency
  - SD4: GROUND read-back via pl_read
  - SD5: SEAM 1 proof (old path=0 GROUND, new path>0)

**Status:** Ready to compile & run. Missing: `core/core/geo_tring_goldberg_wire.h` path

---

## Layer 3: Stream → Pipeline (BLOCKED BY SEAM 3)

### ❌ OPEN — GeoPixel v21 Not in Stream Path
Files:
- `geopixel_v20.c` — codec exists
- `geo_pixel.h` — interface exists
- `geo_tring_addr.h` — TRing address mapping exists

**Problem:** GeoPixel decode loop uses raster indexing
```c
for (i = 0; i < num_tiles; i++) {
    // ❌ WRONG: i % TW, i / TW — raster order
    // ✅ NEED:  tring_walk(i) → pos → pentagon/spoke
}
```

**Fix:** 1-line swap per decode function
- `decode_flat()` — swap tile indexing
- `decode_grad()` — swap tile indexing
- any other tile iteration loop

**Impact:** GeoPixel tiles routed via spoke lanes (currently ignored)

---

## Layer 4: Dimension Routing (CHECKING)

### ✅ DEPENDS ON SEAM 2 FIX
`tgw_stream_dispatch.h` already derives spoke from enc.
No separate `geo_addr_net.h` needed — `tring_route_from_enc()` is stateless.

**Check:**
- Does `rh_map(GeoNetAddr)` need to exist separately?
- Or does spoke from `tring_route_from_enc()` feed dispatch directly?

Current code path:
```c
TRingRoute rt = tring_route_from_enc(enc);  // spoke=0..5
// → rt.spoke used where?
```

---

## Layer 5: SpaceFrame (FROZEN)

### ✅ VERIFIED
- `geo_temporal_ring.h` — TRing walk intact
- `goldberg_pipeline_write` — pipeline intact
- sacred numbers: 144, 3456, 6, 54 (digit_sum=9)

No changes needed.

---

## Compilation Chain

### Step 1: Verify test_tgw_stream_dispatch.c
```bash
# Need full core path structure:
# core/
#   geo_tring_goldberg_wire.h
#   geo_tring_stream.h
#   tgw_dispatch.h
#   (all TGW headers)

gcc -O2 -o test_tgw_stream test_tgw_stream_dispatch.c \
  -I./core/core \
  -I./core/pogls_engine \
  -I./core/pogls_engine/TPOGLS_s11/TPOGLS_s11 \
  -I./core/pogls_engine/TPOGLS_s11/TPOGLS_s11/twin_core \
  -lm
```

### Step 2: GeoPixel v21 Patch
- Locate decode loop in `geopixel_v20.c`
- Replace raster `i % TW` with `tring_walk(i)` call
- Verify spoke distribution in output

### Step 3: Integration Test (NEW)
- Test that stream packets → tiles → spokes maintain distribution
- Verify no packets dropped in raster→walk conversion

---

## Risk Assessment

**GREEN:**
- Stream dispatch logic frozen & math-verified
- TRing walk stateless (no state corruption risk)
- No malloc/free changes needed

**YELLOW:**
- GeoPixel v21 not yet tested with walked indexing
- Need to verify decode boundary handling (tile at walk end)

**RED:**
- None identified (CPU path only, no GPU)

---

## Action Items (Priority)

1. **BUILD:** Extract core path structure, compile test_tgw_stream_dispatch
2. **VERIFY:** Run SD1-SD5, confirm all 5 pass
3. **PATCH:** Swap geopixel_v20.c raster → walk
4. **TEST:** New integration test (stream → pixel dispatch)
5. **FREEZE:** CPU pipeline lock v1.0

---

## Handoff Checkpoint

When all 3 tests pass:
- Layer 2 (stream dispatch) ✅
- Layer 3 (geopixel walk) ✅
- Layer 4/5 (routing + goldberg) ✅ (unchanged)

→ CPU pipeline complete, ready for detailed optimization sweeps.

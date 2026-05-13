# CPU Pipeline Lock v1 — Standalone (RAM Only, No Persistence)

## Target: Get dispatch → goldberg → RAM working end-to-end

```
Layer 1: World (skip for now)
  └─ unused

Layer 2: TGW Dispatch ← START HERE
  ├─ tgw_stream_dispatch.h ✅ ready
  └─ polarity derive O(1)

Layer 3: Stream → Pipeline
  ├─ tgw_stream_file + tgw_stream_dispatch ✓
  ├─ geopixel_v21 (needs raster→walk swap)
  └─ output: ROUTE/GROUND split

Layer 4: Dimension (Hilbert) ← frozen, don't touch
  └─ spoke routing (derived from enc in Layer 2)

Layer 5: SpaceFrame (TRing + Goldberg)
  ├─ geo_temporal_ring.h ✓ frozen
  ├─ goldberg_pipeline_write ✓ frozen
  └─ output: pl_write (FtsTwin in RAM)

[LC-GCFS] ← REMOVE from scope, add after v1 locks
```

---

## Three Compilation Targets

### 1. test_tgw_stream_dispatch
Verify Layer 2 dispatch logic
```bash
gcc -O2 -o test_tgw_stream test_tgw_stream_dispatch.c
  -I./core/core
  -I./core/pogls_engine
  -I./core/pogls_engine/TPOGLS_s11/TPOGLS_s11
  -I./core/pogls_engine/TPOGLS_s11/TPOGLS_s11/twin_core
  -lm

Expected: SD1-SD5 all pass ✅
```

**Tests:**
- SD1: spoke coverage (6 spokes)
- SD2: polarity split (~50/50)
- SD3: stats consistent
- SD4: GROUND read-back
- SD5: SEAM 1 proof (old=0, new>0)

**Output:** spoke/polarity distribution stats

---

### 2. geopixel_v21 raster→walk patch
Fix Layer 3 GeoPixel to use TRing ordering
```c
// BEFORE (raster):
for (i = 0; i < num_tiles; i++) {
    int x = i % TW;
    int y = i / TW;
    // process tile at (x,y)
}

// AFTER (TRing walk):
for (i = 0; i < num_tiles; i++) {
    uint16_t pos = tring_pos_inv(i);  // or iter via tring_walk
    int x = pos % TW;
    int y = pos / TW;
    // process tile at (x,y)
}
```

**Compile geopixel_v21 with walk:**
```bash
gcc -O2 -o geopixel_test geopixel_v21_o25.c
  -I./core/geo_headers
  -I./core/geo_headers/pixel
  -lm
```

**Output:** verify decode works, no boundary corruption

---

### 3. Integration Test (NEW)
Wire dispatch → goldberg → RAM verify
```c
// test_cpu_pipeline.c pseudocode

tgw_init(&ctx, seed, bundle);
tgw_dispatch_init(&d, root_seed);
tgw_stream_dispatch_init(&sd);

uint8_t buf[1MB];
make_payload(buf, 1MB);

// L2: dispatch
tgw_stream_dispatch(&ctx, &d, &sd, buf, 1MB);

// L5: should have goldberg writes
GBStats gbs = goldberg_stats(&ctx);
assert(gbs.writes > 0);

// Output: dispatch → goldberg → pl_write
TGWDispatchStats ds = tgw_dispatch_stats(&d);
assert(ds.ground_count + ds.route_count == ds.total_rx);
```

**Metrics to verify:**
- packets routed through ROUTE vs GROUND
- spoke distribution uniform (each 6 roughly equal)
- no gaps in TRing walk
- goldberg blueprints generated
- no memory leaks

---

## Build Chain

### Step 1: Compile test_tgw_stream_dispatch
```bash
# Extract core paths
mkdir -p build/
cd collection/core && find . -name "*.h" | head -100 > /tmp/includes.txt

# Compile
cd /tmp
gcc -O2 \
  -I./collection \
  -I./collection/core/core \
  -I./collection/core/pogls_engine/TPOGLS_s11/TPOGLS_s11 \
  -I./collection/core/pogls_engine/TPOGLS_s11/TPOGLS_s11/twin_core \
  -c ./collection/test_tgw_stream_dispatch.c -o test_tgw_stream_dispatch.o

# If errors: check which headers are missing, extract from core.zip
```

**Expected output:** test_tgw_stream_dispatch.o compiles clean

---

### Step 2: Patch geopixel_v21
Find raster loops in geopixel_v20.c / geopixel_v21_o25.c

```bash
grep -n "i % TW" geopixel_*.c  # find raster indices
# Replace with tring_walk() calls
```

**Files to check:**
- decode_flat()
- decode_grad()
- any tile iteration

**Change:** ~1-3 lines per function

---

### Step 3: Integration Test
```c
// test_cpu_pipeline.c
// Combines tgw_stream_dispatch + goldberg + stats
```

**Compile & run:**
```bash
gcc -O2 -o test_cpu_pipeline test_cpu_pipeline.c \
  -I./collection/core \
  -I./collection/core/pogls_engine/TPOGLS_s11/TPOGLS_s11 \
  -lm

./test_cpu_pipeline
```

**Expected:** all metrics pass, no crashes

---

## Success Criteria (v1 Lock)

| Test | Pass Condition |
|------|---|
| **SD1** | 6 spokes all > 0 ✓ |
| **SD2** | live+mirror both ≥30% of total ✓ |
| **SD3** | sum(ground+route+gap+no_bp) == rx ✓ |
| **SD4** | mirror packets findable via pl_read ✓ |
| **SD5** | old_path=0 GROUND, new_path>0 GROUND ✓ |
| **GeoPixel** | decode produces valid tiles, no corruption ✓ |
| **Integration** | dispatch→goldberg→stats all consistent ✓ |

**When all 7 pass → CPU Pipeline v1 Locked ✅**

---

## Scope (v1)

✅ **Included:**
- Layer 2 dispatch (ROUTE/GROUND split)
- Layer 3 stream + geopixel
- Layer 5 goldberg (frozen)
- RAM storage (FtsTwin)

❌ **Deferred:**
- Layer 1 world/shell (add later)
- Layer 4 LetterCube (persistence, add after v1)
- GPU batch (Phase 14)
- Multi-file coordination
- Network streaming

---

## Files to Modify

**Minimal changes:**
1. `test_tgw_stream_dispatch.c` — already complete, just compile
2. `geopixel_v21.c` — 1-3 line swaps (raster → walk)
3. NEW: `test_cpu_pipeline.c` — integration test

**No changes to:**
- tgw_dispatch.h
- tgw_stream_dispatch.h
- goldberg_pipeline_write
- geo_temporal_ring.h

---

## Timeline

| Phase | Action | Time |
|-------|--------|------|
| **P1** | Extract core, compile test_tgw_stream | 15 min |
| **P2** | Locate geopixel raster, patch to walk | 10 min |
| **P3** | Write integration test | 20 min |
| **P4** | Debug + verify all 7 tests pass | 30 min |

**Total: ~1.5 hours to v1 lock**

---

## Next After v1 Lock

- Add LC-GCFS as Layer 4 output hook (persistence)
- Benchmark throughput (target: GB/s on T4)
- Optimize memory layout
- Then GPU batch (Phase 14)

---

## Remember

- **No malloc anywhere** (stack only)
- **No floats** (all integer math)
- **No external deps** (just C stdlib)
- **Stateless routing** (O(1) LUTs)
- **No persistence** (v1 = RAM only)

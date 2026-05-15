# HBV Session 4 Handoff

## Pipeline Status: REAL ✅

### What was built this session

**fibo_layer_header.h**
- FiboLayerHeader (32B self-describing)
- 12+4 route system: 4 sets × (3 pos + 1 inv)
- inv_of_inv = XOR fold proof of structure (never stored)
- fibo_layer_expand() O(1) no-heap
- fibo_layer_verify() integrity check free

**fibo_tile_dispatch.h**
- ScanEntry → FiboLayer → GpxTileInput bridge
- G channel = delta-hilbert (x-neighbor abs-clamped dh≤4)
  - G formula: `128 + (dh<<3) + (z & 0x0F)`  var≈125 ✅
- R channel = chunk>>1 (reduce dominance) var≈54
- B channel = (face*17 + px + py) & 0xFF  var≈10
- Epoch advance at 144-chunk Fibo closure

**fibo_hb_wire.h**
- GpxTileInput → hamburger classify+encode
- Encode RGB direct (not YCgCo) → ZSTD sees spatial correlation
- chunk-raw fallback if enc > 64B

**pogls_hilbert64_encoder.h + pogls_to_geopixel.h**
- H64Encoder: 64-slot Hilbert grid (12 faces × 4 paths)
- RGB balanced: 12 slots per channel
- invert derived: XOR of 3 positive per face (never stored)
- ghost zone: slots 48..63 for residual delta
- H64GeopixelBridge: H64Encoder → HbTileIn[] for hamburger

### Real Pipeline (test_h64_diamond.c)
```
File → pogls_scanner (real theta_map, real ScanEntry)
     → fold_block_init (chunk XOR → DiamondBlock)
     → diamond_gate (L0 xor_audit + L1 drift detect)
     → diamond_route → MAIN(1) or TEMP(2) lane
     → H64Encoder (path_id per lane)
     → h64_finalize (derive all inverts)
     → H64GeopixelBridge
     → hamburger (CODEC_SEED + ZSTD fallback)
```

### Results (high_detail.bmp 1.77MB)
```
drop(gate=0) :     0  (all blocks pass)
drift events :   693  (real content signal)
MAIN lane    : 10030  (36%)
TEMP lane    : 17619  (64%)
packets      :   769
skip(inv)    :  2162  (invert tiles = 4B seed only)
EDGE         : 87.9%
ratio        : 1.128x  (vs 64B chunk input)
```

### Open items (next session)

**O1 — MAIN vs TEMP codec split (ratio improvement)**
- MAIN lane → CODEC_SEED (symmetric/predictable)
- TEMP lane → FREQ delta + ZSTD19 (asymmetric/temporal)
- Expected: ratio → ~1.4x

**O2 — Face-balanced H64 feed**
- Real theta_map distributes face non-uniformly
- Need: track path_id per face (not per lane globally)
- `path_per_face[12]` counter array in PipeState

**O3 — drift response**
- gate==2 (drift detected) → currently ignored
- Should: trigger ghost zone write (h64_commit_ghost)
- 693 drift events = 693 potential ghost slots to exploit

**O4 — S5 multi-cycle real image**
- verify _c000/_c001 + manifest + seed_local no drift across cycle

### Key files for next session
- test_h64_diamond.c  ← main pipeline (start here)
- pogls_hilbert64_encoder.h + pogls_to_geopixel.h ← H64 system
- fibo_layer_header.h + fibo_tile_dispatch.h ← Fibo layer clock
- hamburger_encode.h ← has FREQ+ZSTD19 post-pass (S4 done)

### Sacred numbers reminder
- 27 = 3³ Hilbert fold
- 144 = Fibo closure (layer epoch)
- 720 = TRing slots (12 face × 60)
- 64 = DiamondBlock size = 1 chunk = 1 H64 slot
- 4B = invert tile storage (seed only)

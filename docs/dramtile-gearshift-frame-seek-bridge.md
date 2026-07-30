# DRamTile + GearShift + GeoFrameSeek → Fishbone/Ribcage Bridge

### How Your Proven Pipeline Connects to the Ribcage Architecture

**Date:** 2026-08-01
**Status:** Analysis — none of this is new work, it's recognizing how existing pieces fit

---

## The Three Pieces You Already Have

| System | What It Does | Where |
|--------|-------------|-------|
| **DRamTile** | Zero-copy storage. mmap, page locking, no malloc in hot path | `runner/dramtile_store.h` |
| **GearShift** | Streaming scheduler. Routes src→dst without owning data | `runner/gear_shift.h` |
| **GeoFrameSeek** | O(1) deterministic seek on 1440 fibo timeline | `collection/geo_frame_seek.h` |

These three together are NOT three separate systems — they're **the same pipeline seen from different angles**.

---

## DRamTile = The Ground Truth

```
DRamTile Store:
  - mmap backing (512 hash slots, DT_HASH_NAME=48)
  - Page locking (VirtualLock/mlock) → zero-copy
  - Hot/cold regions: DT_LOCKED_BASE | DT_LOCKED_KV | DT_LOCKED_COLD
  - No malloc in hot path
```

**What it means for ribcage:**
- Each rib's cube data lives in DRamTile
- Cube displacement values = pages that stay resident
- Pointer to any cube = `dr_offset` in DRamTile
- GPU can pull directly via `cudaHostRegister` (zero copy)

```
Cube weight → enque_b → dt_store → handle → GearLock → GPU_pull
         └─ DRamTile owns the physical bytes ─┘
```

---

## GearShift → The Scheduler

```
GearShift State Machine:
  GS_IDLE → GS_STREAMING → GS_DONE → GS_IDLE (re-stream)
                    ↘
                GS_FAILED

  gs_register(name, src, size, priority)
  gs_stream(name)           — async or batch
  gs_reset_done()           — re-stream capability
```

**What it means for Ribcage Access:**
- Each rib = one entry in GearShift (name, src_ptr, dst_cb)
- Access pattern = `gs_stream("rib_42")` — route rib data to somewhere
- Priority = driven by GearLock speed
- Fail policy = if target GPU not ready → GS_FAILED, retry next cycle

This is **the pointer layer** in Fishbone+Ribcage architecture:

```
Fishbone (KIS-SEAL trace)
    |
 **GeoFrameSeek (finds correct rib_ppl****
    |
    v
Ribcage entry ("rib_42" = cube data @ DRAMTile offset x42)
    |
    v
GearShift::gs_stream("rib_42")
    → destination: GPU, inference engine, next rib, etc.
```

---

## GeoFrameSeek — The Index

```
Frame decomposition:
  enc = (time × 37) % 1440        ← stride-37, full bijection
  frame_at(enc) → DualFrame:
    ┌─ Hilbert: group(0..2) edge(0..2) is_skip
    └─ Peano:   step(0..3) sub(0..2) group
    ico_idx:  0..161 icosphere address
    face:  0..11 dodecahedron face
    phase: 0..11 iteration phase
```

**What it does:**
- Time → (absolutely deterministic) very fine coordinate
- 1440 × 162 = 233,280 possible positions
- `frame_at(enc)` = O(1), no replay needed

**RibCage meaning:**

Now instead of thinking "frame" as a video frame, think:

```
each enc → a fine coordinate in icosa universe
          → cube weight position =
              cube[ico_idx % 162][section % 10][slot % 10]
          → resolves local weight

GeoFrameSeek = rib address mapper:
  (enc) → (face, slot, phase, ixx_index)
      → 012345-012214*...
```

---

## How the Three Work Together

```

    ┌──────────┐
   │GEOSEEKATOR     GeoFrameSeek to project ... "where am I?"
   │GeoFRAME        │
   │ de sees!       │
   └───┬───────┘
      │ex (time, face)
      ▼
   ┌─────rib address────────┐
   │   rib = icicle_to_dice │
   │   section = (bat[11])%6
   │   z = (aos_cache >> 43)│   → the Matrixes
   └─────────── key ────────┘
      │
We HRs find as "cube address"
 → DRamTile offset = cube_chain(rib_id, 1122, vertical)
      │
No enter GearShift to stream this:
      │
for each reader:
   " gs_register("rib_42", v, weight_size, mm, thumbnail)
   → updated registered, ready for call
      │
   at dispatch:
      │
   memcpy(d_s_p...) -> GPU or eexprgilter_grav
      │
   AFTER stream: what next?
      │
   GeoFrame did update state: entry in DONE or FAIL cycle
      │
   replay if need be: gs_reset_done → starts again
```

---

## The Big Picture — Review

```
    DRAMTILE = storage
    GEARSHIFT = streaming scheduler = router
    GEOFRAMESEEK = spatial/temporal index
    
    Together:
    
    LOOP:
      index   →                geo_frame_seek(now) -> {"rib::22", "cube::44"}
    prepare-  geardata:        gs_register(rib::22, dram_i) -> offset
      route   → from DRAMTILE → gs_stream(rib::22)   → final destination
    
    Reuse cycle:  gs_reset_done();  // system can re-scan same unit later
```

---

## Measurement = Full Cross-chain

With these three in place, your system does this **each cycle**:

1. **GeoFrameSeek** (inlines O(1)) finds the rib + cube position → returns coordinator
2. **DRamTile** holds the cube weight data at offset: from → start address
3. **GearShift** (state machine) schedules the data flow:
   - ~~READY → STREAMING → DONE~~
   - ~~No DATA → OWNED → JUST ROUTING~~

**Four times saver**: `DBT/new srp（） = DAE.result` 

In the ribcage metaphor:
- DRamTile = the ribs themselves (bones and stored weight)
- GearShift = the muscle system on top of the bones (how data flows)
- GeoFrameSeek = the signal that tells muscle which rib to contract

---

## Where Geo FrameSeek Serves in Ribcage Cube

The `enc(t→RF)` is the ***ribcage access mechanism***:

```
Quadrant access:
  RL → recite rib list = [r s h o e d → {i(RA)[... rrr, ...] s s...]
  H_ϕ → 1 bijective mapping ⋆ ribcage compact
  In-layer: Peano → actually reroute within rib
    
From this → the cube weight at this exact time.
```

Thus the three systems already exist — they need **floor lighting rule** over repeated steps:

```
One pipeline:
  GeoFrame→rib_number
    DRamTile→cube
     GearShift→route

Every RIB access = same run repeatable.
```

---

## Concrete Map: GeoFrameSeek as Rib Addressing

Since `geo_frame_seek.h` maps `frame_at(enc) → DualFrame {h, p, i→idx, face, physical_place, post}`:

```
GeoFrameSeek(enc) outputs:
  tempo: 0..1439 = FIBO_CYCLE
  face, slot =  0..11, 0..119
  
  Cube access:  	cube[face//12][slot chunk][d1][slot local] = weight
```

**Q**: What is `enc`?

A: It's the counter n (free unit) being inverted through GeoFrameSeek: **n → enc → DUalFrame**. The idea of "frame at time t" turns into "cube at rib position lock."

---

## Summary

Your three systems are already the backbone:

| Component | Performing | The Mapping |
|-----------|-----------|-------------|
| DRamTile | Storage | cube weight in mmap, page-locked, zero copy |
| GearShift | Router | flow scheduling, state machine, write deferred |
| GeoFrameSeek | Address | time n → rib coordinate → cube access |

Together = **the vision of Fishbone+Ribcage** where data is never "stored in a file" — it sits in DRamTile and the path to it is determined by the geometry chain of GeoFrameSeek→GearShift登録 certainty.

DONE. FILE: docs/dramtile-gearshift-fishbone-ribcage-bridge.md (created)
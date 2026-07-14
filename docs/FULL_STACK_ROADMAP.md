# GeoField Full Stack Roadmap

**Created**: July 14, 2026  
**Target**: 500+ MB/s with 15× compression ratio  
**Status**: Phase 1 in progress (Python+C DLL hybrid at 4 MB/s)

## Overview

The GeoField pipeline achieves ≥15× size reduction through geometric structuring, not compression. Full stack integration with GPU, DRamTile, and GearShift will bring throughput from 4 MB/s to 500+ MB/s.

## Performance Projection

```
Stage              Speed      Ratio    Status
─────────────────────────────────────────────────
Python+C DLL       4 MB/s     15×      ✅ ทำแล้ว
C native           20 MB/s    15×      🔨 Phase 1
+CUDA              200 MB/s   15×      ⏳ Phase 2
+DRamTile          500 MB/s   15×      ⏳ Phase 3
+GearShift         500+ MB/s  15×      ⏳ Phase 4
```

## Phase 1: Port Full Pipeline to C

**Goal**: All 9 stages in C DLL, no Python dependency  
**Target**: 20 MB/s throughput  
**Status**: IN PROGRESS

### Checklist

- [x] flow_chunk (adaptive chunking) — DLL ✅
- [x] diamond_classify — DLL ✅
- [x] skel_decide — DLL ✅
- [x] wallet_chunk_seed — DLL ✅ (10× faster)
- [ ] LetterCube bond/assemble → C
- [ ] CubeCtx from LetterCube → C
- [ ] Cube promote (6→1 recursive) → C
- [ ] Goldberg sphere mapping → C
- [ ] Fibonacci shell fold → C

### Key Files

- `pipeline/geofield_pipeline.c` — C DLL wrapper
- `pipeline/geofield_ctypes.py` — Python ctypes bindings
- `pipeline/lettercube.h` — LetterCube data structure
- `pipeline/cube_assembly.h` — Cube assembly (Phase C1)
- `pipeline/goldberg_sphere.h` — Goldberg sphere mapping (Phase C2)
- `pipeline/fibonacci_fold.h` — Fibonacci shell fold (Phase C3)

### Benchmark (Current)

```
63KB file:
  flow_chunk           1.40 ms
  diamond_classify     1.99 ms  (1.9 µs/block)
  skel_decide          2.35 ms  (2.2 µs/block)
  lettercube           0.46 ms  (3.0 µs/seg)
  cube_ctx             1.48 ms  (9.7 µs/seg)
  cube_promote         0.01 ms  (11.2 µs)
  goldberg_map         0.003 ms (O(1))
  fibo_fold            0.17 ms  (1.1 µs/seg)
  ──────────────────────────────
  TOTAL               15.8 ms   3.8 MB/s
```

## Phase 2: CUDA Kernel

**Goal**: Parallel classify + route on GPU  
**Target**: 200 MB/s throughput  
**Status**: PENDING

### Kernels to Implement

1. **Diamond Shell classify** — parallel per-block (2 µs/block → 0.1 µs/block)
2. **Skeleton decide** — parallel per-block
3. **Tantrix route** — parallel per-chunk
4. **Wang tile validate** — parallel per-window
5. **Cube build** — parallel per-voxel

### Dependencies

- `collection/src/icosa_twin_bridge.cu` — existing GPU bridge
- CUDA 12.6 (verified available)
- GTX 1050 Ti (4GB VRAM)

## Phase 3: DRamTile Zero-Copy

**Goal**: No memcpy between stages  
**Target**: 500 MB/s throughput  
**Status**: PENDING

### Implementation

- DRamTile store for GeoField chunks
- Zero-copy read/write (pointer swap)
- Cold spill for large datasets
- mmap-backed persistent store

### Key Files

- `runner/dramtile_store.h` — DRamTile API
- `runner/dramtile_twin.h` — Twin buffer management

## Phase 4: GearShift Priority Streaming

**Goal**: Priority queue → GPU stream  
**Target**: 500+ MB/s sustained  
**Status**: PENDING

### Implementation

- GearShift router for pipeline stages
- Priority scoring (hot/cold data)
- Async GPU upload via icosa_bridge
- Pipeline overlap (CPU classify + GPU route)

### Key Files

- `runner/gear_shift.h` — GearShift streaming router
- `runner/gear_lock.h` — GearLock state machine

## Phase 5: Full Integration + Benchmark

**Goal**: End-to-end verification  
**Status**: PENDING

### Checklist

- [ ] End-to-end encode/decode roundtrip
- [ ] Verify 15× compression ratio on real data
- [ ] Benchmark against zstd/lz4/brotli
- [ ] Stress test (1MB, 16MB, 1GB files)

## Phase 6: CLI + GUI + Production

**Goal**: User-facing tools  
**Status**: PENDING

### Deliverables

- CLI: `geofield encode/decode/info/batch`
- GUI: tkinter wrapper (encode/decode/verify)
- Package: standalone exe + DLL
- Documentation: user guide + API reference

## Key Design Decisions

1. **Chunk size is adaptive** — determined by geometry via fold_fibo_intersect
2. **LetterCube uses 24 pairs** — matching dual dodeca 24 pentagon faces
3. **Compression ≠ structuring** — Steps 1-5 are structuring, compression at geopixel tile codec
4. **GEOM subtypes** for extensible evolution: INLINE (65B), BLUEPRINT (≤24B), PARAMETRIC (≤12B), TEMPLATE (≤8B)
5. **Goldberg sphere**: cube shell → remap to hexagon → subdivision on GP(n,0)
6. **Fibonacci shell fold**: layers exist only on fibo clock ticks (F(0)=1 always → F(11)=144 rare)

## References

- Card #65 on cross-session board
- Memory id=357 (3-Layer Architecture)
- Memory id=358 (Full Stack Roadmap)
- `docs/PIPELINE_ARCHITECTURE.md` — Architecture plan
- `docs/geom-blueprint-spec.md` — GEOM_BLUEPRINT evolution spec

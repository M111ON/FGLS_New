# FGLS: Geometric Computing for Tensor Compression

## A Technical Reference

**Version:** 0.2 (Draft)  
**Date:** July 30, 2026  
**Status:** 🚧 In Progress — flagged sections need additional data

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Philosophy: Geometry as Computation](#2-philosophy-geometry-as-computation)
3. [The FGLS Pipeline](#3-the-fgls-pipeline)
4. [Core Components](#4-core-components)
5. [Geometric Addressing](#5-geometric-addressing)
6. [The GeoPixel Codec](#6-the-geopixel-codec)
7. [GeoField: Weight Observation](#7-geofield-weight-observation)
8. [DRamTile: Zero-Copy Storage](#8-dramtile-zero-copy-storage)
9. [GPU Pipeline](#9-gpu-pipeline)
10. [Implementation History](#10-implementation-history)
11. [Appendix: API Reference](#11-appendix-api-reference)

---

# 1. Introduction

## 1.1 What is FGLS?

FGLS (Field-Guided Load System) is a geometric computing framework for tensor storage, compression, and inference acceleration. Instead of treating model weights as flat arrays to be compressed, FGLS maps them into geometric coordinate spaces where structure emerges from position rather than algorithms.

## 1.2 Core Principle

> "MAP not COMPRESS" — เปลี่ยนมิติการเข้าถึงข้อมูล ไม่บีบ payload

Traditional compression asks: "How do I make this smaller?"  
FGLS asks: "What geometric structure does this data already have?"

## 1.3 Key Results

| Metric | Value | Notes |
|--------|-------|-------|
| Lossless reconstruction | 600/600 cells | Contour Mask |
| Cross-mask correlation | 0.0002 avg | Masks see genuinely different data |
| Storage overhead | 1.0x | Partition, not compression |
| Geometric addressing | O(1) | Deterministic, no hash collisions |
| GPU bandwidth (T4) | 18.23 GB/s | HBM direct via Jet Puller |

---

# 2. Philosophy: Geometry as Computation

## 2.1 The Paradigm Shift

### Von Neumann (Sequential)
```
1 → process → 2 → process → 3 → process → 4
   ↑ step       ↑ step       ↑ step
   data นิ่ง     ALU ทำงาน    result ย้าย
```

### Geometric (Structural)
```
1, 2, 3, 4

pointer → geometry → values
   ↑ implicit     ↑ inherent
   position       adjacency
   layer          transpose
   shift          distance
```

**Geometry = multi-dimensional LUT where process is built into structure.**

## 2.2 What Geometry Provides

| Operation | 1D LUT | Geometric LUT |
|-----------|--------|---------------|
| lookup | O(1) | O(1) |
| neighbor read | O(n) scan | O(1) by (θ±1, φ) |
| batch read | sequential | 6-direction parallel |
| sign variation | need extra bit | transpose = free |
| distance calc | subtract | XOR = 1 cycle |
| permutation | gather index | rotate (θ, φ) |
| wave modulation | per-element | radius = continuous |

> 🚩 **FLAG:** Need more examples of geometric operations in practice. Current documentation is theoretical — need benchmark comparisons showing real-world speedup.

---

# 3. The FGLS Pipeline

## 3.1 Pipeline Overview

```
File → Chunk[Bond] → Arrange[sequence+header] → GeoPixel[Hilbert maze grid+Hamburger+GeoFrameSeek+etc] → Split(header-structure to CPU, payload to GPU) → CPU steering, GPU batch process → all zero-copy through DRamTile+Gear
```

## 3.2 Pipeline Stages

| Stage | Input | Output | Description |
|-------|-------|--------|-------------|
| **Bond** | Raw bytes | Chunks with geometric address | Pair bytes into nibble bonds |
| **Arrange** | Chunks | Sequence with headers | Hilbert z-order sort |
| **GeoPixel** | Sequence | Geometric tiles | Map to icosahedron faces |
| **Split** | Tiles | CPU headers + GPU payload | Separate steering from data |
| **Pull** | GPU memory | Results | Gear Lock bandwidth pull |

## 3.3 Key Insight

CPU = index/schedule (steering)  
GPU = bandwidth pull (processing)

Zero-copy throughout: DRamTile + Gear architecture.

> 🚩 **FLAG:** Need detailed timing breakdown for each stage. Current benchmarks are aggregate only.

---

# 4. Core Components

## 4.1 pogls_core — Foundation Layer

**Files:** `pogls_core/pogls_platform.h`, `pogls_core/pogls_core.h`, `pogls_core/pogls_addr.h`

### Platform Abstraction

Cross-platform file I/O and memory management:

```c
void* pogls_map_file(const char *path, size_t *size_out);  // mmap
void* pogls_alloc_large(size_t size);                       // VirtualAlloc
int64_t pogls_fsize(const char *path);                      // File size
uint32_t pogls_xorshift32(uint32_t *state);                 // PRNG
```

### Address System

Geometric addressing for tensors:

```c
uint64_t pogls_tier_capacity(uint8_t tier);     // Capacity per tier
uint8_t pogls_select_tier(uint32_t tensor_count, uint32_t hidden_dim);
PoglsAddrDecomp pogls_decompose(uint32_t addr, uint8_t tier);
uint32_t pogls_compose(uint32_t macro, uint32_t micro, uint8_t tier);
```

### Store System

Memory-mapped key-value storage:

```c
PoglsStore* pogls_store_open(const char *path, uint64_t capacity);
int  pogls_store_put(PoglsStore *store, const char *name, const void *data, size_t sz);
void* pogls_store_get(PoglsStore *store, const char *name, size_t *sz_out);
```

## 4.2 pogls_compress — Compression Layer

**File:** `pogls_core/pogls_compress.h`

```c
uint32_t pogls_compress(uint8_t *dst, size_t dst_cap,
                        const uint8_t *src, size_t src_sz);
uint32_t pogls_decompress(uint8_t *dst, size_t dst_cap,
                          const uint8_t *src, size_t src_sz);
```

> 🚩 **FLAG:** Compression algorithm details needed. Is this ZSTD-based? Custom? Need algorithm specification.

## 4.3 pogls_meta — Metadata System

**File:** `pogls_core/pogls_meta.h`

```c
void pogls_meta_header_init(PoglsStoreHeader *hdr);
uint64_t pogls_meta_data_off(const PoglsStoreHeader *hdr);
int pogls_meta_read(const char *path, ...);
int pogls_meta_write(const char *path, ...);
```

## 4.4 pogls_loader — Standalone Loader

**File:** `pogls_core/pogls_loader.h`

Zero-dependency API for loading POGLS files:

```c
PoglsLoader *L = pogls_open("model.pogls");
void *data = pogls_tensor_data(L, "blk.0.attn_q.weight");
size_t sz = pogls_tensor_size(L, "blk.0.attn_q.weight");
pogls_close(L);
```

Features:
- mmap-based zero-copy loading
- O(1) tensor lookup by name or address
- Per-tensor decompression (ZSTD/RAW)
- Thread-safe read-only access

---

# 5. Geometric Addressing

## 5.1 The Field

```
Field size: 20736 slots (12⁴ = full cycle)
Center:     10368 (zero point)
Ceiling:    20735 (max positive)
Floor:      0 (max negative)
```

## 5.2 Weight ↔ Position Mapping

```c
// Encoding (weight → position)
position = CENTER + weight

// Q8 weights (-128..+127):
// position range: 10240..10495
// only 256 slots used out of 20736

// Decoding (position → weight)
weight = position - CENTER
```

## 5.3 Icosahedron Structure

The field maps to an icosahedron with:
- 12 faces
- 120 slots per face
- 144 phases

```
12 faces × 120 slots × 144 phases = 20736
```

## 5.4 Address Decomposition

```c
PoglsAddrDecomp {
    uint32_t macro;    // Face (0-11)
    uint32_t micro;    // Slot within face (0-119)
    uint8_t  tier;     // Resolution level
}
```

> 🚩 **FLAG:** Need mathematical proof that address decomposition is bijective. Current documentation assumes it works but doesn't prove no collisions.

---

# 6. The GeoPixel Codec

## 6.1 Overview

GeoPixel maps tensor data to geometric tiles on an icosahedron, enabling:
- Deterministic addressing (O(1) lookup)
- Lossless reconstruction
- Natural parallelism for GPU

## 6.2 Frame Seek (Stride-37 Walk)

**File:** `geopixel/include/geo_frame_seek.h`

```c
// Timeline: 1440 positions (fibo cycle)
enc(t) = (t × 37) % 1440     // stride-37 walk, full bijection
seek(enc) → frame O(1)       // no replay needed
next(enc) → (enc + 37) % 1440
```

Sacred constants (FROZEN):
- `TRING_WALK_CYCLE = 1440` (12 × 120)
- `TRING_WALK_STRIDE = 37` (prime, gcd(37,1440)=1)
- `META_FACE_SZ = 120` (slots per face)
- `FRAME_EDGES = 12` (9 Hilbert + 3 Peano)

## 6.3 Geo Jump

**File:** `geopixel/include/geo_jump.h`

Maps Hilbert/Peano curves to icosahedron addresses:

```c
static inline uint32_t _jump_hilbert(uint32_t node, uint32_t col, 
                                      uint32_t row, uint32_t floor);
static inline uint32_t _jump_peano(uint32_t node, uint32_t col, 
                                    uint32_t row, uint32_t floor);
```

## 6.4 Hex Tile Encoding

**File:** `geopixel/include/hex_tile.h`

7-cell hexagonal tiles for local structure:

```c
#define HEX_CELLS    7
#define HEX_CENTER   6
#define HEX_RING     6

static inline int hex_tile_encode(const HexTile *t, uint8_t *dst);
static inline int hex_tile_decode(const uint8_t *src, size_t src_len, HexTile *t);
```

Encoding modes:
- `HENC_FLAT` (0x00) — All cells equal
- `HENC_TRIPLET_FLAT` (0x01) — Center + 3 pairs
- `HENC_GRADIENT` (0x02) — Radial gradient

## 6.5 POGLS → GeoPixel Bridge

**File:** `geopixel/include/pogls_to_geopixel.h`

Path A: Pure seed storage — every tile stores seed only (4B).

```
ScanEntry stream
    ↓  pogls_hilbert64_encoder.h
HilbertPacket64  (64 cells, RGB balanced, invert derived)
    ↓  THIS FILE
H64TileIn[]  →  hamburger CODEC_SEED path
    ↓
.gpx5 output:  4B × 64 tiles = 256B per packet
```

### Storage per packet
- positive tiles  : 48 × 4B = 192B
- invert tiles    : 12 × 4B =  48B (derived, verify-only)
- ghost tiles     : 0..4 × 4B
- header          : 16B (one per file)

### GeoPixel v21/v25 (Goldberg Integration)

**File:** `geopixel/src/geopixel_v21_o25.c`

Goldberg Full Integration features:
- Point 2: Blob dedup via stamp hash (XOR fold of blob bytes)
  - Duplicate tiles → ref-pointer in index (bit31 set)
  - Decoder resolves ref-pointer transparently
  - Dedup table: 4096 slots, 16-probe linear hash
- Point 3: Pure circuit_fired replaces all threshold logic
  - n_circuits 0-2 → GRAD9_NORMAL (avg_var ~215)
  - n_circuits 3-5 → GRAD9_LOOSE (avg_var ~311-437)
  - n_circuits 6   → DELTA (avg_var ~575)

> 🚩 **FLAG:** Need explanation of Hamburger codec. Current docs reference it but don't explain the algorithm.

---

# 7. GeoField: Weight Observation

## 7.1 Contour Mask System (Updated Jul 30, 2026)

A geometric observation tool that measures weight tensor structure through independent directional profiles. Silk Screen ≡ Contour Mask — same architecture.

### Concept
Inspired by the contour gauge (profile gauge):
- **Mask** = 3D volume per face (W×H×L)
- **Weight** = displacement from 0 (position = value)
- **Read** = XOR(pos, 0) = pos = weight → lossless 100%

### Architecture: W×H×L × faces
```
W×H×L = 3D volume per face (depth ON the mask)
faces = 6 (cube: A,B,C,D,E,F) or 12 (dodecahedron)
Total units = W × H × L × faces

Example: 10×10×10×6 = 6,000 units (6F)
         10×10×10×12 = 12,000 units (12F)
```

### Displacement Model
```c
// Encode: weight → position from 0
position = weight;  // weight 42 → position 42

// Decode: XOR(position, 0) = position = weight
weight = position ^ 0;  // O(1), lossless
```

### Direction Configuration (Verified Aug 1)
```
All C(6,2) = 15 pairs are independent
Opposite faces do NOT cancel (verified test_opposite_cancel.py)

A↔B, C↔D, E↔F = independent (NOT cancel)
No special treatment for any pair
Config upfront: declare pairs before encoding
```

### Performance (C Benchmark)
| Metric | Value |
|--------|-------|
| Encode | 1.12 ns/sample |
| Decode | O(1) — XOR(pos, 0) = identity |
| Lossless | 100% (verified 1M samples) |
| Storage | 6,000 × 1 byte = 5.9 KB (6F) |

### Comparison: Cube (6F) vs Dodecahedron (12F)
| Metric | Cube (6F) | Dodeca (12F) | Ratio |
|--------|-----------|--------------|-------|
| Faces | 6 | 12 | 2x |
| Edges | 12 | 30 | 2.5x |
| Vertices | 8 | 20 | 2.5x |
| Units | 6,000 | 12,000 | 2x |
| ROI | — | — | 2.5x viewpoints ÷ 2x cost |

### Timeframe
System has its own time dimension = L (depth ON the mask). No fibo tick, no stride-37. Clock = simple counter.

### Results
| Metric | Value |
|--------|-------|
| Lossless | 100% (verified) |
| Storage | 1.0x |
| Cross-mask correlation | 0.0002 avg |
| Opposite pairs cancel | NO (verified Aug 1) |

## 7.2 Fabric Wire

**File:** `collection/geopixel/geofield/fabric_wire.h`

> 🚩 **FLAG:** Need documentation on fabric_wire purpose and API. Currently only header comments exist.

## 7.3 Frustum Layout

**Files:** `frustum_gcfs.h`, `frustum_layout_v2.h`, `frustum_slot64.h`, `frustum_trit.h`

Geometric frustum for weight observation:

### GCFS File Format

GCFS = GiantCube FrustumStore — output format for frustum pipeline.

File layout (4896B total = 288×17):
```
[0    ..3455]  data zone  — 54 × 64B DiamondBlocks (verbatim copy)
[3456 ..3464]  coset_mask — 9B (one byte per coset, reserved_mask[0..8])
[3465 ..3490]  letter_map — 26B (A..Z, caller-supplied)
[3491 ..3498]  slope      — 8B  (uint64_t last slope fingerprint)
[3499 ..3502]  merkle_root— 4B  (XOR of all slot core[0..3])
[3503 ..4895]  _pad       — 1393B zeros (boundary reserve)
```

4896 = 2⁵×3²×17 — factor 17 ∈ FACE_PRIME {7,11,13,17,19,23}
- File boundary unreachable by pure 2ⁿ×3ᵐ arithmetic (security seam)
- Metadata zone = 1440B = 2⁵×3²×5 (has factor 5 = intentional marker)

API:
```c
gcfs_serialize(FrustumStore *fs, uint8_t out[4896]);
gcfs_deserialize(uint8_t in[4896], FrustumStore *fs);
gcfs_merkle_verify(FrustumStore *fs);  // verify merkle_root
```

No malloc. No float. No heap.

> 🚩 **FLAG:** Need explanation of frustum geometry and how it relates to tensor structure.

---

# 8. DRamTile: Zero-Copy Storage

## 8.1 Architecture

DRamTile maps model weights into a large mmap'd region addressed by geometric coordinates.

**Core principle:** "RAM is disk, disk is RAM."

```
┌─────────────────────────────────────┐
│  Weight Region (file-backed)        │  ← persisted on disk
│  dt_put() / dt_putv()               │
├─────────────────────────────────────┤
│  KV Region (anonymous)              │  ← ephemeral
│  dt_put_kv()                        │    lost on process exit
└─────────────────────────────────────┘
```

## 8.2 Address Scheme

```c
name → FNV-1a hash → anchor / x / y / layer → dram_addr(anchor, x, y, layer)
```

Result: 16-bit value (0–20735) encoding 4D coordinate.

## 8.3 Hash Table

```c
slot = dram_addr % DT_HASH_SLOTS  (512 slots)
```

| Field | Type | Description |
|-------|------|-------------|
| `dram_addr` | `uint32_t` | Geometric address (0 = unused) |
| `offset` | `size_t` | Byte offset in mmap region |
| `size` | `size_t` | Stored byte count |

## 8.4 Gear Lock

**File:** `collection/src/gear_lock.h`

Bandwidth pull mechanism for GPU:

```c
// 1728 pipes × 12 ticks
// tick 11: bridge → residual → tick 13
```

> 🚩 **FLAG:** Need detailed Gear Lock timing diagram and bandwidth analysis.

---

# 9. GPU Pipeline

## 9.1 GPU Jet Puller

**File:** `runner/gpu_jet_puller/gpu_jet_puller.cu`

### Architecture

```
CPU = steering (index/schedule)
GPU = bandwidth pull (processing)
```

### Benchmarks

| Platform | Bandwidth | Pulls | Errors |
|----------|-----------|-------|--------|
| Local 1050 Ti | 3.29 GB/s | - | - |
| Colab T4 HBM | 18.23 GB/s | 7.1M | 0 |

### Key Insight

HBM direct (cudaMalloc) vs PCIe hostRegister:
- HBM: 18.23 GB/s
- PCIe: 7.23 GB/s
- **2.5× faster** with HBM

## 9.2 Diamond Shell

**File:** `collection/src/diamond_shell_v2.h`

Classification by `pc` (fold_fibo_intersect popcount), not `nz` (non-zero count).

```c
_chunk_to_pure_block()  // Pure content DiamondBlock with NO metadata override
```

> 🚩 **FLAG:** Need Diamond Shell algorithm details. Current docs mention it but don't explain the geometric invariants.

---

# 10. Implementation History

## 10.1 Timeline

| Date | Milestone | Key Files |
|------|-----------|-----------|
| Jun 15 | Project start | - |
| Jun 20 | End-to-end validation (SID + Cosplay) | `runner/sid_*.c` |
| Jul 13 | Pipeline glue exploration | `pipeline_glue.h` |
| Jul 14 | GFCS codec fix, GPXL v3-v4 rewrite | `geopixel/` |
| Jul 15 | Full pipeline connection | `geofield_full.c` |
| Jul 24 | Beam Addressing system | `beam_addressing/` |
| Jul 25 | Blueprint compression, Shape benchmark | `runner/blueprint_*.c` |
| Jul 28 | GPU Jet Puller v1, Pipeline integration | `gpu_jet_puller.cu` |
|| Jul 29 | Diamond Shell v3 fix, Pre-built verification | `diamond_shell_v2.h` ||
| Jul 30 | Contour Mask V2, Model scan (197 tensors) | `contour_mask_v2.c` ||
| Jul 30 | Contour Mask unification, Displacement model, 12F dodecahedron | `bench_contour_mask_c.c` ||
| Jul 30 | Timeframe = L dimension, Skill consolidated | `geometric-weight-storage` ||

## 10.2 Key Lessons

### Lesson 1: Sequential = Wrong Architecture
> "Sequential chunk = wrong architecture" — pipeline must be geometric, not linear.

### Lesson 2: MAP not COMPRESS
> "คิดจะบีบ = ไปผิดทางทันที" — เปลี่ยนมิติเข้าถึงข้อมูล ไม่บีบ payload

### Lesson 3: Geometry IS Runtime Structure
Geometry is not just shape/polygons — it's:
- Address Space
- Coordinate Space
- Transformation
- Symmetry
- Topology
- Index Mapping

## 10.3 Failed Approaches

| Approach | Why Failed | Lesson |
|----------|------------|--------|
| Sequential chunking | No geometric structure | Pipeline must be spatial |
| ZSTD-only compression | Metadata overhead > savings | Use geometry, not algorithms |
| Hash-based addressing | Collisions under load | Deterministic geometry = O(1) |

> 🚩 **FLAG:** Need more detail on failed approaches. Current list is incomplete.

---

# 11. Appendix: API Reference

## 11.1 Core API

| Function | File | Description |
|----------|------|-------------|
| `pogls_map_file` | platform.c | mmap file |
| `pogls_alloc_large` | platform.c | VirtualAlloc |
| `pogls_tier_capacity` | addr.c | Capacity per tier |
| `pogls_compress` | compress.c | Compression |
| `pogls_decompress` | compress.c | Decompression |
| `pogls_store_open` | store.h | Open KV store |
| `pogls_store_put` | store.h | Insert key-value |
| `pogls_store_get` | store.h | Retrieve value |

## 11.2 GeoPixel API

| Function | File | Description |
|----------|------|-------------|
| `geo_jump` | geo_jump.h | Hilbert/Peano to icosahedron |
| `geo_frame_seek` | geo_frame_seek.h | Deterministic frame lookup |
| `hex_tile_encode` | hex_tile.h | 7-cell hex encoding |
| `hex_tile_decode` | hex_tile.h | 7-cell hex decoding |

## 11.3 DRamTile API

| Function | File | Description |
|----------|------|-------------|
| `dt_open` | geo_dram_tile.h | Open store |
| `dt_put` | geo_dram_tile.h | Store weight |
| `dt_get` | geo_dram_tile.h | Retrieve weight |
| `dt_put_kv` | geo_dram_tile.h | Store KV cache |

> 🚩 **FLAG:** Complete API reference needed. Current list is partial.

---

# Glossary

| Term | Definition |
|------|------------|
| **FGLS** | Field-Guided Load System |
| **GeoPixel** | Geometric pixel mapping to icosahedron |
| **DRamTile** | Zero-copy geometry-addressed tensor store |
| **Gear Lock** | GPU bandwidth pull mechanism |
| **Diamond Shell** | Geometric weight classification |
| **Contour Mask** | Directional weight observation — W×H×L×faces, displacement model (Jul 30) |
| **Stride-37** | Fibonacci walk on 1440 timeline (ADDRESSING only, not weight generation) |
| **Bond** | Nibble pairing for geometric addressing |

---

# References

1. `docs/DEVELOPMENT_SUMMARY.md` — SID + Cosplay + Experiment Pipeline
2. `docs/FULL_STACK_ROADMAP.md` — GeoField Full Stack Roadmap
3. `docs/fibo_tick_architecture.md` — Three-View Entropy Container
4. `docs/dramtile.md` — DRamTile Architecture
5. `docs/geometric-beam-encoding.md` — Beam Encoding System
6. `docs/geometric-computing-paradigm.md` — Geometric Computing Paradigm

---

*Document generated from prose documentation and workspace analysis.*  
*Flagged sections (🚩) require additional data from implementation or benchmarks.*

# Bermuda Geometry Store — Handoff Document

## What is this?

A **geometry-addressable weight store** for neural networks.

Instead of loading a model's weights by layer index (layer 0, layer 1...), we route each weight vector through a **Hilbert curve geometry encoder** and index it by its **geometry key (zone, shape)**. Once indexed, any input with the same geometry can find its matching weights in **O(1)** — no scanning, no full model load needed.

---

## ✅ What's Done (Working Now)

### Core Engine (`collection/`)

| File | What it does |
|------|-------------|
| `bermuda_reshape_v3.py` | Hilbert curve geometry encoder. 4 gears, traverse modes (ORBITAL/CHIRAL/CROSS/HUB), codebook, gate, CROSS_LUT. |
| `bermuda_router_v1.py` | Float tensor → geometry → routing verdict (zone, shape, polarity, tring_slot per token). |
| `bermuda_block.py` | DiamondBlock serialization: routing entries packed into 64B blocks + optional shadow (full float32 values stored for reconstruction). |
| `bermuda_export.h/.c` | C ABI for geometry ops (gear_snap, Hilbert, traverse, zone, route_batch). Compiled as `pogls_bermuda.dll`. |

### Bond Layer (`collection/`)

| File | What it does |
|------|-------------|
| `pogls_bond.h` | Bond layer: piece creation, bond_verify (32-bit double-pass fibo), slot creation, plug_connect. |
| `pogls_bond_export.h/.c` | C ABI for bond ops. Compiled as `pogls_bond.dll`. |
| `python_src/bermuda_bond_bridge.py` | Unified ctypes bridge for both DLLs. Exposes all geometry + bond ops to Python. |
| `python_src/bermuda_pipeline.py` | Full pipeline: route → _verdict_to_pieces → _pieces_to_slots → bond verify. |

### Pre-trained Models (`collection/build/`)

| File | Description |
|------|-------------|
| `bermuda_gate_g2_d128.pt` | Trained gate (gear 2, code_dim=32, dim=128). Used for geometry classification. |
| `bermuda_gate_g2_d768.pt` | Larger gate (dim=768) for higher-dim models. |
| `bermuda_gate_g4_d768.pt` | Gear 4 gate for high-precision routing. |
| `pogls_bermuda.dll` | Compiled geometry engine (51KB). |
| `pogls_bond.dll` | Compiled bond layer (v1.1.0, 32-bit verify mask). |

### Tests & Demos (`collection/`)

| File | Status |
|------|--------|
| `test_bermuda_e2e.py` | 44/44 pass — full route → serialize → verify chain. |
| `test_bermuda_real_data.py` | 27/27 pass — real GGUF weights (Qwen3-0.6B) routed through pre-trained gate. |
| `_demo_geom_store.py` | **Proof of concept** — indexes GGUF weights by geometry, then queries by geometry key. |
| `_process_gguf_attn.py` | Routes all 113 attention tensors (147K rows) through pipeline. |

### Demo Output

```
Input → route → geometry (zone=6, shape=S)
→ lookup → 120 matching weights (NO GGUF reload)
→ dot product = 93.7

Without store: load 2.4GB GGUF → dequant → scan → compute
With store:    route 128 floats → O(1) lookup → direct float values
Speedup:      ~1000x less data touched
```

### Pipeline Throughput

| Batch size | Throughput |
|-----------|-----------|
| 1024 tokens | 130K tok/s |
| 32 tokens | ~800 tok/s (Python overhead) |
| 1 token | needs C route() — not yet optimized |

---

## 🔧 What Needs to Be Built

### Phase 1: POGLS Store Backend (1-2 hrs per team)

**Goal:** Replace in-memory Python dict with real persistent store.

**Status:** C read-only store reader is now in place for `.gsidx/.gsdat` with O(1) composite-key lookup and zero-copy mapped data.
**Status 2:** Python lazy pool is now in place for multi-model access with bounded RAM and per-model prime-on-demand.
**Status 3:** Coordinate registry is now in place so models can be resolved and primed by `(zone, shape, ns)` directly.
**Status 4:** A coordinate-first CLI runtime is now available via `python_src/coord_runtime.py`.
**Status 5:** `core/llama_pogls_runner.c` now has `--coord <registry> <zone> <shape> [ns]` to resolve and launch the matching GGUF.
**Status 6:** `python_src/coord_runtime.py` now exposes `resolve`, which the C runner uses to obtain `gguf_path`/`store_path`.
**Status 7:** `core/llama_pogls_runner.c` also has `--coord-resolve` for registry validation without llama startup.
**Status 8:** `python_src/coord_runtime.py` now has `inspect-text` and `inspect-npy` for coord debugging.

**Needs:**
- Write DiamondBlocks (64B each) to persistent storage indexed by geometry key `(zone, shape)`
- Query API: `store.query(zone, shape)` → returns shadow float32 values
- Batch insert + batch query
- Cursor-based iteration for efficiency

Implemented:
- `geopixel/geofield/geometry_store_reader.h`
- `geopixel/geofield/test_geometry_store_reader.c`
- `python_src/geometry_model_pool.py`
- `python_src/test_geometry_model_pool.py`
- `python_src/zero_warmup_engine.py` now exposes `prime()` and direct store query helpers
- `python_src/geometry_model_pool.py` now supports `GeometryCoord`, `query_coord()`, `prime_coord()`, and `forward_coord()`
- `python_src/coord_runtime.py` and `python_src/test_coord_runtime.py`
- `core/llama_pogls_runner.c` coord resolver path
- `python_src/coord_runtime.py resolve` output for runner handoff
- `core/llama_pogls_runner.c --coord-resolve`
- `python_src/coord_runtime.py inspect-text / inspect-npy`

**Input format (from pipeline):**
```python
# BermudaBlockWriter.from_verdict() → list of 64-byte blocks
blocks = BermudaBlockWriter.from_verdict(verdict, shadow_float_data, attach_shadow=True)
# Each block: 24B header + N×8B entries + optional shadow data
# Shadow float values extractable from SHADOW blocks (magic 0x5348)
```

**Output API:**
```python
store = GeometryStore(path)
store.index(zone, shape, float_values)  # write
values = store.query(zone, shape)       # read (no GGUF)
```

### Phase 2: Minimal Inference Engine (2-4 hrs per team)

**Goal:** Route input → find matching weights by geometry → compute forward pass.

**Needs:**
- Take an input vector (1×128 float32)
- Route through Bermuda → get geometry key
- Look up weights in store by geometry key
- Perform basic operations (dot product, matmul) using geometry-matched weights
- Chain operations: layer 0 geometry → layer 1 geometry → ... → output

**Architecture:**
```python
# Step 1: Route
geometry = router.route(input_vector)  # → (zone, shape)
# Step 2: Load weights by geometry (NO GGUF)
weights = geom_store.query(geometry.zone, geometry.shape)
# Step 3: Compute
output = matmul(input_vector, weights[0])  # forward using indexed weights
# Step 4: Route output → next layer's geometry
next_geometry = router.route(output)
# Repeat until final output
```

### Phase 3: Zero-Warmup Production Demo (4-8 hrs per team)

**Goal:** Cold start → first token output **without loading any model weights or CUDA kernel compilation**.

**Needs:**
- Load model metadata only (tensor names, shapes — kilobytes, not gigabytes)
- On first input: route → load weight block by geometry → dequantize on-the-fly → compute
- Only the specific weight block needed for current computation is loaded
- No GGUF file open, no `torch.load`, no CUDA warmup

**Comparison:**
```
Traditional:    load 2.4GB GGUF (11s) → CUDA warmup (5s) → first token
Zero-warmup:   load metadata (0.1s) → route (0.001s) → load 64B block → compute → token
```

---

## 📁 Files Included in Package

```
collection/
├── bermuda_reshape_v3.py        # Hilbert geometry engine
├── bermuda_router_v1.py         # Float → geometry router
├── bermuda_block.py             # DiamondBlock serialization
├── bermuda_export.h             # C ABI header
├── bermuda_export.c             # C ABI implementation
├── pogls_bond.h                 # Bond layer header
├── pogls_bond_export.h/c        # C ABI bond exports
├── pogls_bond.dll               # Compiled bond DLL
├── python_src/
│   ├── bermuda_bond_bridge.py   # C bridge (unified)
│   ├── bermuda_pipeline.py      # Full pipeline
│   ├── bermuda_bridge.py        # Legacy bridge
│   └── pogls_bond_py.py         # Python bond mirror (outdated)
├── test_bermuda_e2e.py          # 44 tests
├── test_bermuda_real_data.py    # 27 GGUF tests
├── _demo_geom_store.py          # Proof-of-concept
├── _process_gguf_attn.py        # GGUF weight processor
└── build/
    ├── bermuda_gate_g2_d128.pt  # Pre-trained gate
    ├── bermuda_gate_g2_d768.pt
    ├── bermuda_gate_g4_d768.pt
    ├── pogls_bermuda.dll        # Geometry C DLL
    └── pogls_bond.dll           # Bond C DLL
```

## 📐 Quick Reference: Geometry Encoder

```
Input (128 floats)
  → snap_gear() → pad → Hilbert scatter
  → gate.encode_tokens() → geometry index (uint64)
  → traverse(mode) → idx_out
  → classify() → zone, shape, polarity, tring_slot

Output: RoutingVerdict with:
  zone:       0-11 (geographic region)
  shape:      I/O/T/S/Z/L (routing topology class)
  polarity:   0=ROUTE, 1=GROUND
  tring_slot: 0-719 (720-slot ring position)
  idx_in:     pre-traverse geometry index
  idx_out:    post-traverse geometry index
```

## 📐 Quick Reference: DiamondBlock (64 bytes)

```
[0:8]    magic:  8 bytes  ('DIAMONDB' = 0x4449414D4F4E4442)
[8:12]   block_id: uint32 (sequential)
[12:16]  count:   uint32 (routing entries in this block)
[16:20]  link_next: uint32 (chain to next block, 0 = final)
[20:24]  flags:   uint32 (mode, shadow_attached bit)
[24:24+N*8] entries: N × 8-byte routing records
[remainder] shadow data (if SHADOW_ATTACHED flag set)
```

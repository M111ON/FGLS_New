# AGENTS.md — Session Handoff

## ก่อนสแกน (Pre-scan check)
- ก่อนเรียก scan() ให้เรียก get_project_index() เพื่อดูสถานะ cache
- หาก cache มีข้อมูลล่าสุด ให้ใช้ cached data แทนการสแกนใหม่
- หาก cache ไม่มีข้อมูลหรือล้าสมัย ให้สแกนตามปกติ

## Summary: Geometry-Routed LLM Inference PoC

### Goal
Prove that LLM hidden states can be mapped to Bermuda geometry structure, enabling geometry-routed inference.

### What Was Built

#### 1. C Gate Forward (`C:\TPOGLS\test_gate_forward.c`)
- Loads exported gate weights (.f32 files) from `collection/build/gate_export/`
- Encoder: matmul(128→256) → LayerNorm → GELU → matmul(256→32)
- Codebook: L2 argmin over 1024 codes → code index
- Geotable: index → 8 geometry params
- **Verified identical to Python** (code_idx=906 matches exactly)

#### 2. Geometry Store (`collection/build/qwen_geom_v3.gsidx/.gsdat`)
- 84 keys = 7 namespaces (Q/K/V/O/G/U/D) × 12 zones
- 100,352 rows × 1,024 cols, 401MB
- Each weight type stored with specific shape per zone:
  - Q/U: 'I' (south) / 'O' (north)
  - K/D: 'O' always  
  - V: 'S' always
  - O/G: 'L' always
- Store format: 32B header + 20B entries

#### 3. Live Routing PoC (`C:\TPOGLS\geom_llm_proof.c`)
- Loads Qwen3-0.6B via llama.dll (dual GTX 1050 Ti, CUDA)
- Sets `cparams.embeddings = true`
- After each `llama_decode()`, calls `llama_get_embeddings_ith()` → routing
- Reduces 1024-dim → 128-dim via mean-pooling → gate → code/zone/shape
- **Proven**: 16 tokens decoded, each routes to different geometry

### 4. Geometry Engine Dashboard (`collection/python_src/`)
- **Server**: `engine_dashboard.py` (FastAPI, port 8766)
- **UI**: `engine_dashboard.html` (single-file, Bauhaus Neo-Brutalist design, ~1085 lines)
- **7 Features** (auto-discovered from `features/`):
  - **Geometry Store** — stats, coverage matrix, key browser, weight query + heatmap
  - **Bermuda Router** — 4 modes (ORBITAL/CHIRAL/CROSS/HUB), zone radar, shape donut, R/G bar, TRing 720-slot heatmap, mode comparison
  - **Bermuda Gate** — forward test (Python), compare with C (code_idx=906 match)
  - **C Pipeline** — DLL bridge to `pogls_bermuda.dll`, Hilbert bijection, traverse, batch route
  - **C Binaries** — list/run `.exe` from browser, terminal output
  - **Geometry Codec** — **Hamburger encoder**: 8×8 dot-grid via slot-collapse (`slot%64`), cycle_1440, frame-seek `enc=(t×37)%1440`, cylinder (12 spokes × 6 faces), sparse dots (blank=free), inter-frame delta, pipeline header frame encodes POGLSHeader 30B, connection tracking via bond_key identity (enter/leave/persist), flow matrix, hot/cold pipeline stats, lossless MP4 via libx264rgb
  - **GeoPixel Codec** — pure-Python port of C `geo_pixel.h` + `bond_to_geopixel.h`: encode(idx→RGB), decode(RGB→fields), CRT byte recovery, piece fingerprint (1px RGB), 27-pixel lossless stripe encode/decode, 27×27 address grid (SVG render), batch roundtrip verify, (trit,coset,fibo) uniqueness at W=27. 10 API endpoints. 8 tests + 10 endpoint smoke tests all pass.
- **Design**: Bauhaus Neo-Brutalist: `#f5f0e8` paper bg, `#ffcc00` gold, thick 2px black borders, `3px 3px 0px #1a1a1a` shadow, Space Grotesk + Inter. No border-radius.
- **Auto-refresh**: gauge row updates every 15s
- **Start**: `cd collection/python_src && python engine_dashboard.py`

### Key Files
| File | Purpose |
|------|---------|
| `C:\TPOGLS\geom_llm_proof.c` | Live routing PoC (compiled .exe) |
| `C:\TPOGLS\test_gate_forward.c` | Gate C verification test |
| `I:\FGLS_new\collection\build\gate_export\*.f32` | Gate weights (8 files, ~323KB total) |
| `I:\FGLS_new\collection\build\qwen_geom_v3.gsidx` | Geometry store index (84 entries) |
| `I:\FGLS_new\collection\build\qwen_geom_v3.gsdat` | Geometry store data (401MB) |
| `I:\FGLS_new\collection\python_src\geometry_store.py` | Python store builder + reader |
| `I:\FGLS_new\collection\python_src\features\container_feature.py` | GPR1 + GPX4 container API (14 routes: encode/decode/info/inspect/timeline/preview/demo) |
| `I:\FGLS_new\collection\python_src\gpr1_container.py` | GPR1 container Python wrapper |
| `I:\FGLS_new\collection\python_src\gpx4_container.py` | GPX4 container Python reader/writer with O4 grid decode + SVG render |
| `I:\FGLS_new\collection\python_src\engine_dashboard.py` | FastAPI server (24 routes) |
| `I:\FGLS_new\collection\python_src\engine_dashboard.html` | Dashboard UI (~1085 lines, single-file) |
| `I:\FGLS_new\collection\bermuda_router_v1.py` | Python router (classify_idx, route) |
| `I:\FGLS_new\collection\bermuda_reshape_v3.py` | BermudaGate, GeoCodebook classes |

### C Build Commands
```bash
# Gate test
gcc -O2 -o test_gate_forward test_gate_forward.c -lm

# Live routing PoC
gcc -O2 -Wall -I. -I"I:/llama/llama_cuda124_x64/include" \
    -o geom_llm_proof geom_llm_proof.c \
    "I:/llama/llama_cuda124_x64/llama.dll" \
    "I:/llama/llama_cuda124_x64/ggml.dll" \
    "I:/llama/llama_cuda124_x64/ggml-base.dll" -lm

# Runner V3
cd C:\TPOGLS && make llama_pogls_runner_v3.exe
```

### Runner Benchmarks (Qwen3-0.6B Q8_0)
- CPU-only: 4.35 tok/s
- GPU (2× GTX 1050 Ti, 28 layers): 21.5 tok/s
- Path: Set-Location C:\TPOGLS; $env:Path = "I:\llama\llama_cuda124_x64;$env:Path"

### Container Formats (GPR1 / GPX4)
- **GPR1**: Generic residual container — chunk-based delta storage with 4B CRCs, single-header C codec (`gpr1_container.h`), Python file-level wrapper, FastAPI upload/download/decode endpoints.
- **GPX4**: Multi-layer GeoPixel container — O4 grid layers (27×N RGB pixels), delta layers (YCgCo + ZSTD), animation header with frame index, tile table for tiled layers. Python reader + writer, FastAPI inspect/timeline/preview endpoints.
- **O4 Grid Preview**: Dashboard can decode O4 layer (raw RGB or PNG via PIL) and render as inline SVG grid. Timeline view shows ordered keyframes (F*) + deltas (D*) with per-frame preview. Demo builder creates synthetic 27×3 O4 GPX4 for testing.
- **File layout**: Header (16B) + Layer table (14B per layer) + Optional tile table (8B per tile) + Layer data. All big-endian.
| `collection/python_src/geom_codec.py` | Hamburger encoder: 8×8 dot-grid, frame-seek, encode_sequence (APNG), encode_level_mp4 (lossless libx264rgb), bond-key tracking |
| `collection/pogls_pipeline_bridge.py` | Python↔C bridge: verdict_to_chunk_desc, header_to_frame0, encode_sequence_with_header (returns pipeline_info with header metadata) |
| `collection/hot_store_wire.py` | HotStoreFn wiring: PyFglsStore, DllFglsStore (full ctypes ABI with stats struct + serialize + tick + destructor), FglsSessionIndex, WiredPipeline (encode + decode with session persistence) |
| `collection/pogls_fgls_export.c` | C DLL export wrapper: alloc/free/init/store_raw/tick/serialize/stats (187 lines, exports 10+ symbols) |
| `collection/build_fgls_dll.bat` / `.sh` | Build scripts for pogls_fgls.dll/.so (MinGW/Linux gcc) |
| `collection/pogls_geofield_export.h` | GeoField → POGLS Header Export (Approach B, 413 lines) |
| `collection/pogls_pipeline.h` | Full encode/decode pipeline: ColdStore 144-ring + hash table overflow (≤75% load, 32-probe limit) + HotStore + verify |

### 5. C Reader for .gsidx/.gsdat (`collection/geo_store_reader.h`)
- Single-header C library: no malloc in query path, heap-backed for cross-platform
- Format: 32B header (magic="GSIDX001" + n_entries + data_size + pad) + 20B entries (raw_zone + shape_idx + offset + n_rows + n_cols)
- Namespace encoding: raw_zone = base(0-11) + shift(Q:+12/K:+24/V:+36/O:+48/G:+60/U:+72/D:+84)
- API: `geo_store_open()` / `open_mem()`, `geo_store_query(ns, zone, shape)`, `geo_store_has()`, `geo_store_list()`, `geo_store_close()`
- Two open modes: file-backed (reads .gsidx/.gsdat into heap) and memory-backed (zero-copy via caller-supplied buffer)
- **Verified**: 49/49 tests pass — all 84 keys queryable across 7 namespaces, data matches Python output byte-exact
- Build: `gcc -O2 -Icollection -o test_geo_store_reader collection/tests/test_geo_store_reader.c -lm`
- Test data: `collection/build/test_geom.gsidx/.gsdat` (84 keys × 8×64 float32 = 168KB, generated by `tests/make_test_store.py`)

### Not Yet Done
- Weight subset inference (use geometry store weights instead of full GGUF)
- Integration of gate routing into runner_v3's decode loop

### Bond Connection Tracking
- `track_connections()` uses **bond_key identity** (not `bond_verify` topology): same bond_key across levels = persist; new/disappeared bond_key = enter/leave
- `connection_flow()` matches next-level tokens to previous-level tokens by bond_key equality
- bond_key is treated as a command/iteration tag, not topological constraint — no topology verification needed for chunk tracking
- `_piece_from_token(zone, shape, tring_slot) → seed = (zone<<16)|slot → make_piece(seed, fold_axis)` 
- Same zone+shape+slot → same bond_key → persist. Different → enter/leave.
- Verified: 4 ticks → persist=8, enter=10, leave=7

### Pipeline ColdStore Hash Table
- `cold_push`: overflow eviction halts at 75% load (`cap - cap/4`) to guarantee short probe chains
- `cold_find`: linear probe capped at `POGLS_COLD_PROBE_MAX=32` iterations (not unbounded)
- Earlier flat array scan was O(n) at 100% load (4096 probes per miss); now at 75% load, avg cluster ≈ 4, max 32
- `cold_store_init` no longer takes separate `occ` pointer — `overflow_occ` sits at `overflow_buf + overflow_cap` automatically

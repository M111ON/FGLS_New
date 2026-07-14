# HANDOFF — GeoPixel Pipeline (July 14, 2026)

## Goal of Next Session

Wire the complete GeoPixel pipeline: **GeoField → Wallet → Geopixel** — a one-stop service that takes any file and encodes/decodes it through the full geometric compression stack.

## State of Play

### Done

| Step | Status | File |
|------|--------|------|
| GeoField 64B chunking | ✅ | `tools/geopixel_pipeline.py` |
| GpAddr mapping (tile_id, dim) | ✅ | `tools/geopixel_pipeline.py` |
| Skeleton classify (ID/FLAT/DIFF/RAW) | ✅ | `tools/geopixel_pipeline.py` |
| Wallet CoordRecord (face\|edge\|z) | ✅ | `tools/geopixel_pipeline.py` |
| Wang tile validation (Fib 2&7 chord) | ✅ | `tools/geopixel_pipeline.py` |
| Tantrix 256-state routing | ✅ | `tools/geopixel_pipeline.py` |
| Timeline reconstruction | ✅ | `tools/geopixel_pipeline.py` |
| 6-face unfold/fold | ✅ | `tools/geopixel_pipeline.py` |
| UI service (tkinter) | ✅ | `tools/geopixel_service_v2.py` |
| Dynamic cube sizing (base 4,8,16) | ✅ | `tools/geopixel_service_v2.py` |

### Not Done / Needs Work

1. **Wang tile edge validation failing** — edge_bot[win] ≠ edge_top[win+1]
   - Root cause: Wang window computation uses `frame_seek(t)` but the frame-to-enc mapping needs to match `geo_frame_seek.h` exactly
   - Fix needed: align `_compute_window` with C `fwang_compute_win`

2. **True compression not working** — files expand instead of compress
   - Root cause: cube side is too large for small files (padding)
   - Fix: use chunking (split file into chunks, each encoded separately)
   - Or: only encode when cube_volume > file_size (skip compression for small files)

3. **Interior reconstruction is synthetic** — not true timeline reconstruction
   - Current: `interior = (face * 100 + slot * 10 + ico_idx + phase * 5)`
   - Needed: actual geo_frame_seek + geo_jump routing for real data

4. **No file format yet** — pipeline outputs raw 6 faces, no header/footer
   - Need: `.geopixel` file format with metadata (original size, gp_level, base, seed)

5. **No chunking for large files** — 1MB+ files need chunked encoding
   - Each chunk encoded independently → parallel

## Open Decisions

1. **Cube size strategy**: Dynamic (per file) vs fixed (100³)?
   - Fixed: simpler, but wastes space for small files
   - Dynamic: better compression, but needs base selection

2. **Chunking strategy**: Split file into 1MB chunks? Or adapt cube size?
   - Chunking: parallelizable, simpler
   - Adaptive: better compression

3. **File format**: Binary `.geopixel` with header?
   - Header: magic, version, gp_level, base, original_size, n_chunks
   - Body: 6 faces per chunk

## Skills to Use

- `pogls-pipeline` — POGLS geometric compression pipeline
- `msys2-build-pipeline` — Build C/C++ components

## Artifacts

### Core C Headers (reference implementations)

| File | Purpose |
|------|---------|
| `collection/geopixel/geofield/geo_field_core.h` | GeoField encode/decode (64B chunks → GpAddr → FrustumBlock) |
| `collection/geo_frame_seek_wang.h` | Wang tile validation (Fib 2&7 chord, 120 windows) |
| `collection/lc_tantrix.h` | Tantrix 256-state routing (entry/exit/spoke/class) |
| `core/pogls_engine/pogls_coord_wallet.h` | Wallet format (CoordRecord: face\|edge\|z, seed, checksum) |
| `collection/geo_frame_seek.h` | Timeline (stride-37, 1440 frames, 12 faces × 120 slots) |
| `collection/geo_jump_module/geo_jump.h` | GeoJump routing (20736 nodes, O(1)) |
| `collection/geopixel/geofield/geo_goldberg_sphere.h` | Goldberg sphere coords (10n²+2 tiles) |

### Python Tools (current)

| File | Purpose |
|------|---------|
| `tools/geopixel_pipeline.py` | **Full pipeline**: GeoField → Wallet → Geopixel (reference impl) |
| `tools/geopixel_service_v2.py` | Tkinter UI (browse, encode/decode, log) |
| `tools/hamburger_codec_poc_v5.py` | 6-face cube operations (unfold/reconstruct) |

### AGENTS.md

`I:\FGLS_new\AGENTS.md` — Full project state, all verified systems, session history

## Key Architecture

```
Raw file
  ↓
64-byte chunks (geo_field_core.h)
  ↓
GpAddr (tile_id, dim) via gp_chunk_to_addr()
  ↓
Skeleton classify: ID / FLAT / DIFF / RAW
  ↓
CoordRecord (face|edge|z) — wallet format
  ↓
Wang tile validation (Fib 2&7 chord)
  ↓
Tantrix routing (256-state)
  ↓
Cube (timeline-derived)
  ↓
6 faces (Hamburger Codec)
```

### Key Constants

```python
CHUNK_SZ = 64           # bytes per chunk
GEO_FULL = 20736        # 144² address space
FRAME_CYCLE = 1440      # frames in timeline
FRAME_STRIDE = 37       # prime walk
FRAME_FACE_SZ = 120     # slots per face
GOLDEN_PHI = 1.618...   # golden ratio
GP_LEVEL_DEFAULT = 2    # Goldberg level (42 tiles)
```

## Performance

```
Operation         Speed
─────────────────────────────
GpAddr            95 ns/call
Skeleton          1.6 µs/call
Wallet seed       24.6 µs/call  ← bottleneck
Wang chord        162 ns/call
Tantrix route     211 ns/call
Frame at          178 ns/call

Pipeline (per chunk): ~57 µs
1MB file: 886 ms (Pentium G4400)
```

## Critical Context

- **This is a C11 project**, not Python. Python tools are reference implementations for rapid prototyping
- **The C headers are the source of truth** — Python must match their logic exactly
- **Wang tile edge validation** is the current blocker — edges must chain: `edge_bot[win] == edge_top[win+1]`
- **Scale base 4, 8, 16** — user explicitly wants base-4/8/16 scaling to match geo_jump
- **The real pipeline lives in `collection/` headers** — the Python is just a scaffold

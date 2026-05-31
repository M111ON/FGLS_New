# Geometry Routing Architecture

## Overview
Content-agnostic input routing using geometric address encoding. Instead of embedding-based similarity search, inputs are mapped to a geometric address and routed to expert models by address range.

## Pipeline

```
Input ──→ geometric address ──→ zone/shape selector ──→ model dispatch
                ↑                        │
            gb_decode_tile()              │
                ↑                        ↓
           .gsten store            llama_pogls_runner
         (tile-encoded weights)      (GGUF inference)
```

## Geometric Address
```
[face:8][spoke:8][slot:8][world:8][layer:6][intensity:6][bits:20] = 64 bits
```
Encoded as 11-char base62 string (POGLS address format).

## .gsten Tile Format

### File structure
```
┌─────────────────────────────┐
│ Header (16B)                │
│  magic(4B) = 0x4E455453    │
│  n_tiles(4B)                │
│  tile_sz(1B) = 7            │
│  reserved(7B)               │
├─────────────────────────────┤
│ Index (n_tiles × 6B)        │
│  offset(4B) - byte offset   │
│  size(1B)   - encoded size  │
│  type(1B)   - tile type     │
├─────────────────────────────┤
│ Tile data (variable)        │
│  tile_0: [type|pred|resid..]│
│  tile_1: ...                │
└─────────────────────────────┘
```

### Hex Tile Types
| Type | Code | Size | Description |
|------|------|------|-------------|
| FLAT | 0x00 | 2B | All 7 bytes identical → type + value |
| TRIPLET_FLAT | 0x01 | 9B | One triplet flat → pred + residuals |
| GRADIENT | 0x02 | 9B | Smooth transition → pred + residuals |
| EDGE | 0x03 | 9B | High contrast → pred + residuals |

### Prediction
For non-FLAT tiles: prediction = median of ring median when no triplet flat. Residuals stored as `(original - pred + 128) & 0xFF`.

## Routing Modes

### ORBITAL
Spoke-based zones radiating from center. Each spoke maps to different expert behavior.

### CHIRAL
Handedness-based split — left/right geometry determines route.

### CROSS
Cross-shaped partitions — inputs in cardinal directions route differently.

### HUB
Central hub collects all inputs → dispatches to spokes by sub-address.

## Dual-Model Dispatch
Multiple GGUF models loaded in one process. Dispatch by pointer swap:
- ~0 µs switch cost
- KV cache conflict on per-token alternation (not an issue for single-dispatch)
- Models: qwen25 (580ms load, 14.6 t/s CPU), smollm2 (220ms load, 18.7 t/s CPU)

## Model ↔ Zone Registry
`coord_real_registry.json` maps zone ranges to model:
- `qwen` → zones 0-3
- `qwen25` → zones 4-5, 9-11
- `smollm2` → zones 6-8

## Known Limitations
- **M-RoPE bug**: Qwen3-Embedding returns zero embeddings in llama.cpp (issue #20085). Bypass: use qwen25 (qwen2 arch) or smollm2 (llama arch)
- **BermudaGate codebook collapse**: Training on repetitive generated tokens → encoder output uniform → all map to code 0. Fix: train on diverse embeddings
- **KV cache conflict**: Alternating contexts corrupts KV on rapid model switching

## Container Formats

### GPX4
Multi-layer container: O4 grid + delta layers (YCgCo + ZSTD) + GEO address layer.
- `GPX4_LAYER_O4`: Base layer
- `GPX4_LAYER_DELTA`: Residual layer
- `GPX4_LAYER_GEO` (0x06): Hex-encoded geometry addresses (via geo_hex_layer.h)

### GPX5
Carrier-based per-tile container.
- `GPX5_CODEC_HEX` (0x07): hex_tile.h encode/decode
- `GPX5_CODEC_L2` (0x08): hex_codec.h batch encode (XOR dual predictor)

### GPR1
Generic residual container. Chunk-based delta storage with 4B CRC. Single-header C codec.

## Codec Stack
```
GPX5 pipeline (carrier-based, per-tile)
  └─ GPX5_CODEC_HEX (0x07)
  └─ GPX5_CODEC_L2 (0x08)
Standalone batch
  └─ GPX5_CODEC_L2 + hex_codec_impl.c
Bridge to GPX4
  └─ geo_hex_layer.h
```

## File Map
```
collection/
├── geom_raw_bridge.h         # Single-header RawBridge
├── hex_tile.h                # 7-cell hex tile codec
├── hex_codec.h               # Batch hex codec v3
├── src/hex_codec_impl.c      # hex_codec implementation
├── geo_hex_layer.h           # GPX4 GEO layer bridge
├── lc_tantrix.h              # 256-state routing layer
├── lc_twin_gate.h            # Twin-gate layer
├── gpx4_container.h          # GPX4 container (3 copies)
├── gpx5_container.h          # GPX5 container (4 copies)
├── gpr1.h                    # GPR1 residual container
├── build_geom_tile_store.py  # Python .qdat → .gsten
├── bermuda_reshape_v3.py     # BermudaGate training
├── coord_real_registry.json  # Model ↔ zone mapping
├── build/
│   ├── qwen25_tensors_raw/   # 291 .qdat files
│   ├── qwen25_full_gsten/    # 291 .gsten files (tile-encoded)
│   └── qwen25_gsten/         # Legacy: 20 files (blk.0-1 only)
└── tests/
    ├── c_gsten_encode.c      # C encoder (40s full model)
    ├── test_geom_bridge.c    # End-to-end load/decode test
    ├── test_weight_reconstruct.c  # Roundtrip verification
    ├── test_grb.c            # GeomBridge synthetic + real test
    ├── test_tantrix.c        # 11 tests for lc_tantrix
    ├── test_hex_tile.c       # 24 tests for hex_tile.h
    ├── test_hex_codec.c      # 28 tests for hex_codec.h
    └── test_geo_hex_layer.c  # 23 tests for geo_hex_layer.h
```

# POGLS v3: Geopixel Header-Only Store

## Overview

POGLS v3 applies the **geopixel principle**: data flows through a deterministic field, and only a compact header is needed to seek to any tensor's data. Weights stay in the original GGUF file — the .pogls file is a ~20KB lookup table.

## Architecture

```
┌────────────────────────┐       ┌────────────────────────────┐
│ model.pogls (20 KB)    │       │ model.gguf (4.79 GB)       │
│ ─────────────────────  │       │ ─────────────────────────  │
│ Header (128 B)         │       │ GGUF metadata             │
│  magic, version, count │       │ tensor data section:      │
│                        │       │  [token_embd.weight]      │
│ Entries[] (256 × 80B)  │       │  [blk.0.attn_q.weight]   │
│  sorted by name        │──→    │  [blk.0.attn_k.weight]   │
│  binary search O(lgN)  │  seek│  ...                      │
│                        │       │                            │
│ GGUF relative path     │──────→│ mmap + offset = data ptr  │
└────────────────────────┘       └────────────────────────────┘
```

## Lookup Flow

1. **Hash tensor name** → deterministic geo address (`addr_from_tensor_name()`)
2. **Binary search** v3 entries (sorted by name) → entry with GGUF offset
3. **Read from GGUF mmap** at offset → tensor data pointer

## Benchmark

| Operation | v3 Binary Search | GGUF Linear Scan | Speedup |
|---|---|---|---|
| Per lookup | 181 ns | 1,343 ns | **7.4×** |
| 256 tensors × 100K scans | 4.6s | 34.4s | 7.4× |

## File Sizes

| Format | Size | vs GGUF |
|---|---|---|
| model.gguf | 4.79 GB | 1.0× |
| model.pogls v2 (full data) | 5.15 GB | 1.08× |
| **model.pogls v3 (header only)** | **20.2 KB** | **41,681× smaller** |

## Usage

```bash
# Create v3 header
runner\gguf_to_pogls_v3.exe I:\model\model.gguf model.pogls --relative --verify

# Run with v3
runner\llama_pogls_runner_sid_v2.exe I:\model\model.gguf --pogls-v3 model.pogls --chat
```

## Key Design: 80-byte Entries

```
Offset  Size  Field
0       4     addr          geo address (deterministic from name)
4       4     dtype         GGUF data type (F32=0, Q4_K=12, ...)
8       4     ndim          number of dimensions
12      4     nbytes        tensor data size in bytes
16      8     gguf_offset   absolute offset in GGUF file
24      16    dims[4]       tensor dimensions
40      40    name          tensor name (null-terminated)
──────────────────────
Total: 80 bytes per entry
```

## Files

| File | Purpose |
|---|---|
| `runner/pogls_v3_geopixel.h` | Header format + binary search API |
| `runner/gguf_to_pogls_v3.c` | GGUF → .pogls converter |
| `runner/test_pogls_v3.c` | Unit tests (7/7 pass) |
| `runner/test_pogls_v3_e2e.c` | E2E tests (7/7 pass) |
| `runner/llama_pogls_runner_sid_v2.c` | Runner with `--pogls-v3` flag |

## Geopixel Principle

The geopixel concept is NOT about compressing tensor bytes — Q4 quantized weights are byte-level random noise (100% EDGE blocks). The principle is:

1. **Deterministic field**: tensor positions computed from name hash, not stored
2. **Header-only seek**: compact mapping replaces full data copy
3. **Weights stay in GGUF**: no duplication, zero copy

This is exactly the geo_frame_seek theory applied to model weight storage: the "frame" is the 20KB header, the "seek" is binary search, and the "reconstruction" is reading from the original GGUF at the stored offset.

# POGLS Toolchain CLI Reference

Complete CLI tool reference for the POGLS (Parallel Optimized Geometry-Linked Storage) toolchain.

## Quick Start

```bash
# Build everything
mingw32-make all

# Run tests
mingw32-make test

# Install to tools directory
mingw32-make install
```

---

## Core Library

### `pogls_core.lib` (Static Library)

Link with `-lpogls_core` to use POGLS functionality.

**Headers:**
- `pogls_core.h` — Single-include entry point
- `pogls_platform.h` — Platform abstraction (VirtualAlloc/mmap)
- `pogls_compress.h` — ZSTD compression with auto-raw fallback
- `pogls_addr.h` — 144² address resolution
- `pogls_store.h` — Store API contract

---

## Phase 1: Core CLI Tools

### `pogls_compress`

Compress raw data with ZSTD compression.

```
Usage: pogls_compress <input> <output> [-l level]
  -l level  ZSTD compression level (default: 3)
```

**Behavior:**
- Reads raw bytes from input file
- Compresses with ZSTD
- Auto-falls-back to RAW if compression ratio < 1.10x
- Output format: `[4B comp_type][4B orig_sz][4B comp_sz][data]`

**Example:**
```bash
.\pogls_compress weights.bin weights.zst
```

---

### `pogls_decompress`

Decompress compressed files back to raw data.

```
Usage: pogls_decompress <input> <output>
```

**Behavior:**
- Reads compressed file (ZSTD or RAW format)
- Decompresses to output file
- Auto-detects format by magic number

**Example:**
```bash
.\pogls_decompress weights.zst weights.bin
```

---

### `pogls_inspect`

Dump POGLS file metadata to console.

```
Usage: pogls_inspect <file.pogls>
```

**Output includes:**
- Header: magic, version, tensor count, flags
- Tensor metadata: address, dtype, dimensions, compression
- Summary: total bytes, compression ratio

**Example:**
```bash
.\pogls_inspect model.pogls
```

---

### `pogls_roundtrip`

Verify compress → decompress roundtrip integrity.

```
Usage: pogls_roundtrip <file> [-l level]
  -l level  ZSTD compression level (default: 3)
```

**Output:**
- PASS if decompressed bytes match original exactly
- FAIL if any mismatch

**Example:**
```bash
.\pogls_roundtrip model_weights.bin
```

---

## Phase 2: Address & Conversion Tools

### `addr_resolve`

Resolve tensor names to 144² addresses.

```
Usage: addr_resolve --name "tensor.name" [--tier N]
       addr_resolve --file model.gguf [--tier N]
```

**Modes:**
- `--name` — Resolve single tensor name, show address decomposition + face rotation
- `--file` — List all tensors in GGUF file with their addresses

**Address tiers:**
- Tier 0: 20736 addresses (128×162 = 144²)
- Tier 1: 429M addresses (massive MoE)

**Example:**
```bash
.\addr_resolve.exe --name "blk.0.attn_q.weight"
.\addr_resolve.exe --file model.gguf
```

---

### `gguf_dump`

Inspector for GGUF model files.

```
Usage: gguf_dump <model.gguf>
```

**Output:**
- File size, tensor count, data offset
- Full tensor list with sizes and types

**Example:**
```bash
.\gguf_dump.exe model.gguf
```

---

## Phase 3: Store & Delta Tools

### `dramtile_dump`

Dump DRamTile store statistics.

```
Usage: dramtile_dump [--twin <file>] [--cold <file>]
```

**Output:**
- Capacity, used, free bytes
- Hash slot distribution (weight, kv, bond, empty)
- KV region stats (if present)
- Cold region stats (if present)

**Example:**
```bash
.\dramtile_dump.exe
.\dramtile_dump.exe --twin model.twin
```

---

### `dramtile_bench`

Benchmark DRamTile put/get/free performance.

```
Usage: dramtile_bench [--size KB] [--count N]
  --size    Tensor size in KB (default: 64)
  --count   Number of operations (default: 1000)
```

**Output:**
- Init time, Put rate, Get rate, Free rate
- Final capacity and fill percentage

**Example:**
```bash
.\dramtile_bench.exe --size 128 --count 500
```

---

### `kv_delta_bench`

Benchmark KV delta compression performance.

```
Usage: kv_delta_bench [--size KB]
  --size    Data size in KB (default: 128)
```

**Output:**
- Compression ratio for 10%, 20%, 40%, 80% change levels
- Compression time per level

**Example:**
```bash
.\kv_delta_bench.exe --size 256
```

---

### `kv_delta_test`

Roundtrip test for KV delta compression.

```
Usage: kv_delta_test
```

**Tests:**
- 1B, 63B, 64B, 256B, 1KB, 4KB, 64KB, 128KB
- Reports PASS/FAIL for each size

**Example:**
```bash
.\kv_delta_test.exe
```

---

## Phase 4: Pipeline & One-Shot Tools

### `pogls_verify`

Verify POGLS file integrity.

```
Usage: pogls_verify <file.pogls>
```

**Checks:**
- File not empty
- Magic number valid
- Version supported
- Tensor count > 0
- Header flags valid
- Data section alignment
- Tensor meta validity

**Example:**
```bash
.\pogls_verify.exe model.pogls
```

---

### `pogls_build`

One-shot GGUF → POGLS build.

```
Usage: pogls_build <model.gguf> <output.pogls> [--compress] [--verify]
  --compress  Enable ZSTD per-tensor compression
  --verify    Run verification after build
```

**Example:**
```bash
.\pogls_build.exe model.gguf model.pogls --compress --verify
```

---

### `pogls_cat`

Concatenate multiple files.

```
Usage: pogls_cat <output> <input1> <input2> [...]
```

**Example:**
```bash
.\pogls_cat.exe combined.bin part1.bin part2.bin part3.bin
```

---

### `pogls_diff`

Byte-level diff between two files.

```
Usage: pogls_diff <file1> <file2> [--first-diff N]
  --first-diff N  Stop after N differences (default: 20)
```

**Example:**
```bash
.\pogls_diff.exe original.bin modified.bin
.\pogls_diff.exe a.bin b.bin --first-diff 5
```

---

## Build Targets

| Target | Description |
|--------|-------------|
| `all` | Build core + tools + runner |
| `core` | Build `pogls_core.lib` static library |
| `tools` | Build all 14 CLI tools |
| `runner` | Build llama_pogls_runner + gguf_to_pogls |
| `test` | Run toolchain test suite |
| `install` | Install tools to `I:/FGLS_new/tools` |
| `clean` | Remove all build artifacts |
| `help` | Show build targets |

---

## Architecture

```
CLI Tools ─────────────────────────────────────┐
│ pogls_compress │ dramtile_cli │ addr_resolve  │
├──────────────────────────────────────────────┤
POGLS Core Library (pure C, no platform deps)  │
│ pogls_compress.h  pogls_addr.h  pogls_meta.h │
├──────────────────────────────────────────────┤
Platform Layer (swap one file for Linux)        │
│ pogls_platform.h = VirtualAlloc + Win32 I/O  │
└──────────────────────────────────────────────┘
```

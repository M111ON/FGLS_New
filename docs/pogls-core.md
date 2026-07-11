# pogls_core — Foundation Layer

Zero-dependency C11 library providing platform abstraction, compression, address space, metadata format, and store interface.

## Architecture

```
pogls_core.h  ← umbrella include
    ├── pogls_platform.h/c  — VirtualAlloc/mmap, file I/O
    ├── pogls_compress.h/c  — ZSTD + RAW fallback
    ├── pogls_addr.h/c     — 144²=20736 address space
    ├── pogls_meta.h/c     — POGLS v2 format
    └── pogls_store.h      — store contract (no .c)
```

## Key Design Decisions

- **Zero platform #ifdefs** in business-logic headers — all OS code isolated in platform layer
- **Linux port** = replace `pogls_platform.c` only
- **No malloc in hotpath** — address decomposition is pure integer math
- **Compression** auto-detects compressibility per tensor

## 144² Address Space

128×162 = 144² = 20736. Tier0 for current models.
- Tier1: 144⁴ ≈ 430M (massive MoE)
- Tier2: 144⁶ ≈ 8×10¹⁴ (future)

## POGLS v2 Format

| Section | Content |
|---------|---------|
| Header | magic(4B) + version(4B) + flags(4B) + n_tensors(4B) + data_pos(8B) |
| Tensor meta[] | name(64B) + addr(4B) + offset(8B) + size(8B) + dtype(4B) + comp(1B) + pad |
| Index (optional) | sorted address index for binary search |
| Tensor data | raw or compressed |

## Build

```bash
gcc -O2 -std=c11 -Ipogls_core main.c pogls_core/*.c -o tool
```

# KV Archive — DGLS Shell Compressed KV Cache Snapshots

## What
Compresses KV cache tensor data with **DGLS Binary Shell Codec** (lossless, Zstd-based) into compact snapshots. Replaces the old raw-backup-buffer approach that doubled RAM.

Key capabilities:
- **`/snap`** — losslessly compress all layers' K/V into ~5-15% of original size
- **`/unsnap`** — decompress snapshot back to original tensor memory
- **`/evict N`** — zero-copy pointer swap to working buffer (for "forgetting" behavior)
- **`/restore`** — swap tensor pointers back to original buffer
- **Full combine**: snap → evict → (model forgets) → restore + unsnap → (model remembers)

### Compression Results (Qwen2.5-0.5B, 24 layers, 2048 ctx)
| State | Raw KV | Compressed | Ratio |
|---|---|---|---|
| Empty cache (init) | 24 MB | 0.79 MB | 32:1 |
| After ~18 tokens | 24 MB | 1.43 MB | 17.6:1 |
| After ~36 tokens | 24 MB | 1.56 MB | 16.2:1 |
| After ~80 tokens | 24 MB | 2.50 MB | 10.1:1 |

The compression applies **Binary Shell Codec** (`binary_shell_codec.h`):
- **FLAT** (2B per 64B chunk) for zero-filled cache regions
- **SPARSE** or **DENSE→Zstd** for active fp16 cache data

## Why
- **Reduce working set physical RAM**: snapshot + decommit (VirtualAlloc MEM_RESET) tells the OS to reclaim physical pages backing the original KV buffer. This reduces **physical RAM usage** without freeing virtual address space.
- **Session save/load**: compressed snapshots are portable and can be written to disk (future: `/save` / `/load` commands)
- **Persona switching**: evict → model forgets → unsnap → model remembers from compressed state
- **Debugging**: verify KV tensor access works across architectures

## How to compile
```bash
# With KV archive features (snap/evict/save/restore)
gcc -DKV_ARCHIVE -O2 -std=c11 ... -I../collection/dgls/diamond/include -lzstd -o runner.exe

# Without — zero overhead, no KV snapshot code
gcc -O2 -std=c11 ... -o runner.exe
```

## Commands (when compiled with `-DKV_ARCHIVE`)
| Command | Description |
|---|---|
| `/snap` | Compress all layers' K/V → compact snapshot |
| `/unsnap` | Restore + decompress snapshot back to original memory |
| `/evict N` | Swap N oldest layers to zeroed working buffers (forget) |
| `/restore` | Swap all tensor pointers back to original buffer |

## Architecture support
| Type | Status |
|---|---|
| `llama_kv_cache` (Qwen2.5, Llama, etc.) | ✅ |
| `llama_memory_hybrid` (LFM2, Qwen3.5) | ✅ attention layers only |
| `llama_kv_cache_dsa` (DeepSeekV3) | ⬜ not implemented |

## Example workflow
```
>>> My name is Bob. Please remember that.
... (model responds)
>>> /snap             ← compress KV state (saves Bob's identity)
>>> /evict 24         ← zero-copy swap, model forgets
>>> What is my name?  ← model doesn't know
... (model: "I don't know")
>>> /restore          ← swap tensor pointers back to original
>>> /unsnap           ← decompress snapshot → Bob's data restored
>>> What is my name?  ← model remembers!
... (model: "Your name is Bob.")
```

## RAM reduction path
1. **`/snap`**: compress KV to ~5-15% — **no RAM saved yet** (compressed + original both exist)
2. **Physical decommit** (future / `--kv-decommit` flag): after snap, call `VirtualAlloc(MEM_RESET)` on the original KV buffer → OS reclaims physical pages. On next decode, pages fault back in as zero. Combined with unsnap which writes snapshot data back, this gives true physical RAM reduction.
3. **VRAM reduction**: for GPU backend, copy compressed data to system RAM, free GPU buffer (requires backend-specific code)

Peak decode RAM is unchanged (the original KV buffer stays allocated in virtual address space). The saving is in **physical RAM between decode steps** and **memory pressure on the system**.

## Files
- `kv_sid_evict.h` — core: compress, decompress, evict, restore, decommit
- `kv_tensor_access.cpp` — C++ bridge: `resolve_kv()` finds KV cache across memory types
- `kv_tensor_access.h` — public API
- `README_KV_ARCHIVE.md` — this file

## Dependencies
- **zstd** (`-lzstd`) — used by Binary Shell Codec for DENSE chunk compression
- **DGLS** includes (`-I../collection/dgls/diamond/include`)

## Warning
- Poison pattern (0xDE/0xAD) for eviction verification may segfault if the model reads from poisoned memory during decode
- Recurrent/hybrid models (LFM2) still retain information via non-attention layers
- `/unsnap` requires `/restore` first to swap pointers back to original buffer

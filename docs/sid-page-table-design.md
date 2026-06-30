# SID Page Table Design (June 30, 2026)

## Problem

Current SID implementation consumes ~15GB RAM + heavy memcpy:
- 5.1GB sid_cache (preload all tensors)
- 5.1GB DRamTile (VirtualAlloc + copy)
- 5.1GB face buffers + temp copies
- Task Manager spikes during swap

This contradicts the original geometry-based zero-copy design.

## Core Equation

```
128 × 162 = 144 × 144 = 20736  (= GEO_FULL)

128 = 2⁷  (binary, Hilbert 64×64×2 layers)
162 = 2×3⁴  (icosphere f=4 vertices, icosa ternary)
144 = F(12) = 2⁴×3²  (Fibonacci bridge: power-of-2 × power-of-3)
```

## Key Insight

DRamTile, GGUF mmap, GPU buffer, Y-triangle grid — **all share the same address space** (0..20735). A tensor's `dram_addr` is the same regardless of backend.

## Proposed Design: Page Table Indirect

### Layout

```
[tensor_0 | tensor_1 | ... | tensor_20735]  ← orig (GGUF mmap / GPU buffer)
[tensor_0 | tensor_1 | ... | tensor_20735]  ← face (spare, lazy-allocated)

each at stride = elem_size bytes per tensor
```

### Page Table

```c
uint8_t sid_page[2592];  // 20736 bits = 2592 bytes

// bit[N] = 0 → read from orig_base[N]
// bit[N] = 1 → read from face_base[N]
```

### Operations

```c
// Swap: flip 1 bit (no memcpy, no malloc, no I/O)
void sid_swap(int addr) {
    sid_page[addr >> 3] ^= (1 << (addr & 7));
}

// Read: indirect (CPU pointer or GPU kernel indirection)
void *tensor_data(int addr) {
    return (sid_page[addr>>3] >> (addr&7)) & 1
        ? face_base + addr * elem_size   // lazy page fault on first access
        : orig_base + addr * elem_size;  // OS mmap / GPU buffer
}
```

### GPU Kernel

```c
float w = sid_page[addr>>3] & (1<<(addr&7))
    ? face_buf[addr * stride + i]    // SID face
    : orig_buf[addr * stride + i];    // original
```

No extra read, no extra bandwidth — just one bit test.

### Memory

| Component | Size | Resident |
|-----------|------|----------|
| orig_base | 5.1GB | GGUF mmap (already there) |
| face_base | 5.1GB | committed but RESIDENT = only actively swapped tensors |
| sid_page[] | **2,592 bytes** | always hot |
| **Total extra** | **~2.5KB + lazy pages** | vs current ~15GB |

No sid_cache. No DRamTile copy. No preload. No progressive.

### Time Travel (Delta Ring)

Same concept: instead of journaling pointers, journal page table flips:

```c
// each entry = which bit flipped
typedef struct {
    int      addr;         // 0..20735
    uint8_t  old_bit;      // before
    uint8_t  new_bit;      // after
    uint64_t tick;
} DTRingEntry;

// rewind → unflip the bit
// ffwd  → reflip the bit
// checkpoint = ring position (same as before)
```

No pointer journaling either — just bit indices.

## Why Current Implementation Drifted

1. Historically: SID built on CPU first, used `tensor_set_data()` (pointer swap at offset 248) — works on CPU, breaks on GPU
2. GPU fix: `ggml_backend_tensor_set()` — real memcpy, kills zero-copy
3. Sid cache: brute force preload + compression — solved CPU load speed but not memory
4. DRamTile: mmap copy of all tensors — doubles memory
5. No one realized page table indirect solves both CPU and GPU with the same design

## Status

Not implemented. Current codebase uses old approach (sid_cache + DRamTile + tensor_set_data + ggml_backend_tensor_set).

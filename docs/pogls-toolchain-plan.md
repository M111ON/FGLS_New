# POGLS Toolchain — Foundation-First Build Plan

**Goal**: Build POGLS/FGLS/DGLS into real CLI tools that work on Windows.

**Architecture**: Foundation first — core library with clean platform separation, then CLI tools on top.

**Platform**: Windows-first. Core logic = pure C (no platform dependency). Platform layer = `pogls_platform.h` (Windows implementation, swappable).

**Date**: July 10, 2026

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  CLI Tools                                              │
│  pogls_compress  pogls_decompress  pogls_inspect         │
│  addr_resolve    dramtile_cli      kv_remap_cli          │
├──────────────────────────────────────────────────────────┤
│  POGLS Core Library (pure C, no platform dependency)     │
│  pogls_compress.h   — ZSTD compression API               │
│  pogls_addr.h       — 144² address resolution             │
│  pogls_meta.h       — .pogls file format read/write       │
│  pogls_store.h      — Store API contract                  │
├──────────────────────────────────────────────────────────┤
│  Platform Layer (swappable per OS)                       │
│  pogls_platform.h   — VirtualAlloc, Win32 file I/O       │
│  (future: pogls_platform_linux.h — mmap, POSIX I/O)      │
├──────────────────────────────────────────────────────────┤
│  External: zstd.dll, ggml.dll, llama.dll                 │
└──────────────────────────────────────────────────────────┘
```

---

## Phase 0: Foundation — Core Library

### 0.1 `pogls_platform.h` — Platform Abstraction

**Purpose**: Isolate all Windows-specific code into one file.

**API**:
```c
// Memory mapping
void* pogls_map_file(const char* path, size_t* size_out);    // Read-only mmap
void* pogls_alloc_large(size_t size);                         // VirtualAlloc
void  pogls_free_large(void* ptr, size_t size);               // VirtualFree

// File I/O
FILE* pogls_fopen(const char* path, const char* mode);
int   pogls_fseek(FILE* f, int64_t offset, int origin);
int64_t pogls_ftell(FILE* f);
```

**Implementation**: `#ifdef _WIN32` for VirtualAlloc + _fseeki64, else mmap + fseeko.

**Dependencies**: None (pure C + platform headers)

### 0.2 `pogls_core.h` — Single-Include Header

**Purpose**: One `#include` to get everything.

```c
// pogls_core.h — Single include for POGLS library
#include "pogls_platform.h"
#include "pogls_compress.h"
#include "pogls_addr.h"
#include "pogls_meta.h"
#include "pogls_store.h"
```

### 0.3 `pogls_compress.h` — Compression API (Pure C)

**Purpose**: ZSTD compression with auto-raw fallback. No platform dependency.

**Already exists** in `pogls_meta.h:320-389` as `pogls_compress_tensor()` / `pogls_decompress_tensor()`.

**Extract into standalone header** so CLI tools can include it without the full `pogls_meta.h` file format logic.

**Dependencies**: `zstd.h` (when `POGLS_USE_ZSTD` defined)

### 0.4 `pogls_addr.h` — Address Resolution (Pure C)

**Purpose**: 144² address space. Tensor name → flat address → (macro, micro).

**Already exists** in `addr_space.h`. Extract portable parts.

**Dependencies**: None (pure math)

### 0.5 `pogls_meta.h` — File Format (Pure C, already exists)

**Status**: ✅ Already clean. `pogls_meta_read()` / `pogls_meta_write()` work standalone.

**No changes needed** — just ensure it doesn't pull platform headers.

### 0.6 `pogls_store.h` — Store API Contract (Pure C)

**Purpose**: Define the store interface that `dramtile_store.h` implements.

**Extract** struct definitions and function declarations from `dramtile_store.h` into a clean contract header. Implementation stays in `dramtile_store.h`.

### 0.7 Build: `pogls_core.lib` — Static Library

**Makefile target**:
```makefile
pogls_core.lib: pogls_platform.o pogls_compress.o pogls_addr.o
	ar rcs $@ $^

pogls_platform.o: pogls_platform.c pogls_platform.h
	$(CC) $(CFLAGS) -c -o $@ $<

pogls_compress.o: pogls_compress.c pogls_compress.h
	$(CC) $(CFLAGS) -DPOGLS_USE_ZSTD -c -o $@ $<

pogls_addr.o: pogls_addr.c pogls_addr.h
	$(CC) $(CFLAGS) -c -o $@ $<
```

### Phase 0 Success Criteria
- [ ] `pogls_core.lib` compiles clean (0 warnings)
- [ ] Each header compiles standalone (no missing includes)
- [ ] `pogls_platform.h` abstracts VirtualAlloc/mmap behind unified API
- [ ] All pure-C headers have zero platform #ifdefs
- [ ] `make pogls_core.lib` works

---

## Phase 1: Core CLI Tools

### 1.1 `pogls_compress.exe`
**Purpose**: Compress raw data → POGLS chunk  
**Lines**: ~80  
**Deps**: `pogls_core.lib` + `zstd.dll`

### 1.2 `pogls_decompress.exe`
**Purpose**: Decompress POGLS chunk → raw data  
**Lines**: ~60  
**Deps**: `pogls_core.lib` + `zstd.dll`

### 1.3 `pogls_inspect.exe`
**Purpose**: Dump .pogls metadata to console  
**Lines**: ~120  
**Deps**: `pogls_core.lib` (no zstd)

### 1.4 `pogls_roundtrip.exe`
**Purpose**: Auto roundtrip verify (compress→decompress→compare)  
**Lines**: ~50  
**Deps**: `pogls_core.lib` + `zstd.dll`

### Phase 1 Success Criteria
- [ ] Each tool compiles with `make`
- [ ] Each tool has `--help` flag
- [ ] Roundtrip: compress → decompress → byte-identical
- [ ] Error handling: missing file, bad input → clear error message

---

## Phase 2: Address & Conversion Tools

### 2.1 `addr_resolve.exe`
**Purpose**: Tensor name → 144² address  
**Lines**: ~100  
**Deps**: `pogls_core.lib` (no external DLLs)

### 2.2 `gguf_dump.exe`
**Purpose**: GGUF metadata inspector  
**Lines**: ~150  
**Deps**: None (pure C file I/O)

### Phase 2 Success Criteria
- [ ] `addr_resolve --name "blk.0.attn_q.weight"` → correct address + decomposition
- [ ] `gguf_dump model.gguf` → prints all metadata + tensor list

---

## Phase 3: Store Tools

### 3.1 `dramtile_cli.exe`
**Purpose**: DRamTile store CRUD  
**Lines**: ~200  
**Deps**: `pogls_core.lib` + `geo_dram_tile.h`

### 3.2 `kv_remap_cli.exe`
**Purpose**: KV Remap compress/decompress  
**Lines**: ~150  
**Deps**: `pogls_core.lib` + `kv_remap_diamond.h`

### Phase 3 Success Criteria
- [ ] `dramtile_cli create test.dt --capacity 64` → creates store
- [ ] `dramtile_cli put test.dt --name tensor1 --file data.bin` → stores
- [ ] `dramtile_cli get test.dt --name tensor1 --output out.bin` → retrieves
- [ ] `kv_remap_cli compress delta.kv --output delta.compressed` → works

---

## Phase 4: Integration Pipeline

### 4.1 `pogls_pipeline.exe`
**Purpose**: GGUF → POGLS → verify in one command  
**Lines**: ~200  
**Deps**: `pogls_core.lib` + `zstd.dll` + `gguf_index.h`

### 4.2 Makefile `all` Target Update
```makefile
all: pogls_core.lib \
     pogls_compress.exe pogls_decompress.exe pogls_inspect.exe pogls_roundtrip.exe \
     addr_resolve.exe gguf_dump.exe \
     dramtile_cli.exe kv_remap_cli.exe \
     pogls_pipeline.exe \
     test_swap_all.exe gguf_to_pogls.exe llama_pogls_runner_sid_v2.exe
```

### Phase 4 Success Criteria
- [ ] `make all` builds everything (0 errors)
- [ ] `pogls_pipeline convert model.gguf -o model.pogls --compress` → works
- [ ] `pogls_pipeline verify model.pogls model.gguf` → PASS

---

## Phase 5: Build System & Documentation

### 5.1 Makefile Overhaul
- Separate `all` into `core`, `tools`, `runner`, `tests`
- Add `install` target (copy to `C:\pogls\bin\`)
- Add `clean` target
- Remove hardcoded paths, use env vars

### 5.2 Documentation
- `docs/pogls-cli.md` — User-facing CLI reference
- `docs/pogls-architecture.md` — Technical architecture
- Update `AGENTS.md` — Toolchain status

### Phase 5 Success Criteria
- [ ] `make` (no args) builds core + tools
- [ ] `make tests` builds + runs all tests
- [ ] `docs/pogls-cli.md` covers all tools with examples

---

## Implementation Order

| Step | What | Lines | Deps | Est. Time |
|------|------|-------|------|-----------|
| 0.1 | `pogls_platform.h` | ~60 | None | 30min |
| 0.2 | `pogls_core.h` | ~15 | All above | 10min |
| 0.3 | Extract `pogls_compress.h` | ~80 | zstd | 20min |
| 0.4 | Extract `pogls_addr.h` | ~100 | None | 20min |
| 0.5 | Verify `pogls_meta.h` clean | 0 | None | 10min |
| 0.6 | `pogls_store.h` contract | ~50 | None | 15min |
| 0.7 | `Makefile` core lib target | ~30 | All above | 15min |
| 1.1 | `pogls_compress.exe` | ~80 | Core lib | 30min |
| 1.2 | `pogls_decompress.exe` | ~60 | Core lib | 20min |
| 1.3 | `pogls_inspect.exe` | ~120 | Core lib | 30min |
| 1.4 | `pogls_roundtrip.exe` | ~50 | Core lib | 15min |
| 2.1 | `addr_resolve.exe` | ~100 | Core lib | 30min |
| 2.2 | `gguf_dump.exe` | ~150 | None | 30min |
| 3.1 | `dramtile_cli.exe` | ~200 | Core lib | 45min |
| 3.2 | `kv_remap_cli.exe` | ~150 | Core lib | 30min |
| 4.1 | `pogls_pipeline.exe` | ~200 | Core lib | 45min |
| 4.2 | Makefile `all` update | ~30 | All above | 15min |
| 5.1 | Makefile overhaul | ~100 | All above | 30min |
| 5.2 | Documentation | ~200 | All above | 30min |
| **Total** | | **~1,675** | | **~8 hours** |

---

## File Structure (after Phase 0)

```
runner/
├── pogls_core/              ← NEW: Core library
│   ├── pogls_core.h         ← Single include
│   ├── pogls_platform.h     ← Windows platform layer
│   ├── pogls_compress.h     ← Compression API (extracted from pogls_meta.h)
│   ├── pogls_addr.h         ← Address API (extracted from addr_space.h)
│   ├── pogls_meta.h         ← File format (existing, verify clean)
│   ├── pogls_store.h        ← Store contract (extracted from dramtile_store.h)
│   ├── pogls_platform.c     ← Windows implementation
│   ├── pogls_compress.c     ← Compression implementation
│   └── pogls_addr.c         ← Address implementation
├── pogls_tools/             ← NEW: CLI tools
│   ├── pogls_compress.c
│   ├── pogls_decompress.c
│   ├── pogls_inspect.c
│   ├── pogls_roundtrip.c
│   ├── addr_resolve.c
│   ├── gguf_dump.c
│   ├── dramtile_cli.c
│   ├── kv_remap_cli.c
│   └── pogls_pipeline.c
├── Makefile                 ← Updated with core lib + tools targets
├── addr_space.h             ← Existing (kept for backward compat)
├── dramtile_store.h         ← Existing (implementation)
├── kv_remap.h               ← Existing
├── pogls_meta.h             ← Existing (file format)
└── ... (existing files unchanged)
```

---

## Key Design Decisions

1. **Core library = static .lib** — each tool links `pogls_core.lib`, no DLL hell
2. **Platform layer = one file** — `pogls_platform.h` handles VirtualAlloc/mmap
3. **Pure C headers** — zero platform #ifdefs in business logic
4. **Windows-first** — primary target, no cross-platform burden now
5. **Existing files untouched** — new structure alongside existing code, no breaking changes
6. **Backward compatible** — `gguf_to_pogls.exe`, `berb.py`, tests all still work

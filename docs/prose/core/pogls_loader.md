# pogls_loader.h

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_core`  
**Path:** `pogls_core/pogls_loader.h`  
**Status:** `active`  
**Note:** modified 18d ago; included by 3 file(s)  
**Generated:** 2026-07-29 23:38  

## Description

* pogls_loader.h — Standalone POGLS Loader Library
* Zero-dependency API for loading POGLS files from any C/C++ runner.
* Works with llama.cpp, Ollama, vLLM, or any custom runner.
* Features:
*   - mmap-based zero-copy loading
*   - O(1) tensor lookup by name or address
*   - Per-tensor decompression (ZSTD/RAW)
*   - Thread-safe read-only access
*   - No platform dependencies (pure C)
* Usage:
*   #include "pogls_loader.h"
*   PoglsLoader *L = pogls_open("model.pogls");
*   if (L) {
*       void *data = pogls_tensor_data(L, "blk.0.attn_q.weight");
*       size_t sz = pogls_tensor_size(L, "blk.0.attn_q.weight");
*       // use data...
*       pogls_close(L);
*   }
* Build:
*   gcc -O2 -std=c11 -c pogls_loader.c -o pogls_loader.o

## Structures

- `typedef struct PoglsLoader PoglsLoader`
- `typedef struct`
- `typedef struct`
- `struct PoglsLoader`
- `struct`
- `struct stat st`

## API Functions

- `PoglsLoader* pogls_open(const char *path)`
- `void pogls_close(PoglsLoader *L)`
- `uint32_t pogls_tensor_count(PoglsLoader *L)`
- `uint32_t pogls_version(PoglsLoader *L)`
- `uint32_t pogls_model_meta_size(PoglsLoader *L)`
- `uint32_t pogls_tensor_addr(PoglsLoader *L, uint32_t idx)`
- `uint32_t pogls_tensor_dtype(PoglsLoader *L, uint32_t idx)`
- `uint32_t pogls_tensor_ndim(PoglsLoader *L, uint32_t idx)`
- `void pogls_tensor_shape(PoglsLoader *L, uint32_t idx, uint32_t dims[4])`
- `size_t pogls_tensor_nbytes(PoglsLoader *L, uint32_t idx)`
- `size_t pogls_tensor_comp_nbytes(PoglsLoader *L, uint32_t idx)`
- `uint32_t pogls_tensor_comp_type(PoglsLoader *L, uint32_t idx)`
- `int pogls_find_tensor(PoglsLoader *L, const char *name)`
- `void* pogls_tensor_data(PoglsLoader *L, const char *name)`
- `size_t pogls_tensor_size(PoglsLoader *L, const char *name)`
- `int pogls_find_tensor_by_addr(PoglsLoader *L, uint32_t addr)`
- `void* pogls_tensor_data_by_addr(PoglsLoader *L, uint32_t addr)`
- `uint32_t pogls_face_addr(uint32_t addr, int face)`
- `void* pogls_tensor_data_face(PoglsLoader *L, const char *name, int face)`
- `uint64_t pogls_file_size(PoglsLoader *L)`

## Constants

- `#define POGLS_LOADER_H`
- `#define POGLS_LOADER_MAGIC_V1  0x504F474Cu  /* "POGL" — v1 legacy */`
- `#define POGLS_LOADER_MAGIC_V2  0x53474F50u  /* "POGS" — v2 current */`
- `#define POGLS_LOADER_COMP_RAW   0u`
- `#define POGLS_LOADER_COMP_ZSTD  1u`
- `#define POGLS_LOADER_COMP_SHELL 2u`
- `#define POGLS_LOADER_COMP_DELTA 3u`
- `#define WIN32_LEAN_AND_MEAN`
- `#define POGLS_L_MAX_ADDR    20736u`
- `#define POGLS_L_HEADER_SZ   128u`
- `#define POGLS_L_INDEX_SZ    (POGLS_L_MAX_ADDR * 16u)`
- `#define POGLS_L_NAME_LEN    64u`
- `#define POGLS_L_MAX_TENSORS 4096u`


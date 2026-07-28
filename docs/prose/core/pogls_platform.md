# pogls_platform.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_core`  
**Path:** `pogls_core/pogls_platform.c`  
**Status:** `active`  
**Note:** modified 16d ago  
**Generated:** 2026-07-28 10:23  

## Structures

- `struct stat st`

## API Functions

- `void* pogls_map_file(const char *path, size_t *size_out)`
- `void* pogls_alloc_large(size_t size)`
- `return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)`
- `void pogls_free_large(void *ptr, size_t size)`
- `FILE* pogls_fopen(const char *path, const char *mode)`
- `return fopen(path, mode)`
- `int pogls_fseek(FILE *f, int64_t offset, int origin)`
- `return _fseeki64(f, offset, origin)`
- `return fseeko(f, offset, origin)`
- `int64_t pogls_ftell(FILE *f)`
- `return _ftelli64(f)`
- `return ftello(f)`
- `int64_t pogls_fsize(const char *path)`
- `uint32_t pogls_xorshift32(uint32_t *state)`


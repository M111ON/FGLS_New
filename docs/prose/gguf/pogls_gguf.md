# pogls_gguf.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_gguf`  
**Path:** `pogls_gguf/pogls_gguf.c`  
**Status:** `active`  
**Note:** modified 16d ago  
**Generated:** 2026-07-28 10:24  

## API Functions

- `size_t pogls_gguf_type_size(uint32_t dtype)`
- `int pogls_gguf_block_size(uint32_t dtype)`
- `int pogls_gguf_is_q4(const PoglsGgufReader *r, uint32_t idx)`
- `int pogls_gguf_type_is_kquant(uint32_t dtype)`
- `static int skip_kv(FILE *f, uint64_t n_kv)`
- `int pogls_gguf_open(const char *path, PoglsGgufReader *r)`
- `int pogls_gguf_read_tensor(const char *path, const PoglsGgufReader *r,`
- `void pogls_gguf_close(PoglsGgufReader *r)`
- `int pogls_gguf_idx_open(const char *path, PoglsGgufIndex *idx)`
- `void pogls_gguf_idx_close(PoglsGgufIndex *idx)`
- `uint64_t pogls_gguf_idx_tensor_off(const PoglsGgufIndex *idx, uint64_t i)`
- `int pogls_gguf_idx_meta_blob(const char *path,`


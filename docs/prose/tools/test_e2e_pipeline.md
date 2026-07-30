# test_e2e_pipeline.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_tools`  
**Path:** `pogls_tools/test_e2e_pipeline.c`  
**Status:** `active`  
**Note:** modified 18d ago  
**Generated:** 2026-07-30 03:43  

## Description

* test_e2e_pipeline.c — End-to-end pipeline test
* Tests: GGUF → POGLS → verify → loader → compare tensor data
* Build:
*   gcc -O2 -std=c11 -DPOGLS_LOADER_IMPLEMENTATION -o test_e2e_pipeline.exe test_e2e_pipeline.c -lm

## API Functions

- `static int read_gguf_tensor(const char *gguf_path, uint64_t offset, size_t sz, void *buf)`
- `int main(int argc, char **argv)`

## Constants

- `#define POGLS_LOADER_IMPLEMENTATION`
- `#define TEST(name, cond) do { \`


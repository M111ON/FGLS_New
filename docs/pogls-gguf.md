# pogls_gguf — Standalone GGUF Reader

Zero-dependency GGUF file parser. No llama.cpp headers required. Handles
standard GGUF files (magic `0x46554747`) with full type table support for all
common GGML quantization formats.

## Architecture

Two reader types trade off convenience vs. footprint:

| Reader | Purpose | Loads | Field Types |
|---|---|---|---|
| `PoglsGgufReader` | Full tensor access | names, offsets (u64), sizes (u32), dtypes in memory | `uint32_t n_tensors`, `uint64_t data_offset` |
| `PoglsGgufIndex` | Metadata / position only | same fields but sizes as `uint64_t`, plus `data_sec_off` | `uint64_t n_tensors`, separate reader + `idx_tensor_off()` |

**PoglsGgufReader** is for callers that need to read tensor data by index. It
stores per-tensor sizes as `uint32_t` (max ~4 GB per tensor) and a single
`data_offset` that applies to all tensor offsets.

**PoglsGgufIndex** is lighter — the `data_sec_off` field stores the absolute
file position of the data section, so `pogls_gguf_idx_tensor_off()` returns the
absolute file offset directly. `pogls_gguf_idx_meta_blob()` can dump the entire
metadata section (header + KV + tensor info) as a single blob.

Both types are trivially copyable and own no external resources beyond their
allocated arrays.

## Type Info Table

The type table maps GGML type IDs to per-block byte size and block size
(number of elements). Types 4 and 5 are unhandled (zero entries).

| ID | Name | Bytes/Block | Elements/Block |
|---|---|---|---|
| 0 | `F32` | 4 | 1 |
| 1 | `F16` | 2 | 1 |
| 2 | `Q4_0` | 18 | 32 |
| 3 | `Q4_1` | 20 | 32 |
| 6 | `Q5_0` | 22 | 32 |
| 7 | `Q5_1` | 24 | 32 |
| 8 | `Q8_0` | 34 | 32 |
| 9 | `Q8_1` | 36 | 32 |
| 10 | `Q2_K` | 84 | 256 |
| 11 | `Q3_K` | 110 | 256 |
| 12 | `Q4_K` | 144 | 256 |
| 13 | `Q5_K` | 176 | 256 |
| 14 | `Q6_K` | 210 | 256 |
| 15 | `Q8_K` | 292 | 256 |
| 16 | `IQ2_XXS` | 2 | 256 |
| 17 | `IQ2_XS` | 2 | 256 |
| 18 | `IQ3_XXS` | 2 | 256 |
| 19 | `IQ1_S` | 1 | 256 |
| 20 | `IQ4_NL` | 2 | 32 |
| 21 | `IQ3_S` | 1 | 256 |
| 22 | `IQ2_S` | 1 | 256 |
| 23 | `IQ4_XS` | 2 | 256 |
| 24 | *(reserved)* | 1 | 1 |
| 25 | *(reserved)* | 2 | 1 |
| 26 | *(reserved)* | 4 | 1 |
| 27 | `F64` | 8 | 1 |
| 28 | *(reserved)* | 8 | 1 |
| 29 | `IQ1_M` | 1 | 256 |
| 30 | *(reserved)* | 2 | 1 |

Unknown type IDs (>30) default to 4 bytes per element, block size 1.

## API Reference

### Readers

```c
int pogls_gguf_open(const char *path, PoglsGgufReader *r);
```

Open a GGUF file and read the full tensor index (names, offsets, sizes,
dtypes). Returns 0 on success, -1 on I/O or parse error. Allocates internal
arrays — call `pogls_gguf_close()` when done.

```c
int pogls_gguf_read_tensor(const char *path, const PoglsGgufReader *r,
                           uint32_t idx, uint8_t *buf, uint32_t cap);
```

Read tensor `idx` data into `buf` (at least `r->sizes[idx]` bytes).
Returns 0 on success, negative on error:
- `-1`: index out of range
- `-2`: buffer too small (`sizes[idx] > cap`)
- `-3`: can't reopen file
- `-4`: seek failure
- `-5`: read failure

```c
void pogls_gguf_close(PoglsGgufReader *r);
```

Free all arrays and zero out the struct.

### Index Reader

```c
int pogls_gguf_idx_open(const char *path, PoglsGgufIndex *idx);
```

Open a GGUF file and read only the tensor index (no data section parsing).
Sizes are stored as `uint64_t`. Call `pogls_gguf_idx_close()` when done.

```c
void pogls_gguf_idx_close(PoglsGgufIndex *idx);
```

Free all arrays and zero out the struct.

```c
uint64_t pogls_gguf_idx_tensor_off(const PoglsGgufIndex *idx, uint64_t i);
```

Absolute file offset of tensor `i`'s data: `data_sec_off + offsets[i]`.

```c
int pogls_gguf_idx_meta_blob(const char *path,
                              uint8_t **blob_out, uint64_t *size_out);
```

Read the entire metadata section (header + KV metadata + tensor info) into a
heap-allocated blob. The caller owns the buffer (`free()` when done).
Returns 0 on success, -1 on error.

### Type Queries

```c
int pogls_gguf_is_q4(const PoglsGgufReader *r, uint32_t idx);
```

Returns 1 if tensor `idx` is a Q4-quantized type (IDs 2, 3, or 12).

```c
int pogls_gguf_type_is_kquant(uint32_t dtype);
```

Returns 1 if `dtype` is a K-quant format (IDs 10–19, 21–23, 29).

```c
size_t pogls_gguf_type_size(uint32_t dtype);
int    pogls_gguf_block_size(uint32_t dtype);
```

Type properties — byte size per block and number of elements per block.
Unknown types (>30) return 4 and 1 respectively.

## File Format Understanding

GGUF layout:

```
[  0] Magic           — 4B, 0x46554747 ("GGUF")
[  4] Version         — 4B (typically 2 or 3)
[  8] Tensor count    — 8B (uint64_t)
[ 16] KV count        — 8B (uint64_t)
[ 24] KV metadata     — variable size, key-value pairs
            key      : uint64_t length + UTF-8 bytes
            value    : uint32_t type + payload (string, int, float, array, etc.)
[...] Tensor info     — one entry per tensor
            name     : uint64_t length + name bytes
            n_dims   : uint32_t
            dims[]   : n_dims × uint64_t
            dtype    : uint32_t (GGML type ID)
            offset   : uint64_t (offset within data section)
[...] Padding          — padded to 32-byte alignment (POGLS_GGUF_ALIGN)
[...] Tensor data     — raw tensor bytes at each tensor's offset
```

**KV skip methodology**: The `skip_kv()` function iterates over KV pairs,
reading the key (length-prefixed string), then dispatching on value type to
advance the correct number of bytes. Arrays and strings are handled
recursively. Types not in the known set return `-1` (parse failure).

**32-byte alignment**: After scanning all tensor info entries, the file
position is rounded up to the next multiple of 32 bytes. This is the data
section start. The `data_offset` (Reader) or `data_sec_off` (Index) stores
this value.

**Tensor size calculation**: After reading dimensions, dtype, and per-tensor
size metadata, the size is computed as `(ne / block_size) * type_size` where
`ne` is the product of all dimensions. `pogls_gguf_read_tensor` uses this
size to read the exact tensor bytes.

## Usage Example

```c
#include "pogls_gguf.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s model.gguf\n", argv[0]); return 1; }

    PoglsGgufReader r;
    if (pogls_gguf_open(argv[1], &r) != 0) {
        fprintf(stderr, "Failed to open %s\n", argv[1]);
        return 1;
    }

    printf("File: %s\n", argv[1]);
    printf("Tensors: %u\n", r.n_tensors);
    printf("Data section offset: %llu\n", (unsigned long long)r.data_offset);

    for (uint32_t i = 0; i < r.n_tensors && i < 10; i++) {
        printf("  [%u] %-40s size=%7u  dtype=%2u  offset=%llu\n",
               i, r.names[i], r.sizes[i], r.dtypes[i],
               (unsigned long long)r.offsets[i]);
    }

    // Read tensor 0 into a buffer
    uint8_t *buf = (uint8_t*)malloc(r.sizes[0]);
    if (pogls_gguf_read_tensor(argv[1], &r, 0, buf, r.sizes[0]) == 0) {
        printf("Tensor 0 read OK (%u bytes)\n", r.sizes[0]);
    }
    free(buf);

    // Lightweight index variant — metadata only
    PoglsGgufIndex idx;
    if (pogls_gguf_idx_open(argv[1], &idx) == 0) {
        printf("Index: %llu tensors, data at offset %llu\n",
               (unsigned long long)idx.n_tensors,
               (unsigned long long)idx.data_sec_off);
        for (uint64_t i = 0; i < idx.n_tensors && i < 5; i++) {
            printf("  [%llu] %s  off=%llu  sz=%llu\n",
                   (unsigned long long)i, idx.names[i],
                   (unsigned long long)pogls_gguf_idx_tensor_off(&idx, i),
                   (unsigned long long)idx.sizes[i]);
        }
        pogls_gguf_idx_close(&idx);
    }

    // Dump metadata blob
    uint8_t *meta;
    uint64_t meta_sz;
    if (pogls_gguf_idx_meta_blob(argv[1], &meta, &meta_sz) == 0) {
        printf("Metadata blob: %llu bytes\n", (unsigned long long)meta_sz);
        free(meta);
    }

    pogls_gguf_close(&r);
    return 0;
}
```

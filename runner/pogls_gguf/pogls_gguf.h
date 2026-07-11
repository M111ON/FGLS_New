#ifndef POGLS_GGUF_H
#define POGLS_GGUF_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POGLS_GGUF_MAGIC   0x46554747u
#define POGLS_GGUF_ALIGN   32u

typedef struct {
    uint32_t n_tensors;
    uint64_t data_offset;
    uint64_t *offsets;
    uint32_t *sizes;
    char   **names;
    uint32_t *dtypes;
} PoglsGgufReader;

typedef struct {
    uint64_t n_tensors;
    char   **names;
    uint32_t *dtypes;
    uint64_t *offsets;
    uint64_t *sizes;
    uint64_t  data_sec_off;
} PoglsGgufIndex;

int  pogls_gguf_open(const char *path, PoglsGgufReader *r);
int  pogls_gguf_read_tensor(const char *path, const PoglsGgufReader *r,
                            uint32_t idx, uint8_t *buf, uint32_t cap);
void pogls_gguf_close(PoglsGgufReader *r);

int  pogls_gguf_idx_open(const char *path, PoglsGgufIndex *idx);
void pogls_gguf_idx_close(PoglsGgufIndex *idx);
uint64_t pogls_gguf_idx_tensor_off(const PoglsGgufIndex *idx, uint64_t i);
int  pogls_gguf_idx_meta_blob(const char *path,
                               uint8_t **blob_out, uint64_t *size_out);

int  pogls_gguf_is_q4(const PoglsGgufReader *r, uint32_t idx);
int  pogls_gguf_type_is_kquant(uint32_t dtype);

size_t pogls_gguf_type_size(uint32_t dtype);
int    pogls_gguf_block_size(uint32_t dtype);

#ifdef __cplusplus
}
#endif

#endif /* POGLS_GGUF_H */

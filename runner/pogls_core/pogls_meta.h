#ifndef POGLS_CORE_META_H
#define POGLS_CORE_META_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POGLS_META_MAGIC     0x53474F50u
#define POGLS_META_VERSION   2u
#define POGLS_MAX_ADDR       20736u
#define POGLS_HEADER_SZ      128u
#define POGLS_INDEX_SZ       ((uint64_t)POGLS_MAX_ADDR * 16u)

#define POGLS_FLAG_HAS_TMETA 0x0001u
#define POGLS_FLAG_HAS_MMETA 0x0002u
#define POGLS_FLAG_MCOMPRESS 0x0004u

#define POGLS_NAME_LEN 64

typedef struct {
    uint32_t addr;
    uint32_t dtype;
    uint32_t ndim;
    uint32_t nbytes_orig;
    uint32_t comp_type;
    uint32_t comp_nbytes;
    uint32_t dims[4];
    char     name[POGLS_NAME_LEN];
} PoglsTensorMeta;

#define POGLS_META_ENTRY_SZ  ((uint32_t)sizeof(PoglsTensorMeta))

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_tensors;
    uint32_t flags;
    uint64_t tensor_meta_off;
    uint32_t tensor_meta_count;
    uint32_t _pad0;
    uint64_t model_meta_off;
    uint32_t model_meta_sz;
    uint64_t gguf_path_off;
    uint32_t gguf_path_sz;
    uint8_t  _pad[72];
} PoglsStoreHeader;

typedef struct {
    uint32_t n_layers;
    uint32_t n_heads;
    uint32_t n_head_kv;
    uint32_t n_embd;
    uint32_t n_ff;
    uint32_t n_expert;
    uint32_t n_expert_used;
    uint32_t ftype;
    uint64_t n_params;
    uint32_t n_tensors;
    char     arch[16];
    char     desc[64];
} PoglsModelMeta;

void pogls_meta_header_init(PoglsStoreHeader *hdr);

uint64_t pogls_meta_data_off(const PoglsStoreHeader *hdr);

void pogls_meta_entry_init(PoglsTensorMeta *e);

int  pogls_meta_read(const char *path,
                     PoglsStoreHeader *hdr_out,
                     uint8_t *idx_out,
                     void *meta_out,
                     uint64_t *data_off_out);

int  pogls_meta_write(const char *path,
                      const PoglsStoreHeader *hdr,
                      const uint8_t *idx,
                      const PoglsTensorMeta *meta,
                      const uint8_t *model_meta,
                      const uint8_t *data,
                      uint64_t data_sz);

const PoglsTensorMeta* pogls_meta_find(const PoglsTensorMeta *meta,
                                        uint32_t count, uint32_t addr);

const PoglsTensorMeta* pogls_meta_find_name(const PoglsTensorMeta *meta,
                                             uint32_t count, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* POGLS_CORE_META_H */

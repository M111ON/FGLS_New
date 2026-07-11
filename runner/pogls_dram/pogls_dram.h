#pragma once
#ifndef POGLS_DRAM_H
#define POGLS_DRAM_H

#include <stdint.h>
#include <stddef.h>
#include "../pogls_core/pogls_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POGLS_DRAM_MAX_ADDR     20736u
#define POGLS_DRAM_KV_FLAG      0x80000000u
#define POGLS_DRAM_MAX_DIR      512u

typedef struct {
    char     name[64];
    uint32_t dram_addr;
    uint32_t nbytes;
    uint32_t session_tick;
    uint8_t  flags;
    uint32_t _pad;
} PoglsDramEntry;

/* ── Store structure (fields are internal, access via API only) ── */
#define POGLS_DRAM_PATH_MAX  260u

typedef struct {
    uint8_t  *base;         /* VirtualAlloc/mmap base */
    size_t    capacity;     /* total arena capacity in bytes */
    size_t    used;         /* bytes consumed in arena */
    uint32_t  max_entries;  /* directory slot count */
    uint32_t  n_entries;    /* active entries */
    uint32_t  session_tick; /* monotonic counter */
    void     *entries;      /* internal: directory entries array */
    uint8_t  *arena;        /* data arena start */
    char      filepath[POGLS_DRAM_PATH_MAX];
} PoglsDramStore;

int      pogls_dram_open(PoglsDramStore *store, const char *path, size_t capacity);
void     pogls_dram_close(PoglsDramStore *store);
int      pogls_dram_save(PoglsDramStore *store, const char *path, int is_kv);
int      pogls_dram_put(PoglsDramStore *store, const char *name, uint32_t addr, const void *data, size_t sz);
void*    pogls_dram_get(PoglsDramStore *store, uint32_t addr, size_t *sz_out);
void*    pogls_dram_get_name(PoglsDramStore *store, const char *name, size_t *sz_out);
int      pogls_dram_free(PoglsDramStore *store, uint32_t addr);
int      pogls_dram_has(PoglsDramStore *store, uint32_t addr);
uint32_t pogls_dram_count(PoglsDramStore *store);
uint64_t pogls_dram_bytes(PoglsDramStore *store);

#ifdef __cplusplus
}
#endif

#endif /* POGLS_DRAM_H */

/*
 * pogls_v3_geopixel.h — POGLS v3: Header-Only Geopixel Store
 *
 * Core principle: weights stay in GGUF. POGLS is a compact lookup header
 * that maps tensor names → GGUF offsets. Binary search O(log n) lookup.
 *
 * File layout (v3):
 *   [Header       128B]  magic + version(3) + n_tensors + flags +
 *                        gguf_rel_path + entries_off + reserved
 *   [Entries        ...]  n_tensors × PoglsV3Entry (80B each, sorted by name)
 *   [GGUF Path     ...]  null-terminated relative GGUF path
 *
 * Total for 256-tensor model: 128 + 256×80 + path ≈ 20 KB
 * Compare to v2: 128 + 331776 + 256×64 + 5.15GB data = 5.15GB
 */

#ifndef POGLS_V3_GEOPIXEL_H
#define POGLS_V3_GEOPIXEL_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pogls_meta.h"
#include "addr_space.h"

#define POGLS_V3_MAGIC     0x53474F50u
#define POGLS_V3_VERSION   3u

#define POGLS_V3_FLAG_HAS_GGUF   0x0001u
#define POGLS_V3_FLAG_RELATIVE   0x0002u

/* Per-Tensor Entry (80 bytes, sorted by name) */
typedef struct {
    uint32_t addr;
    uint32_t dtype;
    uint32_t ndim;
    uint32_t nbytes;
    uint64_t gguf_offset;
    uint32_t dims[4];
    char     name[40];
} PoglsV3Entry;

#define POGLS_V3_ENTRY_SZ  80u

/* V3 Header (128 bytes) */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_tensors;
    uint32_t flags;
    uint32_t gguf_path_off;
    uint16_t gguf_path_sz;
    uint16_t _pad0;
    uint32_t entries_off;
    uint32_t _pad1;
    uint8_t  _pad2[100];
} PoglsV3Header;

static inline void pogls_v3_header_init(PoglsV3Header *hdr) {
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic   = POGLS_V3_MAGIC;
    hdr->version = POGLS_V3_VERSION;
    hdr->entries_off = sizeof(PoglsV3Header);
}

static inline uint64_t pogls_v3_total_size(const PoglsV3Header *hdr) {
    uint64_t end = (uint64_t)hdr->entries_off +
                   (uint64_t)hdr->n_tensors * POGLS_V3_ENTRY_SZ;
    if (hdr->gguf_path_off > 0) {
        uint64_t pend = hdr->gguf_path_off + hdr->gguf_path_sz;
        if (pend > end) end = pend;
    }
    return end;
}

static inline int pogls_v3_cmp_name(const void *a, const void *b) {
    return strcmp(((const PoglsV3Entry *)a)->name,
                 ((const PoglsV3Entry *)b)->name);
}

static inline const PoglsV3Entry *pogls_v3_seek(
    const PoglsV3Entry *entries, uint32_t n, const char *name)
{
    PoglsV3Entry key;
    memset(&key, 0, sizeof(key));
    strncpy(key.name, name, sizeof(key.name) - 1);
    return (const PoglsV3Entry *)bsearch(&key, entries, n,
                                         sizeof(PoglsV3Entry), pogls_v3_cmp_name);
}

#endif

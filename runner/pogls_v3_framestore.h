#ifndef POGLS_V3_FRAMESTORE_H
#define POGLS_V3_FRAMESTORE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define FRAMESTORE_MAGIC     0x45534D46u   /* "FMSE" */
#define FRAMESTORE_VERSION   3u

#define FRAMESTORE_HEADER_SZ 32u
#define FRAMESTORE_ENTRY_SZ  16u

#define FRAMESTORE_FLAG_NONE 0u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_tensors;
    uint32_t addr_map_off;
    uint32_t data_off;
    uint32_t flags;
    uint8_t  _pad[8];
} FrameStoreHeader;

typedef struct {
    uint32_t addr;
    uint32_t nbytes;
    uint64_t file_offset;
} FrameStoreEntry;

static inline void framestore_header_init(FrameStoreHeader *hdr) {
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic   = FRAMESTORE_MAGIC;
    hdr->version = FRAMESTORE_VERSION;
    hdr->addr_map_off = FRAMESTORE_HEADER_SZ;
}

static inline int framestore_cmp_addr(const void *a, const void *b) {
    uint32_t aa = ((const FrameStoreEntry *)a)->addr;
    uint32_t bb = ((const FrameStoreEntry *)b)->addr;
    return (aa > bb) - (aa < bb);
}

static inline const FrameStoreEntry *framestore_seek(
    const FrameStoreEntry *entries, uint32_t n, uint32_t addr)
{
    FrameStoreEntry key;
    key.addr = addr;
    return (const FrameStoreEntry *)bsearch(
        &key, entries, n, sizeof(FrameStoreEntry), framestore_cmp_addr);
}

static inline int framestore_read_header(const char *path, FrameStoreHeader *hdr) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int r = (fread(hdr, FRAMESTORE_HEADER_SZ, 1, f) == 1) ? 0 : -1;
    fclose(f);
    if (r == 0 && (hdr->magic != FRAMESTORE_MAGIC || hdr->version != FRAMESTORE_VERSION))
        return -1;
    return r;
}

static inline int framestore_write(const char *path,
    const FrameStoreHeader *hdr, const FrameStoreEntry *entries,
    const uint8_t *const *tensor_data, const uint32_t *tensor_sizes)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    if (fwrite(hdr, FRAMESTORE_HEADER_SZ, 1, f) != 1) { fclose(f); return -1; }

    uint32_t n = hdr->n_tensors;
    if (fwrite(entries, FRAMESTORE_ENTRY_SZ, n, f) != (size_t)n) { fclose(f); return -1; }

    for (uint32_t i = 0; i < n; i++) {
        if (tensor_sizes[i] > 0 && tensor_data[i] != NULL) {
            if (fwrite(tensor_data[i], tensor_sizes[i], 1, f) != 1) { fclose(f); return -1; }
        }
    }

    fclose(f);
    return 0;
}

#endif /* POGLS_V3_FRAMESTORE_H */

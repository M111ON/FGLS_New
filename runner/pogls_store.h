#ifndef POGLS_STORE_H
#define POGLS_STORE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Portable 64-bit file seek */
#if defined(__MINGW32__)
  #define pogls_fseek64 fseeko64
#else
  #define pogls_fseek64 _fseeki64
#endif

#define POGLS_STORE_MAGIC     0x53474F50
#define POGLS_STORE_VERSION   1
#ifndef POGLS_MAX_ADDR
#define POGLS_MAX_ADDR   20736
#endif

/*
 * POGLS Flat Tensor Store
 * ─────────────────────
 * File layout:
 *   [Header      64B]  magic(4) + version(4) + n_tensors(4) + flags(4) + reserved(48)
 *   [Index   331776B]  addr[0..20735]: {file_offset:u64, nbytes:u32, pad:u32} = 16B each
 *   [Data          ]  tensor raw data packed sequentially
 *
 * addr → file_offset: O(1) via index[addr].
 * mmap the whole file → pointer to any tensor data.
 * Zero-copy: read = return base + index[addr].offset.
 */

typedef struct {
    uint64_t offset;   /* file offset to tensor raw data */
    uint32_t nbytes;   /* tensor size in bytes */
    uint32_t _pad;
} PoglsStoreEntry;     /* 16B */

typedef struct {
    uint32_t       magic;
    uint32_t       version;
    uint32_t       n_tensors;
    uint32_t       flags;
    uint8_t        _pad[48];
    PoglsStoreEntry idx[POGLS_MAX_ADDR];
} PoglsStore;        /* 64 + 20736×16 = 331,840B header+index */

static inline int pogls_store_init(PoglsStore *s) {
    memset(s, 0, sizeof(*s));
    s->magic   = POGLS_STORE_MAGIC;
    s->version = POGLS_STORE_VERSION;
    return 0;
}

static inline int pogls_store_write(const char *path, const PoglsStore *s) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(s, sizeof(*s), 1, f);
    fclose(f);
    return 0;
}

static inline int pogls_store_read_header(const char *path, PoglsStore *s) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(s, sizeof(*s), 1, f);
    fclose(f);
    if (r != 1 || s->magic != POGLS_STORE_MAGIC) return -1;
    return 0;
}

static inline int pogls_store_read_tensor(const char *path, uint32_t addr,
                                           uint8_t *buf, uint32_t max_sz) {
    PoglsStore s;
    if (pogls_store_read_header(path, &s) != 0) return -1;
    if (addr >= POGLS_MAX_ADDR) return -1;
    PoglsStoreEntry *e = &s.idx[addr];
    if (e->offset == 0 || e->nbytes == 0) return -1;
    if (e->nbytes > max_sz) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    pogls_fseek64(f, (__int64)e->offset, SEEK_SET);
    size_t r = fread(buf, 1, e->nbytes, f);
    fclose(f);
    return (r == e->nbytes) ? (int)e->nbytes : -1;
}

static inline const uint8_t *pogls_store_ptr(PoglsStore *s, uint32_t addr) {
    if (addr >= POGLS_MAX_ADDR) return NULL;
    PoglsStoreEntry *e = &s->idx[addr];
    if (e->offset == 0 || e->nbytes == 0) return NULL;
    return (const uint8_t*)s + e->offset;
}

/* Add a tensor to the store (appends data after current end) */
static inline int pogls_store_add(PoglsStore *s, uint32_t addr,
                                   const uint8_t *data, uint32_t nbytes) {
    if (addr >= POGLS_MAX_ADDR) return -1;
    if (s->idx[addr].offset != 0) return -1; /* already exists */
    /* offset = sizeof(PoglsStore) + total_data_so_far */
    uint64_t base = sizeof(*s);
    uint64_t off  = base;
    for (uint32_t a = 0; a < POGLS_MAX_ADDR; a++)
        if (s->idx[a].offset > 0)
            off += s->idx[a].nbytes;
    s->idx[addr].offset  = off;
    s->idx[addr].nbytes  = nbytes;
    s->n_tensors++;
    return 0;
}

/* Reconstruct from raw data: init + add all tensors */
static inline int pogls_store_build(PoglsStore *s,
                                     const uint32_t *addrs,
                                     const uint8_t *const *datas,
                                     const uint32_t *nbytess,
                                     uint32_t count) {
    pogls_store_init(s);
    uint64_t off = sizeof(*s);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = addrs[i];
        if (a >= POGLS_MAX_ADDR) continue;
        s->idx[a].offset  = off;
        s->idx[a].nbytes  = nbytess[i];
        off += nbytess[i];
        s->n_tensors++;
    }
    return 0;
}

#endif /* POGLS_STORE_H */

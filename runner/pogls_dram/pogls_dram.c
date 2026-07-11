#include "pogls_dram.h"
#include <string.h>
#include <stdlib.h>

#define POGLS_DRAM_MAGIC     0x50445241u
#define POGLS_DRAM_VERSION   1u
#define POGLS_DRAM_HDR_SZ    64u
#define POGLS_DRAM_PATH_MAX  260u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t capacity;
    uint64_t used;
    uint32_t n_entries;
    uint32_t max_entries;
    uint8_t  pad[32];
} StoreHeader;

typedef struct {
    char     name[64];
    uint32_t dram_addr;
    uint32_t nbytes;
    uint32_t offset;
    uint32_t session_tick;
    uint8_t  flags;
    uint8_t  _pad[3];
} DirEntry;

/* DirEntry is internal — not exposed in the public header */
#define DIR_ENTRIES(store)  ((DirEntry*)(store)->entries)

#define POGLS_DRAM_HDR_ENTRIES(base, max_entries) \
    ((DirEntry*)((uint8_t*)(base) + POGLS_DRAM_HDR_SZ))

#define POGLS_DRAM_HDR_ARENA(base, max_entries) \
    ((uint8_t*)(base) + POGLS_DRAM_HDR_SZ + (size_t)(max_entries) * sizeof(DirEntry))

static size_t dir_bytes(uint32_t max_entries) {
    return (size_t)max_entries * sizeof(DirEntry);
}

static size_t store_total_size(uint32_t max_entries, size_t capacity) {
    return POGLS_DRAM_HDR_SZ + dir_bytes(max_entries) + capacity;
}

static DirEntry* find_by_addr(PoglsDramStore *store, uint32_t addr) {
    uint32_t masked = addr & ~POGLS_DRAM_KV_FLAG;
    for (uint32_t i = 0; i < store->max_entries; i++) {
        DirEntry *de = &DIR_ENTRIES(store)[i];
        if (de->dram_addr != 0 &&
            (de->dram_addr & ~POGLS_DRAM_KV_FLAG) == masked &&
            (de->dram_addr & POGLS_DRAM_KV_FLAG) == (addr & POGLS_DRAM_KV_FLAG)) {
            return de;
        }
    }
    return NULL;
}

static DirEntry* find_by_name(PoglsDramStore *store, const char *name) {
    for (uint32_t i = 0; i < store->max_entries; i++) {
        DirEntry *de = &DIR_ENTRIES(store)[i];
        if (de->dram_addr != 0 &&
            strncmp(de->name, name, 64) == 0) {
            return de;
        }
    }
    return NULL;
}

static DirEntry* find_free_slot(PoglsDramStore *store) {
    for (uint32_t i = 0; i < store->max_entries; i++) {
        if (DIR_ENTRIES(store)[i].dram_addr == 0)
            return &DIR_ENTRIES(store)[i];
    }
    return NULL;
}

int pogls_dram_open(PoglsDramStore *store, const char *path, size_t capacity) {
    if (!store || capacity == 0) return -1;
    memset(store, 0, sizeof(*store));

    uint32_t max_entries = POGLS_DRAM_MAX_DIR;
    size_t total = store_total_size(max_entries, capacity);

    uint8_t *base = (uint8_t*)pogls_alloc_large(total);
    if (!base) return -1;
    memset(base, 0, total);

    store->base         = base;
    store->capacity     = capacity;
    store->used         = 0;
    store->max_entries  = max_entries;
    store->n_entries    = 0;
    store->session_tick = 1;
    store->entries      = (DirEntry*)(base + POGLS_DRAM_HDR_SZ);
    store->arena        = base + POGLS_DRAM_HDR_SZ + dir_bytes(max_entries);

    if (path)
        strncpy(store->filepath, path, POGLS_DRAM_PATH_MAX - 1);

    return 0;
}

void pogls_dram_close(PoglsDramStore *store) {
    if (!store || !store->base) return;
    size_t total = store_total_size(store->max_entries, store->capacity);
    pogls_free_large(store->base, total);
    memset(store, 0, sizeof(*store));
}

int pogls_dram_save(PoglsDramStore *store, const char *path, int is_kv) {
    if (!store || !path) return -1;

    FILE *f = pogls_fopen(path, "wb");
    if (!f) return -1;

    uint32_t save_count = 0;
    for (uint32_t i = 0; i < store->max_entries; i++) {
        DirEntry *e = &DIR_ENTRIES(store)[i];
        if (e->dram_addr == 0) continue;
        if (e->dram_addr & POGLS_DRAM_KV_FLAG && !is_kv) continue;
        save_count++;
    }

    uint32_t magic    = POGLS_DRAM_MAGIC;
    uint32_t version  = POGLS_DRAM_VERSION;
    uint64_t used     = store->used;

    fwrite(&magic,   4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&save_count, 4, 1, f);
    fwrite(&used,    8, 1, f);

    for (uint32_t i = 0; i < store->max_entries; i++) {
        DirEntry *e = &DIR_ENTRIES(store)[i];
        if (e->dram_addr == 0) continue;
        if (e->dram_addr & POGLS_DRAM_KV_FLAG && !is_kv) continue;
        fwrite(e->name,       64, 1, f);
        fwrite(&e->dram_addr, 4,  1, f);
        fwrite(&e->nbytes,    4,  1, f);
        fwrite(&e->flags,     1,  1, f);
        uint8_t zero[3] = {0, 0, 0};
        fwrite(zero, 3, 1, f);
    }

    if (used > 0)
        fwrite(store->arena, 1, (size_t)used, f);

    fclose(f);
    return 0;
}

int pogls_dram_put(PoglsDramStore *store, const char *name, uint32_t addr,
                   const void *data, size_t sz)
{
    if (!store || !store->base || !data || sz == 0) return -1;

    DirEntry *existing = find_by_addr(store, addr);
    if (existing) {
        if (existing->nbytes < sz) return -1;
        memcpy(store->arena + existing->offset, data, sz);
        existing->nbytes = (uint32_t)sz;
        existing->session_tick = store->session_tick++;
        return 0;
    }

    DirEntry *slot = find_free_slot(store);
    if (!slot) return -1;

    if (store->used + sz > store->capacity) return -1;

    memset(slot, 0, sizeof(*slot));
    strncpy(slot->name, name ? name : "", 64);
    slot->name[63] = '\0';
    slot->dram_addr    = addr;
    slot->nbytes       = (uint32_t)sz;
    slot->offset       = (uint32_t)store->used;
    slot->session_tick = store->session_tick++;

    memcpy(store->arena + store->used, data, sz);
    store->used += sz;
    store->n_entries++;
    return 0;
}

void* pogls_dram_get(PoglsDramStore *store, uint32_t addr, size_t *sz_out) {
    if (!store || !store->base) return NULL;
    DirEntry *e = find_by_addr(store, addr);
    if (!e) return NULL;
    if (sz_out) *sz_out = e->nbytes;
    return store->arena + e->offset;
}

void* pogls_dram_get_name(PoglsDramStore *store, const char *name, size_t *sz_out) {
    if (!store || !store->base || !name) return NULL;
    DirEntry *e = find_by_name(store, name);
    if (!e) return NULL;
    if (sz_out) *sz_out = e->nbytes;
    return store->arena + e->offset;
}

int pogls_dram_free(PoglsDramStore *store, uint32_t addr) {
    if (!store || !store->base) return -1;
    DirEntry *e = find_by_addr(store, addr);
    if (!e) return -1;
    e->dram_addr = 0;
    e->nbytes    = 0;
    e->name[0]   = '\0';
    store->n_entries--;
    return 0;
}

int pogls_dram_has(PoglsDramStore *store, uint32_t addr) {
    if (!store || !store->base) return 0;
    return find_by_addr(store, addr) != NULL;
}

uint32_t pogls_dram_count(PoglsDramStore *store) {
    if (!store) return 0;
    return store->n_entries;
}

uint64_t pogls_dram_bytes(PoglsDramStore *store) {
    if (!store) return 0;
    return store->used;
}

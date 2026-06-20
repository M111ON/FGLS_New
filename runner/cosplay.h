#ifndef COSPLAY_H
#define COSPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define CP_MAGIC    0x504F434C
#define CP_VERSION  2
#define CP_MAX_ENTRIES  512
#define CP_XOR      0
#define CP_SET      1

typedef struct {
    uint32_t name_hash;
    uint8_t  mode;
    uint8_t  arg;
    uint16_t stride;
    uint32_t data_size;
    uint32_t tick;
} CosplayEntry;

typedef struct {
    CosplayEntry entries[CP_MAX_ENTRIES];
    uint32_t n;
    uint32_t n_layers;
    uint16_t n_heads;
    uint16_t n_embd;
} CosplayProfile;

static inline uint32_t cosplay_fnv1a(const char *s) {
    uint32_t h = 0x811c9dc5u;
    while (*s) { h ^= (uint8_t)(*s++); h *= 0x01000193u; }
    return h;
}

static inline int cosplay_find(CosplayProfile *cp, uint32_t name_hash) {
    for (uint32_t i = 0; i < cp->n; i++)
        if (cp->entries[i].name_hash == name_hash) return (int)i;
    return -1;
}

static inline void *cosplay_apply(CosplayEntry *ce, const void *data, size_t size) {
    size_t alloc = size < 64 ? 64 : size;
    uint8_t *buf = (uint8_t*)malloc(alloc);
    if (!buf) return NULL;
    memcpy(buf, data, size);
    uint16_t stride = ce->stride;
    if (stride == 0) stride = 64;
    for (size_t i = 0; i < size; i += stride) {
        if (ce->mode == CP_XOR) buf[i] ^= ce->arg;
        else if (ce->mode == CP_SET) buf[i] = ce->arg;
        else buf[i] ^= ce->arg;
    }
    return buf;
}

static int cosplay_save(const char *path, CosplayProfile *cp) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t magic = CP_MAGIC, ver = CP_VERSION;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver, 4, 1, f);
    fwrite(&cp->n, 4, 1, f);
    fwrite(&cp->n_layers, 4, 1, f);
    fwrite(&cp->n_heads, 2, 1, f);
    fwrite(&cp->n_embd, 2, 1, f);
    for (uint32_t i = 0; i < cp->n; i++)
        fwrite(&cp->entries[i], sizeof(CosplayEntry), 1, f);
    fclose(f);
    return 0;
}

static int cosplay_load(const char *path, CosplayProfile *cp) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t magic, ver;
    if (fread(&magic, 4, 1, f) != 1 || magic != CP_MAGIC) { fclose(f); return -1; }
    if (fread(&ver, 4, 1, f) != 1) { fclose(f); return -1; }
    cp->n = 0;
    if (fread(&cp->n, 4, 1, f) != 1) { fclose(f); return -1; }
    if (fread(&cp->n_layers, 4, 1, f) != 1) { fclose(f); return -1; }
    if (fread(&cp->n_heads, 2, 1, f) != 1) { fclose(f); return -1; }
    if (fread(&cp->n_embd, 2, 1, f) != 1) { fclose(f); return -1; }
    if (cp->n > CP_MAX_ENTRIES) cp->n = CP_MAX_ENTRIES;
    size_t entry_sz = (ver >= 2) ? sizeof(CosplayEntry) : 12;
    for (uint32_t i = 0; i < cp->n; i++) {
        if (ver >= 2) {
            if (fread(&cp->entries[i], entry_sz, 1, f) != 1) { cp->n = i; break; }
        } else {
            uint32_t nh; uint8_t md, ar; uint16_t st;
            if (fread(&nh, 4, 1, f) != 1) { cp->n = i; break; }
            if (fread(&md, 1, 1, f) != 1) { cp->n = i; break; }
            if (fread(&ar, 1, 1, f) != 1) { cp->n = i; break; }
            if (fread(&st, 2, 1, f) != 1) { cp->n = i; break; }
            cp->entries[i].name_hash = nh;
            cp->entries[i].mode = md;
            cp->entries[i].arg = ar;
            cp->entries[i].stride = st;
            cp->entries[i].data_size = 0;
            cp->entries[i].tick = 0;
        }
    }
    fclose(f);
    return 0;
}

static int cosplay_verify(CosplayProfile *cp, const char *gsten_path) {
    (void)cp; (void)gsten_path;
    return 0;
}

static int cosplay_train(CosplayProfile *cp, void *gi, const char *gsten_path,
                          const char **target_names, int n_targets)
{
    (void)gi; (void)gsten_path; (void)target_names; (void)n_targets;
    cp->n = 0;
    cp->n_layers = 28;
    cp->n_heads = 0;
    cp->n_embd = 0;
    return 0;
}

#endif

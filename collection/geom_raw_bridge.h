#ifndef GEOM_RAW_BRIDGE_H
#define GEOM_RAW_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RB_OK    0
#define RB_ERR  -1

#define RB_HASH_SIZE 4096
#define RB_MAX_ENTRIES 2048
#define RB_NAME_MAX 256

/* ── Raw .qdat bridge (original) ── */
typedef struct {
    char    name[RB_NAME_MAX];
    void   *data;
    size_t  size;
    int     dtype;       /* GGML quantization type: 0=F32, 8=Q8_0, etc */
    int     occupied;
} RBEntry;

typedef struct {
    RBEntry  entries[RB_MAX_ENTRIES];
    uint32_t n_entries;
} RawBridge;

int  rb_load(RawBridge *rb, const char *dir_path);
int  rb_get(RawBridge *rb, const char *name, void **data, size_t *size);
void rb_free(RawBridge *rb);

/* ── Geometry tile store (.gsten) ──
 *
 * Format:
 *   Header (16B): magic(4B) + n_tiles(4B) + tile_sz(1B) + reserved(7B)
 *   Index (6B/tile): {tile_offset(4B) + enc_size(1B) + type(1B)}
 *   Data: encoded tiles (variable)
 *
 * Decode: given tile_index → index lookup (O(1)) → hex_tile_decode → 7 bytes
 */

#define GSTEN_MAGIC      0x4747454F  /* 'GEOM' */
#define GSTEN_NAME_MAX   RB_NAME_MAX
#define GSTEN_TILE_SZ    7

typedef struct {
    uint32_t tile_offset;   /* byte offset in data blob */
    uint8_t  enc_size;      /* encoded size (2 or 9) */
    uint8_t  type;          /* HENC_FLAT / TRIPLET / GRADIENT / EDGE */
} __attribute__((packed)) GstenIndexEntry;

typedef struct {
    char             name[GSTEN_NAME_MAX];
    uint32_t         n_tiles;
    uint8_t          tile_sz;
    GstenIndexEntry *index;         /* [n_tiles] */
    uint8_t         *data;          /* encoded tile data */
    size_t           data_size;
    uint8_t         *store_blob;    /* full file mmap (free this) */
    size_t           store_size;
    int              occupied;
} GstenEntry;

typedef struct {
    GstenEntry entries[RB_MAX_ENTRIES];
    uint32_t   n_entries;
} GeomBridge;

int  gb_load(GeomBridge *gb, const char *dir_path);
int  gb_get(GeomBridge *gb, const char *name, GstenEntry **out);
int  gb_decode_tile(GstenEntry *entry, uint32_t tile_idx, uint8_t out[7]);
int  gb_decode_tensor(GstenEntry *entry, uint8_t *out, size_t out_sz);
void gb_free(GeomBridge *gb);

#ifdef __cplusplus
}
#endif

#ifdef GEOM_RAW_BRIDGE_IMPLEMENTATION

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#else
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* — hex_tile decode inline (minimal, matches hex_tile.h exactly) — */

#define HENC_FLAT         0x00
#define HENC_TRIPLET_FLAT 0x01
#define HENC_GRADIENT     0x02
#define HENC_EDGE         0x03
#define HENC_HEX_CELLS    7

static inline int _gb_hex_decode(const uint8_t *src, size_t src_len, uint8_t *out) {
    if (src_len < 2) return 0;
    if (src[0] == HENC_FLAT) {
        for (int i = 0; i < HENC_HEX_CELLS; i++) out[i] = src[1];
        return 2;
    }
    if (src_len < (size_t)(2 + HENC_HEX_CELLS)) return 0;
    uint8_t pred = src[1];
    for (int i = 0; i < HENC_HEX_CELLS; i++)
        out[i] = (uint8_t)((src[2 + i] - 128 + pred) & 0xFF);
    return 2 + HENC_HEX_CELLS;
}

/* ── RawBridge (unchanged) ── */

static uint32_t _rb_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) ^ (uint8_t)*s++;
    return h;
}

int rb_load(RawBridge *rb, const char *dir_path) {
    if (!rb || !dir_path) return RB_ERR;
    memset(rb, 0, sizeof(*rb));

#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*.qdat", dir_path);

    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(pattern, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return RB_ERR;

    do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s\\%s", dir_path, ffd.cFileName);

        FILE *f = fopen(full_path, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);

        void *buf = malloc(sz > 0 ? (size_t)sz : 1);
        if (!buf) { fclose(f); continue; }
        size_t got = fread(buf, 1, sz > 0 ? (size_t)sz : 0, f);
        fclose(f);

        if (got == 0 && sz > 0) { free(buf); continue; }

        char name[RB_NAME_MAX];
        const char *dot = strstr(ffd.cFileName, ".qdat");
        size_t nlen = dot ? (size_t)(dot - ffd.cFileName) : strlen(ffd.cFileName);
        if (nlen >= RB_NAME_MAX) nlen = RB_NAME_MAX - 1;
        memcpy(name, ffd.cFileName, nlen);
        name[nlen] = 0;

        uint32_t idx = _rb_hash(name) % RB_MAX_ENTRIES;
        while (rb->entries[idx].occupied) idx = (idx + 1) % RB_MAX_ENTRIES;

        strncpy(rb->entries[idx].name, name, RB_NAME_MAX - 1);
        rb->entries[idx].name[RB_NAME_MAX - 1] = 0;
        rb->entries[idx].data = buf;
        rb->entries[idx].size = got;
        rb->entries[idx].occupied = 1;
        rb->entries[idx].dtype = 8; /* default Q8_0 */

        /* Try loading .qtype sidecar */
        char qtype_path[1024];
        snprintf(qtype_path, sizeof(qtype_path), "%s\\%s.qtype", dir_path, ffd.cFileName);
        /* strip .qdat from cFileName */
        char *dot_qdat = strstr(qtype_path, ".qdat");
        if (dot_qdat) {
            memcpy(dot_qdat, ".qtype", 6);
            dot_qdat[6] = 0;
        }
        FILE *qtf = fopen(qtype_path, "rb");
        if (qtf) {
            int dtype_byte = fgetc(qtf);
            if (dtype_byte != EOF) rb->entries[idx].dtype = dtype_byte;
            fclose(qtf);
        }

        rb->n_entries++;
    } while (FindNextFileA(hFind, &ffd) != 0);

    FindClose(hFind);
#else
    DIR *d = opendir(dir_path);
    if (!d) return RB_ERR;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *ext = strstr(entry->d_name, ".qdat");
        if (!ext) continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        FILE *f = fopen(full_path, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);

        void *buf = malloc(sz > 0 ? (size_t)sz : 1);
        if (!buf) { fclose(f); continue; }
        size_t got = fread(buf, 1, sz > 0 ? (size_t)sz : 0, f);
        fclose(f);

        if (got == 0 && sz > 0) { free(buf); continue; }

        char name[RB_NAME_MAX];
        size_t nlen = (size_t)(ext - entry->d_name);
        if (nlen >= RB_NAME_MAX) nlen = RB_NAME_MAX - 1;
        memcpy(name, entry->d_name, nlen);
        name[nlen] = 0;

        uint32_t idx = _rb_hash(name) % RB_MAX_ENTRIES;
        while (rb->entries[idx].occupied) idx = (idx + 1) % RB_MAX_ENTRIES;

        strncpy(rb->entries[idx].name, name, RB_NAME_MAX - 1);
        rb->entries[idx].name[RB_NAME_MAX - 1] = 0;
        rb->entries[idx].data = buf;
        rb->entries[idx].size = got;
        rb->entries[idx].occupied = 1;
        rb->entries[idx].dtype = 8; /* default Q8_0 */

        /* Try loading .qtype sidecar */
        char qtype_path[1024];
        snprintf(qtype_path, sizeof(qtype_path), "%s/%s.qtype", dir_path, entry->d_name);
        char *dot_qdat = strstr(qtype_path, ".qdat");
        if (dot_qdat) {
            memcpy(dot_qdat, ".qtype", 6);
            dot_qdat[6] = 0;
        }
        FILE *qtf = fopen(qtype_path, "rb");
        if (qtf) {
            int dtype_byte = fgetc(qtf);
            if (dtype_byte != EOF) rb->entries[idx].dtype = dtype_byte;
            fclose(qtf);
        }

        rb->n_entries++;
    }
    closedir(d);
#endif

    return rb->n_entries > 0 ? RB_OK : RB_ERR;
}

int rb_get(RawBridge *rb, const char *name, void **data, size_t *size) {
    if (!rb || !name || !data || !size) return RB_ERR;

    uint32_t idx = _rb_hash(name) % RB_MAX_ENTRIES;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        uint32_t probe = (idx + i) % RB_MAX_ENTRIES;
        if (!rb->entries[probe].occupied) return RB_ERR;
        if (strcmp(rb->entries[probe].name, name) == 0) {
            *data = rb->entries[probe].data;
            *size = rb->entries[probe].size;
            return RB_OK;
        }
    }
    return RB_ERR;
}

void rb_free(RawBridge *rb) {
    if (!rb) return;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (rb->entries[i].occupied) {
            free(rb->entries[i].data);
            rb->entries[i].occupied = 0;
        }
    }
    rb->n_entries = 0;
}

/* ── GeomBridge ── */

static uint32_t _gb_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) ^ (uint8_t)*s++;
    return h;
}

int gb_load(GeomBridge *gb, const char *dir_path) {
    if (!gb || !dir_path) return RB_ERR;
    memset(gb, 0, sizeof(*gb));

#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*.gsten", dir_path);

    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(pattern, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return RB_ERR;

    do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s\\%s", dir_path, ffd.cFileName);

        FILE *f = fopen(full_path, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (sz < 16) { fclose(f); continue; } /* too small for header */

        uint8_t *blob = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
        if (!blob) { fclose(f); continue; }
        fread(blob, 1, sz > 0 ? (size_t)sz : 0, f);
        fclose(f);

        /* Parse header */
        uint32_t magic, n_tiles;
        uint8_t  tile_sz;
        memcpy(&magic,   blob,     4);
        memcpy(&n_tiles, blob + 4, 4);
        tile_sz = blob[8];

        if (magic != GSTEN_MAGIC || n_tiles == 0 || tile_sz != GSTEN_TILE_SZ) {
            free(blob);
            continue;
        }

        /* Index starts at offset 16 */
        GstenIndexEntry *idx = (GstenIndexEntry *)(blob + 16);

        /* Data starts after header(16) + index(n_tiles × 6) */
        size_t idx_bytes = (size_t)n_tiles * sizeof(GstenIndexEntry);
        uint8_t *data    = blob + 16 + idx_bytes;
        size_t data_size = (size_t)sz - 16 - idx_bytes;

        /* Name: strip .gsten suffix */
        char name[GSTEN_NAME_MAX];
        const char *dot = strstr(ffd.cFileName, ".gsten");
        size_t nlen = dot ? (size_t)(dot - ffd.cFileName) : strlen(ffd.cFileName);
        if (nlen >= GSTEN_NAME_MAX) nlen = GSTEN_NAME_MAX - 1;
        memcpy(name, ffd.cFileName, nlen);
        name[nlen] = 0;

        uint32_t hidx = _gb_hash(name) % RB_MAX_ENTRIES;
        while (gb->entries[hidx].occupied) hidx = (hidx + 1) % RB_MAX_ENTRIES;

        strncpy(gb->entries[hidx].name, name, GSTEN_NAME_MAX - 1);
        gb->entries[hidx].name[GSTEN_NAME_MAX - 1] = 0;
        gb->entries[hidx].n_tiles    = n_tiles;
        gb->entries[hidx].tile_sz    = tile_sz;
        gb->entries[hidx].index      = idx;
        gb->entries[hidx].data       = data;
        gb->entries[hidx].data_size  = data_size;
        gb->entries[hidx].store_blob = blob;
        gb->entries[hidx].store_size = (size_t)sz;
        gb->entries[hidx].occupied   = 1;
        gb->n_entries++;
    } while (FindNextFileA(hFind, &ffd) != 0);

    FindClose(hFind);
#else
    /* Unix: use opendir/readdir */
    DIR *d = opendir(dir_path);
    if (!d) return RB_ERR;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *ext = strstr(entry->d_name, ".gsten");
        if (!ext) continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        FILE *f = fopen(full_path, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (sz < 16) { fclose(f); continue; }

        uint8_t *blob = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
        if (!blob) { fclose(f); continue; }
        fread(blob, 1, sz > 0 ? (size_t)sz : 0, f);
        fclose(f);

        uint32_t magic, n_tiles;
        uint8_t  tile_sz;
        memcpy(&magic,   blob,     4);
        memcpy(&n_tiles, blob + 4, 4);
        tile_sz = blob[8];

        if (magic != GSTEN_MAGIC || n_tiles == 0 || tile_sz != GSTEN_TILE_SZ) {
            free(blob);
            continue;
        }

        GstenIndexEntry *idx = (GstenIndexEntry *)(blob + 16);
        size_t idx_bytes = (size_t)n_tiles * sizeof(GstenIndexEntry);
        uint8_t *data    = blob + 16 + idx_bytes;
        size_t data_size = (size_t)sz - 16 - idx_bytes;

        char name[GSTEN_NAME_MAX];
        size_t nlen = (size_t)(ext - entry->d_name);
        if (nlen >= GSTEN_NAME_MAX) nlen = GSTEN_NAME_MAX - 1;
        memcpy(name, entry->d_name, nlen);
        name[nlen] = 0;

        uint32_t hidx = _gb_hash(name) % RB_MAX_ENTRIES;
        while (gb->entries[hidx].occupied) hidx = (hidx + 1) % RB_MAX_ENTRIES;

        strncpy(gb->entries[hidx].name, name, GSTEN_NAME_MAX - 1);
        gb->entries[hidx].name[GSTEN_NAME_MAX - 1] = 0;
        gb->entries[hidx].n_tiles    = n_tiles;
        gb->entries[hidx].tile_sz    = tile_sz;
        gb->entries[hidx].index      = idx;
        gb->entries[hidx].data       = data;
        gb->entries[hidx].data_size  = data_size;
        gb->entries[hidx].store_blob = blob;
        gb->entries[hidx].store_size = (size_t)sz;
        gb->entries[hidx].occupied   = 1;
        gb->n_entries++;
    }
    closedir(d);
#endif

    return gb->n_entries > 0 ? RB_OK : RB_ERR;
}

int gb_get(GeomBridge *gb, const char *name, GstenEntry **out) {
    if (!gb || !name || !out) return RB_ERR;

    uint32_t idx = _gb_hash(name) % RB_MAX_ENTRIES;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        uint32_t probe = (idx + i) % RB_MAX_ENTRIES;
        if (!gb->entries[probe].occupied) return RB_ERR;
        if (strcmp(gb->entries[probe].name, name) == 0) {
            *out = &gb->entries[probe];
            return RB_OK;
        }
    }
    return RB_ERR;
}

int gb_decode_tile(GstenEntry *entry, uint32_t tile_idx, uint8_t out[7]) {
    if (!entry || tile_idx >= entry->n_tiles) return RB_ERR;

    GstenIndexEntry *ie = &entry->index[tile_idx];
    if (ie->enc_size < 2 || ie->enc_size > 9) return RB_ERR;

    uint32_t offset = ie->tile_offset;
    if (offset + ie->enc_size > entry->data_size) return RB_ERR;

    int n = _gb_hex_decode(entry->data + offset, ie->enc_size, out);
    return (n > 0) ? RB_OK : RB_ERR;
}

int gb_decode_tensor(GstenEntry *entry, uint8_t *out, size_t out_sz) {
    if (!entry || !out) return RB_ERR;

    /* Fully decode: tiles × 7 bytes */
    size_t expected = (size_t)entry->n_tiles * GSTEN_TILE_SZ;
    if (out_sz < expected) return RB_ERR;

    static uint8_t tile_buf[GSTEN_TILE_SZ];

    for (uint32_t ti = 0; ti < entry->n_tiles; ti++) {
        if (gb_decode_tile(entry, ti, tile_buf) != RB_OK) return RB_ERR;
        memcpy(out + (size_t)ti * GSTEN_TILE_SZ, tile_buf, GSTEN_TILE_SZ);
    }
    return RB_OK;
}

void gb_free(GeomBridge *gb) {
    if (!gb) return;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (gb->entries[i].occupied) {
            free(gb->entries[i].store_blob);
            gb->entries[i].occupied = 0;
        }
    }
    gb->n_entries = 0;
}

#endif /* GEOM_RAW_BRIDGE_IMPLEMENTATION */
#endif /* GEOM_RAW_BRIDGE_H */

/*
 * geometry_store_reader.h
 * =======================
 *
 * Read-only runtime for .gsidx/.gsdat geometry stores.
 *
 * Properties:
 *   - O(1) lookup by composite geometry key (zone + namespace shift + shape)
 *   - zero-copy access to mapped float32 rows
 *   - tiny RAM footprint: only the dense slot table lives in memory
 */

#ifndef GEOMETRY_STORE_READER_H
#define GEOMETRY_STORE_READER_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define GSIDX_MAGIC        "GSIDX001"
#define GSIDX_HEADER_BYTES 32u
#define GS_SHAPE_COUNT     6u
#define GS_ZONE_CAP        96u  /* 12 base zones × 8 namespace bands */

static const char GS_SHAPES[GS_SHAPE_COUNT] = { 'I', 'O', 'T', 'S', 'Z', 'L' };

typedef struct {
    uint8_t  valid;
    uint16_t zone;
    uint16_t shape_idx;
    uint64_t offset_bytes;
    uint32_t n_rows;
    uint32_t n_cols;
} GeometryStoreSlot;

typedef struct {
    const float *ptr;
    uint32_t     n_rows;
    uint32_t     n_cols;
} GeometryStoreView;

typedef struct {
    char            *idx_path;
    char            *dat_path;
    GeometryStoreSlot slots[GS_ZONE_CAP][GS_SHAPE_COUNT];
    uint32_t         n_entries;
    uint64_t         data_size;
    uint8_t         *data_base;
    size_t           data_len;
#ifdef _WIN32
    HANDLE           dat_file;
    HANDLE           dat_map;
#else
    int              dat_fd;
#endif
} GeometryStoreReader;

static inline void geometry_store_reader_init(GeometryStoreReader *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
#ifdef _WIN32
    st->dat_file = INVALID_HANDLE_VALUE;
    st->dat_map = NULL;
#else
    st->dat_fd = -1;
#endif
}

static inline void geometry_store_reader_free_paths(GeometryStoreReader *st)
{
    if (!st) return;
    free(st->idx_path);
    free(st->dat_path);
    st->idx_path = NULL;
    st->dat_path = NULL;
}

static inline int geometry_store_shape_index(char shape)
{
    for (uint32_t i = 0; i < GS_SHAPE_COUNT; ++i) {
        if (GS_SHAPES[i] == shape) return (int)i;
    }
    return -1;
}

static inline int geometry_store_ns_shift(char ns)
{
    switch (ns) {
    case '\0': return 0;
    case 'Q': return 12;
    case 'K': return 24;
    case 'V': return 36;
    case 'O': return 48;
    case 'G': return 60;
    case 'U': return 72;
    case 'D': return 84;
    default: return -1;
    }
}

static inline int geometry_store_key_zone(uint16_t zone, const char *ns)
{
    int shift = geometry_store_ns_shift((ns && ns[0]) ? ns[0] : '\0');
    if (shift < 0) return -1;
    if (zone >= 12u) return -1;
    return (int)zone + shift;
}

static inline char *geometry_store_make_path(const char *base, const char *suffix)
{
    size_t blen = strlen(base);
    size_t slen = strlen(suffix);
    char *out = (char *)malloc(blen + slen + 1u);
    if (!out) return NULL;
    memcpy(out, base, blen);
    memcpy(out + blen, suffix, slen + 1u);
    return out;
}

static inline int geometry_store_map_data(GeometryStoreReader *st)
{
    if (!st || !st->dat_path) return -1;

#ifdef _WIN32
    st->dat_file = CreateFileA(st->dat_path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (st->dat_file == INVALID_HANDLE_VALUE) return -1;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(st->dat_file, &sz) || sz.QuadPart <= 0) {
        CloseHandle(st->dat_file);
        st->dat_file = INVALID_HANDLE_VALUE;
        return -1;
    }

    st->dat_map = CreateFileMappingA(st->dat_file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!st->dat_map) {
        CloseHandle(st->dat_file);
        st->dat_file = INVALID_HANDLE_VALUE;
        return -1;
    }

    st->data_base = (uint8_t *)MapViewOfFile(st->dat_map, FILE_MAP_READ, 0, 0, 0);
    if (!st->data_base) {
        CloseHandle(st->dat_map);
        CloseHandle(st->dat_file);
        st->dat_map = NULL;
        st->dat_file = INVALID_HANDLE_VALUE;
        return -1;
    }

    st->data_size = (uint64_t)sz.QuadPart;
    st->data_len = (size_t)sz.QuadPart;
    return 0;
#else
    st->dat_fd = open(st->dat_path, O_RDONLY);
    if (st->dat_fd < 0) return -1;

    struct stat sb;
    if (fstat(st->dat_fd, &sb) != 0 || sb.st_size <= 0) {
        close(st->dat_fd);
        st->dat_fd = -1;
        return -1;
    }

    st->data_len = (size_t)sb.st_size;
    st->data_size = (uint64_t)sb.st_size;
    st->data_base = (uint8_t *)mmap(NULL, st->data_len, PROT_READ, MAP_PRIVATE, st->dat_fd, 0);
    if (st->data_base == MAP_FAILED) {
        st->data_base = NULL;
        close(st->dat_fd);
        st->dat_fd = -1;
        return -1;
    }
    return 0;
#endif
}

static inline void geometry_store_close(GeometryStoreReader *st)
{
    if (!st) return;

#ifdef _WIN32
    if (st->data_base) {
        UnmapViewOfFile(st->data_base);
        st->data_base = NULL;
    }
    if (st->dat_map) {
        CloseHandle(st->dat_map);
        st->dat_map = NULL;
    }
    if (st->dat_file && st->dat_file != INVALID_HANDLE_VALUE) {
        CloseHandle(st->dat_file);
        st->dat_file = INVALID_HANDLE_VALUE;
    }
#else
    if (st->data_base) {
        munmap(st->data_base, st->data_len);
        st->data_base = NULL;
    }
    if (st->dat_fd >= 0) {
        close(st->dat_fd);
        st->dat_fd = -1;
    }
#endif

    geometry_store_reader_free_paths(st);
    memset(st->slots, 0, sizeof(st->slots));
    st->n_entries = 0;
    st->data_size = 0;
    st->data_len = 0;
}

static inline int geometry_store_load_index(GeometryStoreReader *st)
{
    FILE *f = fopen(st->idx_path, "rb");
    if (!f) return -1;

    unsigned char magic[8];
    uint32_t n_entries = 0;
    uint64_t data_size = 0;
    unsigned char pad[12];

    if (fread(magic, 1, 8, f) != 8) { fclose(f); return -1; }
    if (memcmp(magic, GSIDX_MAGIC, 8) != 0) { fclose(f); return -1; }
    if (fread(&n_entries, sizeof(n_entries), 1, f) != 1) { fclose(f); return -1; }
    if (fread(&data_size, sizeof(data_size), 1, f) != 1) { fclose(f); return -1; }
    if (fread(pad, 1, 12, f) != 12) { fclose(f); return -1; }

    memset(st->slots, 0, sizeof(st->slots));

    for (uint32_t i = 0; i < n_entries; ++i) {
        uint16_t zone = 0;
        uint16_t shape_idx = 0;
        int64_t offset = 0;
        uint32_t n_rows = 0;
        uint32_t n_cols = 0;
        if (fread(&zone, sizeof(zone), 1, f) != 1) { fclose(f); return -1; }
        if (fread(&shape_idx, sizeof(shape_idx), 1, f) != 1) { fclose(f); return -1; }
        if (fread(&offset, sizeof(offset), 1, f) != 1) { fclose(f); return -1; }
        if (fread(&n_rows, sizeof(n_rows), 1, f) != 1) { fclose(f); return -1; }
        if (fread(&n_cols, sizeof(n_cols), 1, f) != 1) { fclose(f); return -1; }

        if (zone >= GS_ZONE_CAP || shape_idx >= GS_SHAPE_COUNT || offset < 0) {
            fclose(f);
            return -1;
        }

        GeometryStoreSlot *slot = &st->slots[zone][shape_idx];
        if (slot->valid) {
            fclose(f);
            return -1;
        }

        slot->valid = 1;
        slot->zone = zone;
        slot->shape_idx = shape_idx;
        slot->offset_bytes = (uint64_t)offset;
        slot->n_rows = n_rows;
        slot->n_cols = n_cols;
    }

    fclose(f);
    st->n_entries = n_entries;
    st->data_size = data_size;
    if (st->data_size != st->data_len) return -1;
    return 0;
}

static inline int geometry_store_open(GeometryStoreReader *st, const char *base_path)
{
    if (!st || !base_path || !*base_path) return -1;
    geometry_store_reader_init(st);

    st->idx_path = geometry_store_make_path(base_path, ".gsidx");
    st->dat_path = geometry_store_make_path(base_path, ".gsdat");
    if (!st->idx_path || !st->dat_path) {
        geometry_store_close(st);
        return -1;
    }

    if (geometry_store_map_data(st) != 0) {
        geometry_store_close(st);
        return -1;
    }

    if (geometry_store_load_index(st) != 0) {
        geometry_store_close(st);
        return -1;
    }

    return 0;
}

static inline const GeometryStoreSlot *geometry_store_lookup_slot(
    const GeometryStoreReader *st, uint16_t zone, char shape, const char *ns)
{
    int key_zone = geometry_store_key_zone(zone, ns);
    int shape_idx = geometry_store_shape_index(shape);
    if (!st || key_zone < 0 || shape_idx < 0 || key_zone >= (int)GS_ZONE_CAP) return NULL;
    const GeometryStoreSlot *slot = &st->slots[key_zone][shape_idx];
    return slot->valid ? slot : NULL;
}

static inline int geometry_store_query(const GeometryStoreReader *st,
                                       uint16_t zone,
                                       char shape,
                                       const char *ns,
                                       GeometryStoreView *out)
{
    if (!st || !out) return -1;
    if (!st->data_base) return -1;
    const GeometryStoreSlot *slot = geometry_store_lookup_slot(st, zone, shape, ns);
    if (!slot) return 1;

    uint64_t bytes = (uint64_t)slot->n_rows * (uint64_t)slot->n_cols * sizeof(float);
    if (slot->offset_bytes + bytes > st->data_size) return -1;

    out->ptr = (const float *)(const void *)(st->data_base + slot->offset_bytes);
    out->n_rows = slot->n_rows;
    out->n_cols = slot->n_cols;
    return 0;
}

static inline int geometry_store_query_key(const GeometryStoreReader *st,
                                           uint16_t key_zone,
                                           char shape,
                                           GeometryStoreView *out)
{
    if (!st || !out) return -1;
    if (!st->data_base) return -1;
    int shape_idx = geometry_store_shape_index(shape);
    if (shape_idx < 0 || key_zone >= GS_ZONE_CAP) return -1;

    const GeometryStoreSlot *slot = &st->slots[key_zone][shape_idx];
    if (!slot->valid) return 1;

    uint64_t bytes = (uint64_t)slot->n_rows * (uint64_t)slot->n_cols * sizeof(float);
    if (slot->offset_bytes + bytes > st->data_size) return -1;

    out->ptr = (const float *)(const void *)(st->data_base + slot->offset_bytes);
    out->n_rows = slot->n_rows;
    out->n_cols = slot->n_cols;
    return 0;
}

static inline uint32_t geometry_store_count(const GeometryStoreReader *st)
{
    return st ? st->n_entries : 0;
}

static inline uint32_t geometry_store_row_floats(const GeometryStoreView *view)
{
    return view ? view->n_rows * view->n_cols : 0;
}

#endif /* GEOMETRY_STORE_READER_H */

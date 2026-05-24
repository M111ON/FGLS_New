/*
 * geo_store_reader.h — Single-header C reader for .gsidx/.gsdat
 *
 * Reads the GeometryStore format produced by geometry_store.py:
 *
 *   .gsidx — fixed-size index:
 *     HEADER:  magic(8)="GSIDX001" + n_entries(4LE) + data_size(8LE) + pad(12) = 32B
 *     ENTRIES: raw_zone(2LE) + shape_idx(2LE) + offset(8LE) + n_rows(4LE) + n_cols(4LE)
 *              = 20B each
 *
 *   .gsdat — raw float32 rows at offsets declared in index
 *
 * Namespace encoding via raw_zone shift:
 *   base 0-11, Q:+12, K:+24, V:+36, O:+48, G:+60, U:+72, D:+84
 *
 * Usage:
 *   GeoStore gs;
 *   geo_store_open(&gs, "path/to/store");
 *   GeoEntry e;
 *   if (geo_store_query(&gs, 'K', 0, 'I', &e) == 0)
 *       for (uint32_t i = 0; i < e.n_rows * e.n_cols; i++)
 *           printf("%f\n", e.data[i]);
 *   geo_store_close(&gs);
 */
#ifndef GEO_STORE_READER_H
#define GEO_STORE_READER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GEO_STORE_MAGIC      "GSIDX001"
#define GEO_STORE_MAGIC_LEN  8
#define GEO_STORE_HEADER_SZ  32
#define GEO_STORE_ENTRY_SZ   20

#define GEO_STORE_SHAPES     "IOTSZL"
#define GEO_STORE_NS_SHIFT   12
#define GEO_NS_Q  12
#define GEO_NS_K  24
#define GEO_NS_V  36
#define GEO_NS_O  48
#define GEO_NS_G  60
#define GEO_NS_U  72
#define GEO_NS_D  84

typedef struct {
    uint32_t  zone;        /* base zone 0..11 */
    int       shape;       /* shape character */
    uint32_t  n_rows;
    uint32_t  n_cols;
    float    *data;        /* pointer into heap buffer */
    uint32_t  ns_shift;    /* namespace shift */
} GeoEntry;

typedef struct {
    uint16_t  raw_zone;
    uint16_t  shape_idx;
    uint64_t  offset;
    uint32_t  n_rows;
    uint32_t  n_cols;
} GeoIndexEntry;

typedef struct {
    GeoIndexEntry *entries;
    uint32_t       n_entries;
    float         *data_buf;    /* gsdat contents */
    uint64_t       data_size;   /* bytes */
    int            is_open;
    int            own_data;    /* 1 = data_buf allocated by us, free on close */
} GeoStore;

#define GEO_OK           0
#define GEO_ERR_OPEN    -1
#define GEO_ERR_MAGIC   -2
#define GEO_ERR_READ    -3
#define GEO_ERR_NOT_FOUND -4

#ifdef GEO_STORE_READER_IMPL

static uint16_t geo_r16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t geo_r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t geo_r64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static uint32_t geo_ns_shift(uint16_t raw_zone) {
    if (raw_zone < GEO_STORE_NS_SHIFT) return 0;
    uint32_t ns = (raw_zone / GEO_STORE_NS_SHIFT) * GEO_STORE_NS_SHIFT;
    return (ns <= GEO_NS_D) ? ns : 0;
}

static int geo_ns_from_char(char c) {
    switch (c) {
        case 'Q': return GEO_NS_Q; case 'K': return GEO_NS_K;
        case 'V': return GEO_NS_V; case 'O': return GEO_NS_O;
        case 'G': return GEO_NS_G; case 'U': return GEO_NS_U;
        case 'D': return GEO_NS_D; default:  return 0;
    }
}

static int geo_shape_idx(int c) {
    const char *p = strchr(GEO_STORE_SHAPES, c);
    return p ? (int)(p - GEO_STORE_SHAPES) : -1;
}

void geo_store_init(GeoStore *s) {
    memset(s, 0, sizeof(*s));
}

int geo_store_open(GeoStore *s, const char *base_path) {
    geo_store_init(s);
    char idx_path[1024], dat_path[1024];
    int len = (int)strlen(base_path);
    if (len > 1000) return GEO_ERR_OPEN;
    memcpy(idx_path, base_path, (size_t)len);
    memcpy(idx_path + len, ".gsidx", 6);
    memcpy(dat_path, base_path, (size_t)len);
    memcpy(dat_path + len, ".gsdat", 6);

    FILE *f = fopen(idx_path, "rb");
    if (!f) return GEO_ERR_OPEN;

    uint8_t hdr[GEO_STORE_HEADER_SZ];
    if (fread(hdr, 1, GEO_STORE_HEADER_SZ, f) != GEO_STORE_HEADER_SZ) {
        fclose(f); return GEO_ERR_READ;
    }
    if (memcmp(hdr, GEO_STORE_MAGIC, GEO_STORE_MAGIC_LEN) != 0) {
        fclose(f); return GEO_ERR_MAGIC;
    }
    uint32_t n_entries = geo_r32(hdr + 8);
    uint64_t data_size = geo_r64(hdr + 12);

    s->entries = (GeoIndexEntry*)malloc((size_t)n_entries * sizeof(GeoIndexEntry));
    if (!s->entries) { fclose(f); return GEO_ERR_READ; }
    s->n_entries = n_entries;

    for (uint32_t i = 0; i < n_entries; i++) {
        uint8_t raw[GEO_STORE_ENTRY_SZ];
        if (fread(raw, 1, GEO_STORE_ENTRY_SZ, f) != GEO_STORE_ENTRY_SZ) {
            fclose(f); free(s->entries); s->entries = NULL; return GEO_ERR_READ;
        }
        s->entries[i].raw_zone  = geo_r16(raw + 0);
        s->entries[i].shape_idx = geo_r16(raw + 2);
        s->entries[i].offset    = geo_r64(raw + 4);
        s->entries[i].n_rows    = geo_r32(raw + 12);
        s->entries[i].n_cols    = geo_r32(raw + 16);
    }
    fclose(f);

    f = fopen(dat_path, "rb");
    if (!f) { free(s->entries); s->entries = NULL; return GEO_ERR_OPEN; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    s->data_size = (uint64_t)fsize;
    if (s->data_size != data_size) {
        /* mismatch: accept whichever is smaller */
        if (s->data_size > data_size) s->data_size = data_size;
    }
    s->data_buf = (float*)malloc((size_t)s->data_size);
    if (!s->data_buf) {
        fclose(f); free(s->entries); s->entries = NULL; return GEO_ERR_READ;
    }
    if (fread(s->data_buf, 1, (size_t)s->data_size, f) != (size_t)s->data_size) {
        fclose(f); free(s->entries); free(s->data_buf);
        s->entries = NULL; s->data_buf = NULL; return GEO_ERR_READ;
    }
    fclose(f);
    s->own_data = 1;
    s->is_open = 1;
    return GEO_OK;
}

int geo_store_open_mem(GeoStore *s, const uint8_t *idx_data, size_t idx_len,
                        float *dat_data, uint64_t dat_size) {
    geo_store_init(s);
    if (idx_len < GEO_STORE_HEADER_SZ) return GEO_ERR_READ;
    if (memcmp(idx_data, GEO_STORE_MAGIC, GEO_STORE_MAGIC_LEN) != 0)
        return GEO_ERR_MAGIC;

    uint32_t n_entries = geo_r32(idx_data + 8);
    size_t needed = GEO_STORE_HEADER_SZ + (size_t)n_entries * GEO_STORE_ENTRY_SZ;
    if (idx_len < needed) return GEO_ERR_READ;

    s->entries = (GeoIndexEntry*)malloc((size_t)n_entries * sizeof(GeoIndexEntry));
    if (!s->entries) return GEO_ERR_READ;
    s->n_entries = n_entries;

    for (uint32_t i = 0; i < n_entries; i++) {
        const uint8_t *raw = idx_data + GEO_STORE_HEADER_SZ
                             + (size_t)i * GEO_STORE_ENTRY_SZ;
        s->entries[i].raw_zone  = geo_r16(raw + 0);
        s->entries[i].shape_idx = geo_r16(raw + 2);
        s->entries[i].offset    = geo_r64(raw + 4);
        s->entries[i].n_rows    = geo_r32(raw + 12);
        s->entries[i].n_cols    = geo_r32(raw + 16);
    }

    s->data_buf = dat_data;
    s->data_size = dat_size;
    s->own_data = 0;
    s->is_open = 1;
    return GEO_OK;
}

void geo_store_close(GeoStore *s) {
    if (!s->is_open) return;
    free(s->entries);
    s->entries = NULL;
    if (s->own_data) {
        free(s->data_buf);
        s->data_buf = NULL;
    }
    s->data_size = 0;
    s->n_entries = 0;
    s->is_open = 0;
    s->own_data = 0;
}

int geo_store_query(GeoStore *s, int ns_char, uint32_t base_zone,
                     int shape_char, GeoEntry *out) {
    if (!s || !s->is_open || !out) return GEO_ERR_OPEN;
    uint32_t ns = (uint32_t)geo_ns_from_char((char)ns_char);
    uint16_t raw_zone = (uint16_t)(ns + base_zone);
    int si = geo_shape_idx(shape_char);
    if (si < 0) return GEO_ERR_NOT_FOUND;
    uint16_t shape_idx = (uint16_t)si;

    for (uint32_t i = 0; i < s->n_entries; i++) {
        if (s->entries[i].raw_zone == raw_zone
            && s->entries[i].shape_idx == shape_idx) {
            uint64_t byte_off = s->entries[i].offset;
            uint64_t n_floats = (uint64_t)s->entries[i].n_rows
                                * (uint64_t)s->entries[i].n_cols;
            uint64_t byte_end = byte_off + n_floats * sizeof(float);
            if (byte_end > s->data_size) return GEO_ERR_READ;

            out->zone     = base_zone;
            out->shape    = shape_char;
            out->n_rows   = s->entries[i].n_rows;
            out->n_cols   = s->entries[i].n_cols;
            out->ns_shift = ns;
            out->data     = (float*)((uint8_t*)s->data_buf + byte_off);
            return GEO_OK;
        }
    }
    return GEO_ERR_NOT_FOUND;
}

int geo_store_has(GeoStore *s, int ns_char, uint32_t base_zone,
                   int shape_char) {
    GeoEntry tmp;
    return geo_store_query(s, ns_char, base_zone, shape_char, &tmp) == GEO_OK;
}

uint32_t geo_store_count(GeoStore *s) {
    return s ? s->n_entries : 0;
}

typedef void (*GeoStoreListCb)(uint32_t i, int ns, uint32_t zone,
                                int shape, uint32_t rows, uint32_t cols,
                                void *udata);
void geo_store_list(GeoStore *s, GeoStoreListCb cb, void *udata) {
    if (!s || !cb) return;
    for (uint32_t i = 0; i < s->n_entries; i++) {
        uint32_t ns = geo_ns_shift(s->entries[i].raw_zone);
        uint32_t base_zone = (uint32_t)s->entries[i].raw_zone - ns;
        int shape = GEO_STORE_SHAPES[s->entries[i].shape_idx];
        cb(i, (int)ns, base_zone, shape, s->entries[i].n_rows,
           s->entries[i].n_cols, udata);
    }
}

#endif /* GEO_STORE_READER_IMPL */

#ifdef __cplusplus
}
#endif

#endif /* GEO_STORE_READER_H */

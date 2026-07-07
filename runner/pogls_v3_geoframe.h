#ifndef POGLS_V3_GEOFRAME_H
#define POGLS_V3_GEOFRAME_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "addr_space.h"

#define GEOF_MAGIC        0x464F4547u  /* "GEOF" */
#define GEOF_VERSION      1u
#define GEOF_HEADER_SZ    32u
#define GEOF_BITMAP_SZ    2592u  /* 20736 bits */
#define GEOF_N_SLOTS      ADDR_BASE  /* 20736 */
#define GEOF_FRAME_FLAG_NONE  0u
#define GEOF_FRAME_FLAG_ZSTD  1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_occupied;
    uint32_t flags;
    uint8_t  _pad[16];
} GeoFHeader;

typedef struct {
    uint32_t addr;
    uint32_t size;
    uint32_t flags;
    uint32_t _pad;
} GeoFFrame;

static inline void geof_header_init(GeoFHeader *hdr) {
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic   = GEOF_MAGIC;
    hdr->version = GEOF_VERSION;
}

static inline int geof_bitmap_get(const uint8_t *bmap, uint32_t addr) {
    return (bmap[addr >> 3] >> (addr & 7)) & 1;
}

static inline void geof_bitmap_set(uint8_t *bmap, uint32_t addr) {
    bmap[addr >> 3] |= (uint8_t)(1u << (addr & 7));
}

static inline uint32_t geof_bitmap_popcount(const uint8_t *bmap, uint32_t up_to_addr) {
    uint32_t cnt = 0;
    uint32_t bytes_full = up_to_addr >> 3;
    uint32_t bits_part = up_to_addr & 7;
    uint32_t i = 0;
    for (; i + 8 <= bytes_full; i += 8) {
        cnt += __builtin_popcountll(*(const uint64_t *)(bmap + i));
    }
    for (; i < bytes_full; i++) {
        cnt += __builtin_popcount(bmap[i]);
    }
    if (bits_part) {
        cnt += __builtin_popcount(bmap[bytes_full] & ((uint8_t)((1u << bits_part) - 1)));
    }
    return cnt;
}

static inline uint64_t geof_file_size(const GeoFHeader *hdr,
                                       const uint64_t *offsets,
                                       uint32_t n) {
    (void)hdr;
    uint64_t data_start = (uint64_t)GEOF_HEADER_SZ + GEOF_BITMAP_SZ + (uint64_t)n * 8;
    if (n == 0) return data_start;
    uint64_t last_off = offsets[n - 1];
    return data_start + last_off;
}

static inline int geof_seek(const uint8_t *bmap, const uint64_t *offsets,
                             uint32_t addr, uint32_t n_occupied,
                             uint32_t *out_frame_idx, uint64_t *out_file_off)
{
    if (addr >= GEOF_N_SLOTS) return -1;
    if (!geof_bitmap_get(bmap, addr)) return -1;
    uint32_t idx = geof_bitmap_popcount(bmap, addr);
    if (idx >= n_occupied) return -2;
    uint64_t data_start = (uint64_t)GEOF_HEADER_SZ + GEOF_BITMAP_SZ + (uint64_t)n_occupied * 8;
    *out_frame_idx = idx;
    *out_file_off  = data_start + offsets[idx];
    return 0;
}

static inline int geof_read_frame(FILE *f, const uint8_t *bmap,
                                   const uint64_t *offsets,
                                   uint32_t addr, uint32_t n_occupied,
                                   uint8_t *out_buf, uint32_t out_cap,
                                   GeoFFrame *out_frame)
{
    uint32_t idx;
    uint64_t file_off;
    int r = geof_seek(bmap, offsets, addr, n_occupied, &idx, &file_off);
    if (r != 0) return r;

#ifdef _WIN32
    if (_fseeki64(f, (__int64)file_off, SEEK_SET) != 0) return -3;
#else
    if (fseeko(f, (off_t)file_off, SEEK_SET) != 0) return -3;
#endif
    GeoFFrame fr;
    if (fread(&fr, sizeof(fr), 1, f) != 1) return -4;

    if (fr.size > out_cap) return -5;
    if (fread(out_buf, fr.size, 1, f) != 1) return -6;

    if (out_frame) *out_frame = fr;
    return 0;
}

static inline int geof_write(const char *path,
                              const uint32_t *addrs, const uint8_t *const *data,
                              const uint32_t *sizes, uint32_t n)
{
    /* build sorted index */
    uint32_t *idx = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!idx) return -1;
    for (uint32_t i = 0; i < n; i++) idx[i] = i;
    for (uint32_t i = 1; i < n; i++) {
        uint32_t j = i;
        uint32_t tmp = idx[i];
        while (j > 0 && addrs[tmp] < addrs[idx[j-1]]) {
            idx[j] = idx[j-1];
            j--;
        }
        idx[j] = tmp;
    }

    /* count unique addresses */
    uint8_t bmap[GEOF_BITMAP_SZ];
    memset(bmap, 0, sizeof(bmap));
    uint32_t uniq_n = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!geof_bitmap_get(bmap, addrs[idx[i]])) {
            geof_bitmap_set(bmap, addrs[idx[i]]);
            uniq_n++;
        }
    }

    GeoFHeader hdr;
    geof_header_init(&hdr);
    hdr.n_occupied = uniq_n;

    /* build offset table (uint64_t) for unique entries */
    uint64_t *offsets = (uint64_t *)malloc(uniq_n * sizeof(uint64_t));
    if (!offsets) { free(idx); return -1; }
    uint64_t cur = 0;
    {
        uint8_t seen[GEOF_BITMAP_SZ];
        memset(seen, 0, sizeof(seen));
        uint32_t out_i = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t ii = idx[i];
            uint32_t a = addrs[ii];
            if (geof_bitmap_get(seen, a)) continue;
            geof_bitmap_set(seen, a);
            offsets[out_i] = cur;
            cur += (uint64_t)sizeof(GeoFFrame) + (uint64_t)sizes[ii];
            out_i++;
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) { free(offsets); free(idx); return -2; }

    if (fwrite(&hdr, GEOF_HEADER_SZ, 1, f) != 1) goto fail;
    if (fwrite(bmap, GEOF_BITMAP_SZ, 1, f) != 1) goto fail;
    if (fwrite(offsets, 8, uniq_n, f) != (size_t)uniq_n) goto fail;

    /* write frames in address order, deduplicated */
    {
        uint8_t seen[GEOF_BITMAP_SZ];
        memset(seen, 0, sizeof(seen));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t ii = idx[i];
            uint32_t a = addrs[ii];
            if (geof_bitmap_get(seen, a)) continue;
            geof_bitmap_set(seen, a);
            GeoFFrame fr;
            fr.addr  = a;
            fr.size  = sizes[ii];
            fr.flags = GEOF_FRAME_FLAG_NONE;
            fr._pad  = 0;
            if (fwrite(&fr, sizeof(fr), 1, f) != 1) goto fail;
            if (sizes[ii] > 0 && data[ii]) {
                if (fwrite(data[ii], sizes[ii], 1, f) != 1) goto fail;
            }
        }
    }

    fclose(f);
    free(offsets);
    free(idx);
    return 0;

fail:
    fclose(f);
    free(offsets);
    free(idx);
    return -3;
}

static inline int geof_read_header(const char *path, GeoFHeader *hdr) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int r = (fread(hdr, GEOF_HEADER_SZ, 1, f) == 1) ? 0 : -1;
    fclose(f);
    if (r == 0 && (hdr->magic != GEOF_MAGIC || hdr->version != GEOF_VERSION))
        return -2;
    return r;
}

static inline int geof_load(const char *path,
                             GeoFHeader *hdr,
                             uint8_t **out_bmap,
                             uint64_t **out_offsets,
                             uint32_t *out_n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fread(hdr, GEOF_HEADER_SZ, 1, f) != 1) { fclose(f); return -1; }
    if (hdr->magic != GEOF_MAGIC || hdr->version != GEOF_VERSION) { fclose(f); return -2; }

    uint32_t n = hdr->n_occupied;
    uint8_t *bmap = (uint8_t *)malloc(GEOF_BITMAP_SZ);
    uint64_t *offsets = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (!bmap || !offsets) { free(bmap); free(offsets); fclose(f); return -3; }

    if (fread(bmap, GEOF_BITMAP_SZ, 1, f) != 1) { free(bmap); free(offsets); fclose(f); return -1; }
    if (fread(offsets, 8, n, f) != (size_t)n) { free(bmap); free(offsets); fclose(f); return -1; }

    fclose(f);
    *out_bmap    = bmap;
    *out_offsets = offsets;
    *out_n       = n;
    return 0;
}

static inline void geof_free(uint8_t *bmap, uint64_t *offsets) {
    free(bmap);
    free(offsets);
}

static inline int geof_verify(const uint8_t *bmap, uint32_t n_occupied) {
    uint32_t actual = 0;
    for (uint32_t i = 0; i < GEOF_N_SLOTS; i++) {
        actual += geof_bitmap_get(bmap, i) ? 1 : 0;
    }
    if (actual != n_occupied) return -1;
    return 0;
}

#endif

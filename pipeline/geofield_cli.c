/*
 * geofield_cli.c — Pure C GeoField Pipeline CLI
 *
 * Standalone encode/decode/verify for GPXL v4 format.
 * Includes header-only modules directly (no DLL dependency).
 *
 * Build:
 *   gcc -O2 -std=c11 -fno-strict-aliasing \
 *       -I. -I../core -I../collection -I../collection/geo_jump_module/include \
 *       -o geofield_cli.exe geofield_cli.c
 *
 * Usage:
 *   geofield_cli encode [-g] [-o out.gpxl] input
 *   geofield_cli decode [-o out.bin] input.gpxl
 *   geofield_cli verify input
 *   geofield_cli info input.gpxl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define GEO_JUMP_INLINE
#include "geo_jump.h"
#include "geo_frame_seek.h"
/* ── Optional: Hamburger / GPX5 output ─────────────────────── */
#ifdef GPXL_USE_GPX5
#include "gpx5_container.h"
#include "hamburger_classify.h"
#include "hamburger_pipe.h"
#include "hamburger_encode.h"
#endif

/* ── Constants ───────────────────────────────────────────────── */
/* CHUNK_SZ = 48 = GEO_BLOCK = 4×4×3 floors (Metatron unit) */
#define CHUNK_SZ         GEO_BLOCK  /* 48 bytes — geo_jump smallest unit */
#define GPXL_MAGIC       0x4C585047  /* "GPXL" little-endian: G(0x47)P(0x50)X(0x58)L(0x4C) */
#define GPXL_VERSION     5
#define GPXL_HEADER_SZ   48
#define GEO_JUMP_HILBERT 0
/* GEO_FULL is defined in geo_jump.h */
#define GEO_FACE_SLOTS   120u

/*
 * Binary Shell codec for 48B blocks
 * ════════════════════════════════════════════════════════════════
 * 48B chunks treated as 3×4×4 cube (z∈[0,2], y∈[0,3], x∈[0,3]).
 * Rotation scan: 6 orientations on the (x,y) plane (z preserved).
 * Each rotation is a bijection → invertible for lossless roundtrip.
 *
 * Wire format:
 *   FLAT   (0): 2B [flag][rot]                       — all-zero chunk
 *   SPARSE (1): 3 + nz×2 B [flag][rot][nz][idx...][val...] — ≤16 non-zero bytes
 *   DENSE  (2): 6 + csz B [flag][rot][csz:4B][zstd_data]   — zstd compressed
 *   PARTIAL(3): 3 + actual_sz B [flag][pad][actual_sz][data...]  — last partial block
 */
#define BS_FLAG_FLAT      0u
#define BS_FLAG_SPARSE    1u
#define BS_FLAG_DENSE     2u
#define BS_FLAG_PARTIAL   3u
#define BS_SPARSE_THRESH  16u
#define BS_ROT_STATES      6u
#define BS_CHUNK_SZ       48u
#define BS_CUBE_SZ        48u

/* 2D rotations on (x,y) plane, z preserved — all are bijections
 * Forward: out[z*16 + y*4 + x] = in[z*16 + sy*4 + sx]
 * Inverse: each self-inverse or has known inverse. */
static void bs_rotate48(uint8_t out[48], const uint8_t in[48], uint8_t rot) {
    for (uint32_t z = 0; z < 3; z++) {
        for (uint32_t y = 0; y < 4; y++) {
            for (uint32_t x = 0; x < 4; x++) {
                uint32_t sx, sy;
                switch (rot % BS_ROT_STATES) {
                    case 0: sx=x;   sy=y;   break;  /* identity */
                    case 1: sx=y;   sy=x;   break;  /* swap x,y */
                    case 2: sx=3-x; sy=y;   break;  /* mirror x */
                    case 3: sx=x;   sy=3-y; break;  /* mirror y */
                    case 4: sx=3-x; sy=3-y; break;  /* mirror both */
                    case 5: sx=3-y; sy=3-x; break;  /* swap+mirror */
                    default: sx=x;  sy=y;   break;
                }
                out[z*16 + y*4 + x] = in[z*16 + sy*4 + sx];
            }
        }
    }
}

/* Inverse rotations — for each forward rotation, compute the inverse.
 * rot=0: identity → inverse = identity (rot=0)
 * rot=1: swap x,y → inverse = swap x,y (rot=1, self-inverse)
 * rot=2: mirror x → inverse = mirror x (rot=2, self-inverse)
 * rot=3: mirror y → inverse = mirror y (rot=3, self-inverse)
 * rot=4: mirror both → inverse = mirror both (rot=4, self-inverse)
 * rot=5: swap+mirror → inverse = swap+mirror (rot=5, self-inverse)
 * All 6 rotations are involutions (self-inverse)! */
static void bs_inv_rotate48(uint8_t out[48], const uint8_t in[48], uint8_t rot) {
    /* Self-inverse: same as forward */
    bs_rotate48(out, in, rot % BS_ROT_STATES);
}

/* Simple fibo_intersect for rotation discrimination */
static uint64_t bs_fibo_intersect(const uint8_t chunk[48]) {
    uint64_t core = 0;
    memcpy(&core, chunk, 8);
    return core & (core >> 1) & (core >> 2) & (core >> 3);
}

/* Encode 48B chunk → Binary Shell compressed block.
 * Wire format:
 *   FLAT  (0): 2B [flag][pad]  — all-zero chunk
 *   SPARSE(1): 3+2*nz B       — ≤16 non-zero bytes (index+value pairs)
 *   RAW   (4): 1+48 B         — incompressible (flag + raw data)
 */
static uint32_t bs_encode_block(uint8_t *out, const uint8_t chunk48[48]) {
    /* FLAT: all-zero */
    int is_zero = 1;
    for (uint32_t i = 0; i < BS_CHUNK_SZ; i++) { if (chunk48[i]) { is_zero = 0; break; } }
    if (is_zero) { out[0] = BS_FLAG_FLAT; out[1] = 0; return 2; }

    /* Count non-zero bytes in each rotation */
    uint8_t  best_rot = 0;
    int      best_nz  = BS_CHUNK_SZ + 1;
    uint8_t  best_buf[BS_CHUNK_SZ];

    for (uint8_t rot = 0; rot < BS_ROT_STATES; rot++) {
        uint8_t rotbuf[BS_CHUNK_SZ];
        bs_rotate48(rotbuf, chunk48, rot);

        int nz = 0;
        for (uint32_t i = 0; i < BS_CHUNK_SZ; i++) if (rotbuf[i]) nz++;

        if (nz < best_nz) {
            best_nz  = nz;
            best_rot = rot;
            memcpy(best_buf, rotbuf, BS_CHUNK_SZ);
        }
    }

    /* SPARSE: ≤16 non-zero bytes → index+value pairs */
    if ((uint32_t)best_nz <= BS_SPARSE_THRESH) {
        out[0] = BS_FLAG_SPARSE;
        out[1] = best_rot;
        out[2] = (uint8_t)best_nz;
        uint32_t pos = 3;
        for (uint32_t i = 0; i < BS_CHUNK_SZ; i++) {
            if (best_buf[i]) {
                out[pos] = (uint8_t)i;
                out[pos + (uint32_t)best_nz] = best_buf[i];
                pos++;
                if (pos >= 3 + (uint32_t)best_nz) break;
            }
        }
        return 3 + (uint32_t)best_nz * 2;
    }

    /* RAW: incompressible — store raw rotated bytes + rotation index (50B total) */
    out[0] = 0x04; /* RAW flag */
    out[1] = best_rot;
    memcpy(out + 2, best_buf, BS_CHUNK_SZ);
    return 2 + BS_CHUNK_SZ;
}

/* Decode Binary Shell block → 48B output */
static uint32_t bs_decode_block(uint8_t out48[48], const uint8_t *in, uint32_t *bytes_read) {
    uint8_t flag = in[0];

    if (flag == BS_FLAG_FLAT) {
        memset(out48, 0, BS_CHUNK_SZ);
        *bytes_read = 2;
        return BS_CHUNK_SZ;
    }

    if (flag == BS_FLAG_SPARSE) {
        uint8_t rot  = in[1];
        uint8_t rotbuf[BS_CHUNK_SZ] = {0};
        uint8_t nz = in[2];
        for (uint32_t i = 0; i < nz; i++) {
            uint8_t idx = in[3 + i];
            uint8_t val = in[3 + nz + i];
            if (idx < BS_CHUNK_SZ) rotbuf[idx] = val;
        }
        bs_inv_rotate48(out48, rotbuf, rot);
        *bytes_read = 3 + (uint32_t)nz * 2;
        return BS_CHUNK_SZ;
    }

    if (flag == BS_FLAG_DENSE) {
        uint8_t rot = in[1];
        uint32_t csz = (uint32_t)in[2] | ((uint32_t)in[3] << 8)
                      | ((uint32_t)in[4] << 16) | ((uint32_t)in[5] << 24);
        uint8_t rotbuf[BS_CHUNK_SZ];
        memcpy(rotbuf, in + 6, csz < BS_CHUNK_SZ ? csz : BS_CHUNK_SZ);
        bs_inv_rotate48(out48, rotbuf, rot);
        *bytes_read = 6 + csz;
        return BS_CHUNK_SZ;
    }

    /* RAW (flag=4): stores [flag][rot][48B rotated data] = 50B */
    if (flag == 0x04) {
        uint8_t rot = in[1];
        uint8_t rotbuf[BS_CHUNK_SZ];
        memcpy(rotbuf, in + 2, BS_CHUNK_SZ);
        bs_inv_rotate48(out48, rotbuf, rot);
        *bytes_read = 2 + BS_CHUNK_SZ;
        return BS_CHUNK_SZ;
    }

    if (flag == BS_FLAG_PARTIAL) {
        uint32_t pad = in[1];
        uint32_t sz  = in[2];
        memset(out48, 0, BS_CHUNK_SZ);
        memcpy(out48, in + 3, sz < BS_CHUNK_SZ ? sz : BS_CHUNK_SZ);
        *bytes_read = 3 + pad + sz;
        return sz;
    }

    memset(out48, 0, BS_CHUNK_SZ);
    *bytes_read = 0;
    return 0;
}

/* High-level encode: 48B chunk → compressed block */
static uint8_t classify_48b(uint8_t *out, const uint8_t chunk[48]) {
    return (uint8_t)bs_encode_block(out, chunk);
}

/* Encode partial block (< 48B) — store raw bytes */
static uint8_t classify_partial48(uint8_t *out, const uint8_t *data, uint32_t actual_sz) {
    if (actual_sz == 0) { out[0] = BS_FLAG_FLAT; out[1] = 0; return 2; }
    out[0] = BS_FLAG_PARTIAL;
    out[1] = 0;
    out[2] = (uint8_t)actual_sz;
    memcpy(out + 3, data, actual_sz);
    return (uint8_t)(3 + actual_sz);
}

/* High-level decode: compressed block → 48B */
static uint32_t decode_48b(uint8_t out[48], const uint8_t *in, uint32_t *bytes_read) {
    return bs_decode_block(out, in, bytes_read);
}

/* ── xxh64 ────────────────────────────────────────────────────── */

static uint64_t xxh64(const uint8_t *data, size_t len) {
    const uint64_t P1 = 0x9E3779B185EBCA87ULL, P2 = 0x14DEF9DEA2F79CD6ULL;
    const uint64_t P3 = 0x165667B19E3779F9ULL, P4 = 0x85EBCA77C2B2ED6BULL;
    const uint64_t P5 = 0x27D4EB2F165667C5ULL;
    uint64_t v1 = P5 + 8, v2 = P4, v3 = 0, v4 = P1;
    size_t off = 0;
    while (off + 32 <= len) {
        const uint64_t *p = (const uint64_t *)(data + off);
        v1 = ((v1 + p[0] * P2) >> 31) * P1;
        v2 = ((v2 + p[1] * P2) >> 31) * P1;
        v3 = ((v3 + p[2] * P2) >> 31) * P1;
        v4 = ((v4 + p[3] * P2) >> 31) * P1;
        off += 32;
    }
    uint64_t result = len;
    if (off < len) {
        uint64_t buf[4] = {0};
        memcpy(buf, data + off, len - off);
        v1 += buf[0] * P2; v1 = ((v1 >> 31) * P1);
        v2 += buf[1] * P2; v2 = ((v2 >> 31) * P1);
        v3 += buf[2] * P2; v3 = ((v3 >> 31) * P1);
        v4 += buf[3] * P2; v4 = ((v4 >> 31) * P1);
    }
    result = (v1 << 1) + (v2 << 7) + (v3 << 12) + (v4 << 18);
    result = ((result ^ (v1 >> 33)) * P2) + P3;
    result = ((result ^ (v2 >> 29)) * P3) + P4;
    result = ((result ^ (v3 >> 32)) * P4) + P5;
    return result;
}

/* ── Helpers ──────────────────────────────────────────────────── */

static int auto_gp_level(size_t data_size) {
    uint64_t n_chunks = (data_size + (CHUNK_SZ - 1)) / CHUNK_SZ;
    for (int level = 1; level <= 15; level++) {
        if ((uint64_t)(10 * level * level + 2) >= n_chunks) return level;
    }
    return 15;
}

/* geo_jump scatter: route chunk to destination node across all 12 faces.
 * Uses Fibonacci stride (37) across GEO_FULL=20736 address space
 * to ensure even distribution across all 12 faces. */
#define FIBO_STRIDE  37u

static uint32_t geo_jump_route(uint32_t start_node, uint32_t chunk_idx,
                                uint32_t *out_enc, uint32_t *out_face,
                                uint32_t *out_slot) {
    /* Fibonacci stride distributes evenly across GEO_FULL address space */
    uint32_t dest = (start_node + (uint64_t)chunk_idx * FIBO_STRIDE) % GEO_FULL;
    uint32_t tower = dest / GEO_TOWER;
    uint32_t local = dest % GEO_TOWER;
    *out_enc = dest % GEO_FIBO_CLOCK;
    *out_face = tower % GEO_PENTAGONS;
    *out_slot = local;
    return dest;
}

/* ── File I/O ─────────────────────────────────────────────────── */

static uint8_t *read_file(const char *path, size_t *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    fclose(f);
    *out_sz = (size_t)sz;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t sz) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot write %s\n", path); return -1; }
    fwrite(data, 1, sz, f);
    fclose(f);
    return 0;
}

/* ── GPXL header fields ───────────────────────────────────────── */

typedef struct {
    uint8_t  gp_level;
    uint8_t  geometric;
    uint32_t n_segments;
    uint64_t original_size;
    uint64_t xxh64;
} GpxlHeader;

static int parse_gpxl_header(const uint8_t *buf, size_t buf_sz, GpxlHeader *h) {
    if (buf_sz < GPXL_HEADER_SZ) return -1;
    if (memcmp(buf, "GPXL", 4) != 0) return -2;
    h->gp_level = buf[6];
    h->geometric = buf[8];
    memcpy(&h->n_segments, buf + 12, 4);
    memcpy(&h->original_size, buf + 16, 8);
    memcpy(&h->xxh64, buf + 24, 8);
    return 0;
}

/* ── ENCODE ───────────────────────────────────────────────────── */

/* Sort index for geo_jump scatter: chunks sorted by dest_node */
typedef struct {
    uint32_t src_idx;   /* original chunk index */
    uint32_t dest_node; /* geo_jump destination */
    uint32_t face;      /* destination face */
    uint32_t slot;      /* destination slot within tower */
} ChunkSlot;

static int cmp_chunk_slot(const void *a, const void *b) {
    const ChunkSlot *ca = (const ChunkSlot *)a;
    const ChunkSlot *cb = (const ChunkSlot *)b;
    /* sort by face first, then by original index within face */
    if (ca->face < cb->face) return -1;
    if (ca->face > cb->face) return 1;
    if (ca->src_idx < cb->src_idx) return -1;
    if (ca->src_idx > cb->src_idx) return 1;
    return 0;
}

static uint8_t *encode_sequential(const uint8_t *data, size_t data_sz,
                                   GpxlHeader *hdr, size_t *out_sz) {
    hdr->gp_level = auto_gp_level(data_sz);
    hdr->geometric = 0;
    hdr->original_size = data_sz;
    hdr->xxh64 = xxh64(data, data_sz);

    /* Step 1: split data into 48B chunks */
    uint32_t n_chunks = (uint32_t)((data_sz + CHUNK_SZ - 1) / CHUNK_SZ);

    /* Step 2: geo_jump scatter — compute destination for each chunk */
    uint32_t start_node = 0; /* start from node 0 */
    ChunkSlot *slots = malloc(sizeof(ChunkSlot) * n_chunks);
    if (!slots) return NULL;

    for (uint32_t i = 0; i < n_chunks; i++) {
        slots[i].src_idx = i;
        uint32_t dest = (start_node + (uint64_t)i * FIBO_STRIDE) % GEO_FULL;
        slots[i].dest_node = dest;
        slots[i].face = dest / (GEO_FULL / GEO_PENTAGONS);
        slots[i].slot = dest % GEO_TOWER;
    }

    /* Step 3: sort by dest_node — creates spatial coherence */
    qsort(slots, n_chunks, sizeof(ChunkSlot), cmp_chunk_slot);

    /* Step 4: group sorted chunks into segments by face */
    /* Each face becomes a segment — chunks in same face are adjacent */
    uint32_t n_segs = 0;
    uint32_t seg_capacity = (GEO_PENTAGONS + 16);
    uint32_t *seg_offsets = calloc(seg_capacity, sizeof(uint32_t));
    uint32_t *seg_lengths = calloc(seg_capacity, sizeof(uint32_t));
    uint32_t *seg_chunk_counts = calloc(seg_capacity, sizeof(uint32_t));

    if (!seg_offsets || !seg_lengths || !seg_chunk_counts) {
        free(slots); free(seg_offsets); free(seg_lengths); free(seg_chunk_counts);
        return NULL;
    }

    /* group by face */
    uint32_t cur_face = slots[0].face;
    seg_offsets[0] = 0;
    n_segs = 1;
    for (uint32_t i = 0; i < n_chunks; i++) {
        if (slots[i].face != cur_face) {
            /* new face = new segment */
            seg_lengths[n_segs - 1] = i - seg_offsets[n_segs - 1];
            cur_face = slots[i].face;
            if (n_segs >= seg_capacity) break;
            seg_offsets[n_segs] = i;
            n_segs++;
        }
    }
    seg_lengths[n_segs - 1] = n_chunks - seg_offsets[n_segs - 1];

    hdr->n_segments = n_segs;

    /* estimate output size */
    size_t est = GPXL_HEADER_SZ + (size_t)n_segs * 72 + (size_t)n_chunks * 51;
    uint8_t *out = malloc(est);
    if (!out) {
        free(slots); free(seg_offsets); free(seg_lengths); free(seg_chunk_counts);
        return NULL;
    }

    /* write header */
    uint32_t magic = GPXL_MAGIC;
    memcpy(out, &magic, 4);
    uint16_t ver = GPXL_VERSION;
    memcpy(out + 4, &ver, 2);
    out[6] = hdr->gp_level;
    out[7] = 0;
    out[8] = hdr->geometric;
    memset(out + 9, 0, 3);
    memcpy(out + 12, &hdr->n_segments, 4);
    memcpy(out + 16, &hdr->original_size, 8);
    memcpy(out + 24, &hdr->xxh64, 8);
    memset(out + 32, 0, 16);

    size_t pos = GPXL_HEADER_SZ;

    /* Step 5: encode each segment (face group) with Diamond Shell classify */
    for (uint32_t si = 0; si < n_segs; si++) {
        uint32_t seg_start = seg_offsets[si];
        uint32_t seg_count = seg_lengths[si];
        uint32_t face_id = slots[seg_start].face;
        uint32_t seg_byte_len = seg_count * CHUNK_SZ;

        /* store face_id + chunk count in segment header */
        /* segment header: [fibo_tick:1][enc:2][seg_byte_len:4][n_blk:2] = 10B */
        uint8_t fibo_tick = (uint8_t)(frame_enc(si) % 144);
        uint16_t enc = (uint16_t)frame_enc(si);
        uint16_t n_blk16 = (uint16_t)seg_count;
        out[pos] = fibo_tick;
        memcpy(out + pos + 1, &enc, 2);
        memcpy(out + pos + 3, &seg_byte_len, 4);
        memcpy(out + pos + 7, &n_blk16, 2);
        pos += 9;

        /* encode each chunk in this face group */
        for (uint32_t ci = 0; ci < seg_count; ci++) {
            uint32_t src_chunk = slots[seg_start + ci].src_idx;
            uint32_t src_off = src_chunk * CHUNK_SZ;
            uint32_t blk_sz = CHUNK_SZ;
            if (src_off + blk_sz > data_sz) blk_sz = (uint32_t)(data_sz - src_off);

            /* classify and store */
            uint8_t classified[51];
            uint8_t sz;
            if (blk_sz < CHUNK_SZ) {
                sz = classify_partial48(classified, data + src_off, blk_sz);
            } else {
                sz = classify_48b(classified, data + src_off);
            }

            /* check output overflow */
            if (pos + sz > est) {
                est = est * 2 + 1024;
                uint8_t *new_out = realloc(out, est);
                if (!new_out) { free(out); free(slots); free(seg_offsets); free(seg_lengths); free(seg_chunk_counts); return NULL; }
                out = new_out;
            }

            memcpy(out + pos, classified, sz);
            pos += sz;
        }
    }

    free(slots);
    free(seg_offsets);
    free(seg_lengths);
    free(seg_chunk_counts);

    *out_sz = pos;
    return out;
}

/* ── DECODE ───────────────────────────────────────────────────── */

static uint8_t *decode_gpxl(const uint8_t *buf, size_t buf_sz,
                             GpxlHeader *hdr, int *ok) {
    if (parse_gpxl_header(buf, buf_sz, hdr) != 0) {
        *ok = 0; return NULL;
    }

    uint8_t *result = calloc(1, hdr->original_size);
    if (!result) { *ok = 0; return NULL; }

    size_t pos = GPXL_HEADER_SZ;

    /* Phase 1: read scattered chunks from GPXL */
    uint32_t n_chunks_total = (uint32_t)((hdr->original_size + CHUNK_SZ - 1) / CHUNK_SZ);
    uint8_t *chunks = malloc((size_t)n_chunks_total * CHUNK_SZ);
    if (!chunks) { free(result); *ok = 0; return NULL; }
    memset(chunks, 0, (size_t)n_chunks_total * CHUNK_SZ);

    /* We need to reconstruct the scatter map:
     * encode did: for each chunk i, dest = geo_jump(0, HILBERT, i+1)
     * then sorted by dest and grouped by face.
     * decode reads segments in face-grouped order.
     * To unscatter, we need the original index of each scattered chunk. */

    /* Build the scatter map: sorted list of (src_idx, dest_node) */
    ChunkSlot *slots = malloc(sizeof(ChunkSlot) * n_chunks_total);
    if (!slots) { free(chunks); free(result); *ok = 0; return NULL; }

    for (uint32_t i = 0; i < n_chunks_total; i++) {
        slots[i].src_idx = i;
        uint32_t dest = (uint64_t)i * FIBO_STRIDE % GEO_FULL;
        slots[i].dest_node = dest;
        slots[i].face = dest / (GEO_FULL / GEO_PENTAGONS);
        slots[i].slot = dest % GEO_TOWER;
    }
    qsort(slots, n_chunks_total, sizeof(ChunkSlot), cmp_chunk_slot);

    /* Read segments in face-grouped order, write chunks to scattered positions */
    uint32_t chunk_read_idx = 0; /* index into sorted slots array */
    for (uint32_t si = 0; si < hdr->n_segments; si++) {
        if (pos + 10 > buf_sz) break;

        pos += 9; /* skip segment header [fibo_tick:1][enc:2][seg_byte_len:4][n_blk:2] = 10B */

        uint16_t n_blk;
        memcpy(&n_blk, buf + pos - 2, 2); /* n_blk from header (little-endian) */

        for (uint32_t bi = 0; bi < n_blk; bi++) {
            if (pos >= buf_sz) break;
            uint8_t decoded[48] = {0};
            uint32_t bytes_read = 0;
            decode_48b(decoded, buf + pos, &bytes_read);
            pos += bytes_read;

            /* write to the scattered chunk position */
            if (chunk_read_idx < n_chunks_total) {
                uint32_t src_idx = slots[chunk_read_idx].src_idx;
                uint32_t src_off = src_idx * CHUNK_SZ;
                if (src_off + CHUNK_SZ <= hdr->original_size) {
                    memcpy(chunks + src_off, decoded, CHUNK_SZ);
                } else if (src_off < hdr->original_size) {
                    uint32_t rem = (uint32_t)(hdr->original_size - src_off);
                    memcpy(chunks + src_off, decoded, rem);
                }
                chunk_read_idx++;
            }
        }
    }

    /* copy to result */
    memcpy(result, chunks, hdr->original_size);

    free(chunks);
    free(slots);

    uint64_t computed = xxh64(result, hdr->original_size);
    *ok = (computed == hdr->xxh64);
    return result;
}

/* ── CLI commands ─────────────────────────────────────────────── */

static void cmd_info(const char *path) {
    size_t sz;
    uint8_t *data = read_file(path, &sz);
    if (!data) return;

    GpxlHeader hdr;
    if (parse_gpxl_header(data, sz, &hdr) != 0) {
        printf("Not a GPXL file\n");
        free(data);
        return;
    }

    printf("File: %s\n", path);
    printf("  Version:    %d\n", GPXL_VERSION);
    printf("  GP level:   %d\n", hdr.gp_level);
    printf("  Geometric:  %s\n", hdr.geometric ? "YES" : "NO");
    printf("  Segments:   %u\n", hdr.n_segments);
    printf("  Orig size:  %llu\n", (unsigned long long)hdr.original_size);
    printf("  File size:  %llu\n", (unsigned long long)sz);
    printf("  xxh64:      0x%016llx\n", (unsigned long long)hdr.xxh64);

    free(data);
}

static void cmd_encode(const char *in_path, const char *out_path, int geometric) {
    size_t data_sz;
    uint8_t *data = read_file(in_path, &data_sz);
    if (!data) return;

    GpxlHeader hdr = {0};
    hdr.geometric = geometric ? 1 : 0;
    size_t enc_sz = 0;
    uint8_t *encoded = encode_sequential(data, data_sz, &hdr, &enc_sz);
    if (!encoded) { free(data); return; }

    /* patch geometric flag in header */
    encoded[8] = hdr.geometric;

    if (write_file(out_path, encoded, enc_sz) == 0) {
        printf("Encode: %s -> %s\n", in_path, out_path);
        printf("  %llu -> %llu bytes (%.2fx)\n",
               (unsigned long long)data_sz, (unsigned long long)enc_sz,
               (double)enc_sz / data_sz);
    }

    free(data);
    free(encoded);
}

static void cmd_decode(const char *in_path, const char *out_path) {
    size_t buf_sz;
    uint8_t *buf = read_file(in_path, &buf_sz);
    if (!buf) return;

    GpxlHeader hdr;
    int ok = 0;
    uint8_t *decoded = decode_gpxl(buf, buf_sz, &hdr, &ok);
    if (!decoded) { free(buf); return; }

    if (write_file(out_path, decoded, hdr.original_size) == 0) {
        printf("Decode: %s -> %s\n", in_path, out_path);
        printf("  %llu -> %llu bytes (xxh64 %s)\n",
               (unsigned long long)buf_sz, (unsigned long long)hdr.original_size,
               ok ? "PASS" : "FAIL");
    }

    free(buf);
    free(decoded);
}

static void cmd_verify(const char *in_path) {
    size_t data_sz;
    uint8_t *data = read_file(in_path, &data_sz);
    if (!data) return;

    /* encode */
    GpxlHeader hdr = {0};
    size_t enc_sz = 0;
    uint8_t *encoded = encode_sequential(data, data_sz, &hdr, &enc_sz);
    if (!encoded) { free(data); return; }

    /* decode */
    int ok = 0;
    uint8_t *decoded = decode_gpxl(encoded, enc_sz, &hdr, &ok);

    int match = (decoded && memcmp(data, decoded, data_sz) == 0);
    printf("Verify: %s\n", in_path);
    printf("  %llu -> %llu -> %llu bytes\n",
           (unsigned long long)data_sz, (unsigned long long)enc_sz,
           (unsigned long long)data_sz);
    printf("  Roundtrip: %s  xxh64: %s\n",
           match ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");

    free(data);
    free(encoded);
    free(decoded);
}

/* ── Main ─────────────────────────────────────────────────────── */

static void usage(void) {
    fprintf(stderr, "Usage: geofield_cli <command> [options] <input>\n\n");
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  encode [-g] [-o out.gpxl] input    Encode to GPXL\n");
    fprintf(stderr, "  decode [-o out.bin] input.gpxl      Decode from GPXL\n");
    fprintf(stderr, "  verify input                       Roundtrip verify\n");
    fprintf(stderr, "  info input.gpxl                    Show GPXL info\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -g, --geometric    Use FrustumBlock scatter (geometric mode)\n");
    fprintf(stderr, "  -o, --output FILE  Output file (default: auto)\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 1; }

    const char *cmd = argv[1];
    int geometric = 0;
    const char *out_path = NULL;
    const char *in_path = NULL;

    /* parse args */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--geometric") == 0) {
            geometric = 1;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 < argc) out_path = argv[++i];
        } else if (argv[i][0] != '-') {
            in_path = argv[i];
        }
    }

    if (!in_path) { usage(); return 1; }

    if (strcmp(cmd, "info") == 0) {
        cmd_info(in_path);
    } else if (strcmp(cmd, "encode") == 0) {
        if (!out_path) {
            static char default_out[512];
            snprintf(default_out, sizeof(default_out), "%s.gpxl", in_path);
            out_path = default_out;
        }
        cmd_encode(in_path, out_path, geometric);
    } else if (strcmp(cmd, "decode") == 0) {
        if (!out_path) {
            static char default_out[512];
            size_t len = strlen(in_path);
            if (len > 5 && strcmp(in_path + len - 5, ".gpxl") == 0) {
                snprintf(default_out, sizeof(default_out), "%.*s",
                         (int)(len - 5), in_path);
            } else {
                snprintf(default_out, sizeof(default_out), "%s.dec", in_path);
            }
            out_path = default_out;
        }
        cmd_decode(in_path, out_path);
    } else if (strcmp(cmd, "verify") == 0) {
        cmd_verify(in_path);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage();
        return 1;
    }

    return 0;
}

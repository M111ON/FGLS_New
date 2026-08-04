/* ═══════════════════════════════════════════════════════════════════════════
 * fgls_tensor_archive.h — GGUF "Mount" Archive: Lazy / Random-Access
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * GOAL (user's DRamTile-as-drive analogy):
 *   Present our KIS-compressed archive as a VIRTUAL GGUF file so llama.cpp
 *   (unpatched, via gguf_init_from_callback) reads tensors lazily, one at a
 *   time, on demand — like GearLock pulling a DRamTile on read.
 *
 * ARCHITECTURE:
 *   The exported .fgls file looks EXACTLY like the original GGUF at the
 *   metadata layer (header, KV pairs, tensor-info table, and original
 *   tensor offsets are kept verbatim). Only the tensor DATA region is
 *   replaced by KIS-v4 blobs (one per tensor). A footer index maps
 *   virtual-GGUF-offset → owning tensor → blob location.
 *
 *   Physical layout:
 *     [ 0 .. data_offset)         header+metadata+tensor-info  (verbatim A)
 *     [ blob region ]             one KIS-v4 blob per tensor     (C)
 *     [ footer ]                  magic FGLT / version / data_offset /
 *                                 per-tensor: name,type,v_offset,raw_size,
 *                                              blob_offset,blob_size
 *     footer_len (u32, LAST 4 bytes)  → backward-searchable footer
 *
 *   Virtual GGUF byte-stream (what llama.cpp sees via callback):
 *     offset <  data_offset → A[offset]
 *     offset >= data_offset → decode owning tensor's blob (lazy, cached) → slice
 *
 * LOSSLESS: KIS v4 roundtrip proven on real Q8_0 (0 mismatches). Whole-tensor
 * byte arrays are encoded as-is (type-agnostic), so every dtype survives.
 *
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef FGLS_TENSOR_ARCHIVE_H
#define FGLS_TENSOR_ARCHIVE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#define FGLT_MAGIC   0x544C4746u  /* "FGLT" */
#define FGLT_VERSION 2u           /* v2: blob layer = zstd (v1 used KIS, expands 1.7x on real Q8_0) */
#define FGLT_ZSTD_LVL 9

#pragma pack(push, 1)
typedef struct {
    char      name[256];
    uint8_t   type;
    uint64_t  v_offset;   /* virtual GGUF absolute offset where raw data lives */
    uint64_t  raw_size;   /* decoded size in bytes */
    uint64_t  blob_offset; /* file offset of KIS blob (weights for Q8_0) */
    uint32_t  blob_size;   /* compressed size */
    /* second blob: for Q8_0 (type 8), scale bytes (2 f16 per 32-weight block)
     * stored RAW so KIS sees only int8 weights. unused (=0) for other types. */
    uint64_t  blob2_offset;
    uint32_t  blob2_size;
} FGLT_IndexEntry;

#pragma pack(pop)

typedef struct {
    /* mmaped archive */
    uint8_t   *base;        /* mapped file base */
    size_t     file_size;
    uint64_t   data_offset;  /* virtual GGUF data start (=tensor_data_start) */
    uint64_t   n;            /* number of archived tensors */
    uint64_t   index_start;  /* file offset of footer */
    FGLT_IndexEntry *idx;
    /* lazy decode cache: decoded tensor bytes, one buffer per tensor */
    uint8_t  **cache;
    int      *cache_valid;
    uint64_t  decoded_total; /* stats */
    uint64_t  read_calls;
} FGLT_Archive;

/* tiny FATAL helper */
static void _fglt_fatal(const char *m){ fprintf(stderr, "FGLT fatal: %s\n", m); exit(1); }

/* ── Bake: read a GGUF, write .fgls archive ───────────────────────────────── */
/* Requires caller to already have parsed the GGUF via gguf_reader.h:
 *   gf = gguf_open(path): gives tensor_count, tensors[i] (name,type,offset,
 *   size_bytes, n_weights), tensor_data_start.
 * We copy [0,tensor_data_start) verbatim, then KIS-encode each tensor's
 * data bytes from the original file.
 */
typedef struct {
    uint64_t n_tensors;
    uint64_t data_offset;
    /* read tensor raw bytes: fill out[0..size) from original model at virtual
     * offset v_offset (absolute). Return 0 ok. */
    int (*read_tensor_src)(void *ud, uint64_t v_offset, uint8_t *out, uint64_t size);
    void *ud;
} FGLT_BakeIO;

static int fglt_bake(const char *out_path,
                     const char * const *names, const uint8_t *_types,
                     const uint64_t *_v_offsets, const uint64_t *_raw_sizes,
                     uint64_t n_tensors, uint64_t data_offset,
                     FGLT_BakeIO *io)
{
    FILE *f = fopen(out_path, "wb");
    if (!f) return -1;

    size_t hdr_cap = (size_t)data_offset;
    uint8_t *hdr = (uint8_t*)malloc(hdr_cap ? hdr_cap : 1);
    if (hdr_cap && io->read_tensor_src(io->ud, 0, hdr, hdr_cap) != 0)
        { free(hdr); fclose(f); return -2; }
    if (fwrite(hdr, 1, hdr_cap, f) != hdr_cap) { free(hdr); fclose(f); return -3; }
    free(hdr);

    /* BLOB region: one KIS blob per tensor */
    uint64_t *b1 = (uint64_t*)calloc(n_tensors ? n_tensors : 1, sizeof(uint64_t));
    uint32_t *s1 = (uint32_t*)calloc(n_tensors ? n_tensors : 1, sizeof(uint32_t));
    uint64_t *b2 = (uint64_t*)calloc(n_tensors ? n_tensors : 1, sizeof(uint64_t));
    uint32_t *s2 = (uint32_t*)calloc(n_tensors ? n_tensors : 1, sizeof(uint32_t));
    uint8_t *raw = NULL; uint64_t raw_cap = 0;
    uint8_t *enc = NULL; uint64_t enc_cap = 0;

    for (uint64_t i = 0; i < n_tensors; i++) {
        if (_raw_sizes[i] > raw_cap) { raw_cap = _raw_sizes[i]; raw = (uint8_t*)realloc(raw, raw_cap); }
        if (io->read_tensor_src(io->ud, _v_offsets[i], raw, _raw_sizes[i]) != 0)
            { free(raw); free(enc); free(b1); free(s1); free(b2); free(s2); fclose(f); return -4; }

        /* ALL types: zstd-compress the whole tensor blob. If zstd expands
         * (incompressible data), fall back to RAW — never pay a penalty.
         * KIS v4 was tried (v1) but expands 1.7x on real Q8_0: permutation
         * delta-varint costs ~1B/weight regardless of structure. */
        if (_raw_sizes[i] > 0) {
            size_t bound = ZSTD_compressBound((size_t)_raw_sizes[i]);
            if (bound > enc_cap) { enc_cap = bound; enc = (uint8_t*)realloc(enc, enc_cap); }
            size_t csz = ZSTD_compress(enc, bound, raw, (size_t)_raw_sizes[i], FGLT_ZSTD_LVL);
            if (ZSTD_isError(csz)) { free(raw); free(enc); free(b1); free(s1); free(b2); free(s2); fclose(f); return -8; }
            if (csz < _raw_sizes[i]) {
                b1[i] = (uint64_t)ftell(f);
                s1[i] = (uint32_t)csz;
                if (fwrite(enc, 1, csz, f) != csz) { free(raw); free(enc); free(b1); free(s1); free(b2); free(s2); fclose(f); return -9; }
                b2[i] = 0; s2[i] = 0; /* compressed */
            } else {
                b1[i] = (uint64_t)ftell(f);
                s1[i] = (uint32_t)_raw_sizes[i];
                if (fwrite(raw, 1, _raw_sizes[i], f) != _raw_sizes[i]) { free(raw); free(enc); free(b1); free(s1); free(b2); free(s2); fclose(f); return -9; }
                b2[i] = 0; s2[i] = 0; /* raw fallback */
            }
        } else {
            b1[i]=0; s1[i]=0; b2[i]=0; s2[i]=0;
        }
    }
    free(raw); free(enc);

    /* Footer */
    uint64_t footer_start = (uint64_t)ftell(f);
    uint32_t magic = FGLT_MAGIC;
    uint32_t ver = FGLT_VERSION;
    if (fwrite(&magic, 4, 1, f) != 1 || fwrite(&ver, 4, 1, f) != 1 ||
        fwrite(&data_offset, 8, 1, f) != 1 || fwrite(&n_tensors, 8, 1, f) != 1)
        { free(b1); free(s1); free(b2); free(s2); fclose(f); return -10; }
    for (uint64_t i = 0; i < n_tensors; i++) {
        FGLT_IndexEntry e;
        memset(&e, 0, sizeof(e));
        strncpy(e.name, names[i] ? names[i] : "", 255);
        e.type = _types[i];
        e.v_offset = _v_offsets[i];
        e.raw_size = _raw_sizes[i];
        e.blob_offset = b1[i];
        e.blob_size = s1[i];
        e.blob2_offset = b2[i];
        e.blob2_size = s2[i];
        if (fwrite(&e, sizeof(e), 1, f) != 1)
            { free(b1); free(s1); free(b2); free(s2); fclose(f); return -11; }
    }
    uint64_t footer_len = (uint64_t)ftell(f) - footer_start;
    if (fwrite(&footer_len, 8, 1, f) != 1)
        { free(b1); free(s1); free(b2); free(s2); fclose(f); return -12; }
    free(b1); free(s1); free(b2); free(s2);
    fclose(f);
    return 0;
}

/* ── Open / mmap ──────────────────────────────────────────────────────────── */
static int fglt_open(const char *path, FGLT_Archive *a)
{
    memset(a, 0, sizeof(*a));
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END); a->file_size = (size_t)ftell(fp); fseek(fp, 0, SEEK_SET);
    if (a->file_size < 40) { fclose(fp); return -2; }
    a->base = (uint8_t*)malloc(a->file_size);
    if (!a->base) { fclose(fp); return -3; }
    if (fread(a->base, 1, a->file_size, fp) != a->file_size) { fclose(fp); free(a->base); return -4; }
    fclose(fp);

    /* Find footer from end */
    uint64_t footer_len;
    memcpy(&footer_len, a->base + a->file_size - 8, 8);
    uint64_t footer_start = a->file_size - 8 - footer_len;
    if (footer_start >= a->file_size) { free(a->base); return -6; }
    uint32_t magic, ver;
    memcpy(&magic, a->base + footer_start, 4);
    memcpy(&ver,  a->base + footer_start + 4, 4);
    if (magic != FGLT_MAGIC || ver != FGLT_VERSION) { free(a->base); return -7; }
    memcpy(&a->data_offset, a->base + footer_start + 8, 8);
    memcpy(&a->n,            a->base + footer_start + 16, 8);
    a->index_start = footer_start + 24;
    a->idx = (FGLT_IndexEntry*)calloc(a->n ? a->n : 1, sizeof(FGLT_IndexEntry));
    if (!a->idx) { free(a->base); return -8; }
    memcpy(a->idx, a->base + a->index_start, a->n * sizeof(FGLT_IndexEntry));
    a->cache = (uint8_t**)calloc(a->n ? a->n : 1, sizeof(uint8_t*));
    a->cache_valid = (int*)calloc(a->n ? a->n : 1, sizeof(int));
    if (!a->cache || !a->cache_valid) { free(a->idx); free(a->base); free(a->cache); free(a->cache_valid); return -9; }
    return 0;
}

static void fglt_close(FGLT_Archive *a)
{
    for (uint64_t i = 0; i < a->n; i++) { if (a->cache_valid[i]) free(a->cache[i]); }
    free(a->cache); free(a->cache_valid); free(a->idx); free(a->base);
    memset(a, 0, sizeof(*a));
}

/* Find tensor index by virtual offset. Linear (n ~ few hundred). */
static int fglt_find_by_offset(FGLT_Archive *a, uint64_t off)
{
    for (uint64_t i = 0; i < a->n; i++) {
        const FGLT_IndexEntry *e = &a->idx[i];
        if (off >= e->v_offset && off < e->v_offset + e->raw_size) return (int)i;
    }
    return -1;
}

/* Ensure tensor i decoded into cache. Returns 0 ok. */
static int fglt_decode_tensor(FGLT_Archive *a, uint64_t i)
{
    if (a->cache_valid[i]) return 0;
    const FGLT_IndexEntry *e = &a->idx[i];
    if (e->raw_size == 0) { a->cache[i] = NULL; a->cache_valid[i] = 1; return 0; }
    a->cache[i] = (uint8_t*)malloc((size_t)e->raw_size);
    if (!a->cache[i]) return -1;

    /* blob is either zstd-compressed (< raw_size) or RAW fallback (== raw_size) */
    if (e->blob_size == e->raw_size) {
        memcpy(a->cache[i], a->base + e->blob_offset, (size_t)e->raw_size);
    } else {
        size_t dsz = ZSTD_decompress(a->cache[i], (size_t)e->raw_size,
                                     a->base + e->blob_offset, e->blob_size);
        if (ZSTD_isError(dsz) || dsz != e->raw_size) {
            free(a->cache[i]); a->cache[i] = NULL; return -3;
        }
    }
    a->cache_valid[i] = 1;
    a->decoded_total += e->raw_size;
    return 0;
}

/* ── Direct random-access read of a whole tensor by name ──────────────────── */
/* Decodes the named tensor into `out` (caller must allocate >= raw_size).
 * Returns 0 ok, -1 decode error, -2 not found. */
static int fglt_read_tensor(FGLT_Archive *a, const char *name, uint8_t *out)
{
    for (uint64_t i = 0; i < a->n; i++) {
        if (strcmp(a->idx[i].name, name) == 0) {
            if (fglt_decode_tensor(a, i) != 0) return -1;
            if (a->cache_valid[i] && a->idx[i].raw_size > 0)
                memcpy(out, a->cache[i], (size_t)a->idx[i].raw_size);
            return 0;
        }
    }
    return -2; /* not found */
}

/* ── Callback for gguf_init_from_callback (the "mount" hook) ──────────────── */
/* Reads up to `len` bytes at virtual `offset` into `output`. Returns bytes read.
 * This is EXACTLY the gguf_reader_callback_t signature llama.cpp calls. */
static size_t fglt_callback(void *userdata, void *output, uint64_t offset, size_t len)
{
    FGLT_Archive *a = (FGLT_Archive*)userdata;
    a->read_calls++;
    uint8_t *out = (uint8_t*)output;
    size_t written = 0;

    /* header/metadata region: serve verbatim */
    if (offset < a->data_offset) {
        uint64_t avail = a->data_offset - offset;
        uint64_t want = (uint64_t)len < avail ? (uint64_t)len : avail;
        memcpy(out + written, a->base + offset, (size_t)want);
        written += (size_t)want;
        offset   += want;
        len      -= (size_t)want;
        if (len == 0) return written;
    }

    /* tensor data region: decode owning tensor */
    while (len > 0) {
        int ti = fglt_find_by_offset(a, offset);
        if (ti < 0) break; /* past end */
        if (fglt_decode_tensor(a, (uint64_t)ti) != 0) break;
        const FGLT_IndexEntry *e = &a->idx[ti];
        uint64_t within = offset - e->v_offset;
        uint64_t avail  = e->raw_size - within;
        uint64_t want   = (uint64_t)len < avail ? (uint64_t)len : avail;
        memcpy(out + written, a->cache[ti] + within, (size_t)want);
        written += (size_t)want;
        offset  += want;
        len     -= (size_t)want;
    }
    return written;
}

#endif /* FGLS_TENSOR_ARCHIVE_H */
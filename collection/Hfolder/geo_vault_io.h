/*
 * geo_vault_io.h — GeoVault File I/O Layer  (v2: per-frame zstd)
 * ════════════════════════════════════════════════════════════════
 *
 * File layout (v2):
 *   [64B]        GeoVaultHeader
 *   [N×32B]      GeoVaultFileEntry  (n_files entries)
 *   [8B]         path_block_size (uint64 LE)
 *   [path_block] null-terminated paths (sequential)
 *   [F×16B]      GeoVaultFrameEntry seek table  (n_frames entries)
 *   [frame0.zst] compressed frame 0
 *   [frame1.zst] compressed frame 1
 *   ...
 *
 * v1 layout (legacy read support):
 *   ... [entries + paths + 8B_path_sz] [single zstd block]
 *
 * Per-frame lazy decompress: only the frame(s) needed for a file
 * are loaded. Idle frames stay on disk. Suited for large vaults.
 * ════════════════════════════════════════════════════════════════
 */

#ifndef GEO_VAULT_IO_H
#define GEO_VAULT_IO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/file.h>   /* flock(2) */
#include <zstd.h>
#include "geo_vault.h"

#define GV_MAX_FILES     4096
#define GV_MAX_PATH       512
#define GV_ZSTD_LEVEL       3

/* ── Extended Attributes (xattr) ────────────────────────────────────
 * In-memory per-file key-value store. Not persisted to disk (v2).
 * Key max 128B, value max 1024B. Up to GV_XATTR_MAX per file.
 * ──────────────────────────────────────────────────────────────────── */
#define GV_XATTR_MAX      16
#define GV_XATTR_KEY_MAX 128
#define GV_XATTR_VAL_MAX 1024

typedef struct {
    char     key[GV_XATTR_KEY_MAX];
    uint8_t  val[GV_XATTR_VAL_MAX];
    uint32_t val_sz;
} GvXAttr;

typedef struct {
    GvXAttr  attrs[GV_XATTR_MAX];
    uint32_t n;
} GvXAttrSet;


/* ── Hash index (open-addressing, power-of-2 table) ─────────────────
 * Maps FNV1a(path) → file index.  Rebuilt in memory after open/add.
 * Load factor kept ≤ 0.5  →  avg O(1) lookup, worst O(1) amortized.
 * ──────────────────────────────────────────────────────────────────── */
#define GV_HIDX_EMPTY  0xFFFFFFFFu   /* sentinel: slot unused          */
#define GV_HIDX_MIN    16u           /* minimum table size             */

/* ── Runtime vault context ────────────────────────────────────────── */
typedef struct {
    GeoVaultHeader      hdr;
    GeoVaultFileEntry  *entries;        /* n_files × 32B                 */
    char              **paths;          /* n_files paths                 */

    /* O(1) path lookup: open-addressing hash table                     */
    uint32_t           *hidx;           /* hidx[slot] = file index       */
    uint32_t            hidx_cap;       /* power-of-2 capacity           */

    /* v2: per-frame lazy cache */
    GeoVaultFrameEntry *frame_table;    /* n_frames × 16B, NULL for v1  */
    uint8_t           **frame_cache;    /* frame_cache[i] = decompressed */

    /* v1 compat: single decompressed block */
    uint8_t            *frames;         /* v1 only; v2 uses frame_cache  */
    uint64_t            frames_size;    /* n_frames × GV_FRAME_BYTES    */

    uint64_t            compressed_offset; /* v1: offset of single block */
    FILE               *fp;
    int                 fd;    /* fileno(fp) — for flock              */

    /* per-file xattr store (runtime only, not persisted) */
    GvXAttrSet         *xattrs;         /* n_files GvXAttrSet, lazily alloc  */
} GeoVaultCtx;

/* ── Hash index helpers ───────────────────────────────────────────── */
static void _hidx_build(GeoVaultCtx *ctx) {
    /* capacity = next power-of-2 ≥ 2×n_files, min GV_HIDX_MIN */
    uint32_t n = ctx->hdr.n_files;
    uint32_t cap = GV_HIDX_MIN;
    while (cap < n * 2u) cap <<= 1;

    free(ctx->hidx);
    ctx->hidx     = malloc(cap * sizeof(uint32_t));
    ctx->hidx_cap = cap;
    memset(ctx->hidx, 0xFF, cap * sizeof(uint32_t));  /* all EMPTY */

    uint32_t mask = cap - 1u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t h = ctx->entries[i].name_hash & mask;
        while (ctx->hidx[h] != GV_HIDX_EMPTY) h = (h + 1u) & mask;
        ctx->hidx[h] = i;
    }
}

static int _hidx_lookup(const GeoVaultCtx *ctx, const char *rel_path) {
    if (!ctx->hidx || ctx->hidx_cap == 0) return -1;
    uint32_t hash = gv_fnv1a(rel_path);
    uint32_t mask = ctx->hidx_cap - 1u;
    uint32_t h    = hash & mask;
    for (uint32_t probe = 0; probe < ctx->hidx_cap; probe++) {
        uint32_t idx = ctx->hidx[h];
        if (idx == GV_HIDX_EMPTY) return -1;           /* not found    */
        if (ctx->entries[idx].name_hash == hash &&
            strcmp(ctx->paths[idx], rel_path) == 0)
            return (int)idx;
        h = (h + 1u) & mask;
    }
    return -1;
}

static void _hidx_insert(GeoVaultCtx *ctx, uint32_t file_idx) {
    /* rebuild if load factor > 0.5 */
    if (ctx->hdr.n_files * 2u >= ctx->hidx_cap) {
        _hidx_build(ctx);
        return;
    }
    uint32_t mask = ctx->hidx_cap - 1u;
    uint32_t h    = ctx->entries[file_idx].name_hash & mask;
    while (ctx->hidx[h] != GV_HIDX_EMPTY) h = (h + 1u) & mask;
    ctx->hidx[h] = file_idx;
}

/* ════════════════════════════════════════════════════════════════════
 * WRITE (v2): pack files → per-frame compressed .geovault
 * ════════════════════════════════════════════════════════════════════ */
int gv_write(const char **file_paths, const char **rel_paths,
             uint32_t n, const char *out_path) {

    if (n > GV_MAX_FILES) return -1;

    /* ── load all files ─────────────────────────────────────────── */
    uint8_t **bufs  = calloc(n, sizeof(uint8_t*));
    uint32_t *sizes = calloc(n, sizeof(uint32_t));
    uint64_t  total_raw   = 0;
    uint32_t  total_slots = 0;

    for (uint32_t i = 0; i < n; i++) {
        FILE *f = fopen(file_paths[i], "rb");
        if (!f) { fprintf(stderr, "gv_write: cannot open %s\n", file_paths[i]); return -2; }
        fseek(f, 0, SEEK_END); sizes[i] = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
        bufs[i] = malloc(sizes[i] ? sizes[i] : 1);
        if (sizes[i]) { size_t r = fread(bufs[i], 1, sizes[i], f); (void)r; }
        fclose(f);
        total_raw   += sizes[i];
        total_slots += gv_slots_needed(sizes[i]);
    }

    uint32_t n_frames = (total_slots + GV_RESIDUAL - 1) / GV_RESIDUAL;
    if (n_frames == 0) n_frames = 1;

    /* ── build per-frame pixel buffers ─────────────────────────── */
    uint64_t frames_size = (uint64_t)n_frames * GV_FRAME_BYTES;
    uint8_t *frames = calloc(1, frames_size);

    GeoVaultFileEntry *entries = calloc(n, sizeof(GeoVaultFileEntry));
    uint8_t folder_sha[20] = {0};
    uint32_t global_slot = 0;

    for (uint32_t fi = 0; fi < n; fi++) {
        uint32_t slots = gv_slots_needed(sizes[fi]);
        GeoVaultFileEntry *e = &entries[fi];
        e->name_hash   = gv_fnv1a(rel_paths[fi]);
        e->raw_size    = sizes[fi];
        e->slot_start  = global_slot % GV_RESIDUAL;
        e->slot_count  = slots;
        e->frame_start = global_slot / GV_RESIDUAL;

        uint32_t h = gv_fnv1a(rel_paths[fi]);
        for (uint32_t b = 0; b < sizes[fi]; b++) {
            h ^= bufs[fi][b]; h *= 0x01000193u;
            if (b < 12) e->file_sha[b] = bufs[fi][b] ^ (uint8_t)b;
        }
        for (int b = 0; b < 20; b++) folder_sha[b] ^= (uint8_t)(h >> (b%4*8));

        for (uint32_t ci = 0; ci < slots; ci++) {
            uint32_t frame    = global_slot / GV_RESIDUAL;
            uint32_t slot_inf = global_slot % GV_RESIDUAL;
            uint8_t *fbuf     = frames + (uint64_t)frame * GV_FRAME_BYTES;
            uint32_t src_off  = ci * GV_CHUNK_BYTES;
            uint32_t remain   = sizes[fi] > src_off ? sizes[fi] - src_off : 0;
            gv_pack_chunk(bufs[fi] + src_off, remain, slot_inf, fbuf);
            global_slot++;
        }
    }

    /* ── compress each frame independently ─────────────────────── */
    size_t             bound     = ZSTD_compressBound(GV_FRAME_BYTES);
    uint8_t           *cbuf      = malloc(bound);
    GeoVaultFrameEntry *ftable   = calloc(n_frames, sizeof(GeoVaultFrameEntry));
    uint8_t          **cframes   = calloc(n_frames, sizeof(uint8_t*));
    size_t            *cframe_sz = calloc(n_frames, sizeof(size_t));
    uint64_t           total_csz = 0;

    for (uint32_t f = 0; f < n_frames; f++) {
        uint8_t *fbuf = frames + (uint64_t)f * GV_FRAME_BYTES;
        size_t csz = ZSTD_compress(cbuf, bound, fbuf, GV_FRAME_BYTES, GV_ZSTD_LEVEL);
        cframes[f]   = malloc(csz);
        cframe_sz[f] = csz;
        memcpy(cframes[f], cbuf, csz);
        total_csz += csz;
    }
    free(cbuf);
    free(frames);

    /* ── compute frame seek table offsets ───────────────────────── */
    uint64_t path_block_size = 0;
    for (uint32_t i = 0; i < n; i++) path_block_size += strlen(rel_paths[i]) + 1;

    uint64_t data_offset = sizeof(GeoVaultHeader)
                         + (uint64_t)n * sizeof(GeoVaultFileEntry)
                         + 8                       /* path_block_size field */
                         + path_block_size
                         + (uint64_t)n_frames * sizeof(GeoVaultFrameEntry);

    for (uint32_t f = 0; f < n_frames; f++) {
        ftable[f].offset        = data_offset;
        ftable[f].compressed_sz = (uint32_t)cframe_sz[f];
        data_offset += cframe_sz[f];
    }

    /* ── build header ───────────────────────────────────────────── */
    GeoVaultHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic         = GV_MAGIC;
    hdr.version       = GV_VERSION_2;
    hdr.flags         = GVF_COMPRESSED | GVF_MULTIFRAME | GVF_PER_FRAME;
    hdr.n_files       = n;
    hdr.n_frames      = n_frames;
    hdr.total_raw     = total_raw;
    hdr.compressed_sz = total_csz;
    hdr.frame_bytes   = GV_FRAME_BYTES;
    memcpy(hdr.folder_sha, folder_sha, 20);

    /* ── write file ─────────────────────────────────────────────── */
    FILE *out = fopen(out_path, "wb");
    if (!out) { free(entries); free(ftable); return -3; }

    fwrite(&hdr,     sizeof(hdr),                       1,        out);
    fwrite(entries,  sizeof(GeoVaultFileEntry),          n,        out);
    fwrite(&path_block_size, 8,                          1,        out);
    for (uint32_t i = 0; i < n; i++)
        fwrite(rel_paths[i], 1, strlen(rel_paths[i]) + 1, out);
    fwrite(ftable,   sizeof(GeoVaultFrameEntry),         n_frames, out);
    for (uint32_t f = 0; f < n_frames; f++) {
        fwrite(cframes[f], 1, cframe_sz[f], out);
        free(cframes[f]);
    }
    fclose(out);

    fprintf(stderr,
        "gv_write(v2): %u files  raw=%llu  frames=%u  "
        "pixel_buf=%llu  compressed=%llu  ratio=%.2fx\n",
        n, (unsigned long long)total_raw, n_frames,
        (unsigned long long)frames_size, (unsigned long long)total_csz,
        (double)frames_size / (double)total_csz);

    for (uint32_t i = 0; i < n; i++) free(bufs[i]);
    free(bufs); free(sizes); free(entries); free(ftable);
    free(cframes); free(cframe_sz);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * OPEN: load header + file table + frame seek table (no decompress)
 * ════════════════════════════════════════════════════════════════════ */
GeoVaultCtx *gv_open(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    GeoVaultCtx *ctx = calloc(1, sizeof(GeoVaultCtx));
    ctx->fp = fp;
    ctx->fd = fileno(fp);
    if (flock(ctx->fd, LOCK_SH) != 0) goto fail;  /* shared read lock */

    if (fread(&ctx->hdr, sizeof(GeoVaultHeader), 1, fp) != 1) goto fail;
    if (ctx->hdr.magic != GV_MAGIC) goto fail;

    uint32_t n = ctx->hdr.n_files;
    uint32_t f = ctx->hdr.n_frames;

    ctx->entries = malloc(n * sizeof(GeoVaultFileEntry));
    if (fread(ctx->entries, sizeof(GeoVaultFileEntry), n, fp) != n) goto fail;

    /* paths */
    ctx->paths = calloc(n, sizeof(char*));
    uint64_t path_block_size;
    if (fread(&path_block_size, 8, 1, fp) != 1) goto fail;
    char *path_block = malloc(path_block_size);
    if (fread(path_block, 1, path_block_size, fp) != path_block_size) goto fail;
    char *p = path_block;
    for (uint32_t i = 0; i < n; i++) { ctx->paths[i] = strdup(p); p += strlen(p) + 1; }
    free(path_block);

    /* build O(1) hash index */
    _hidx_build(ctx);

    ctx->frames_size = (uint64_t)f * GV_FRAME_BYTES;

    if (ctx->hdr.version >= GV_VERSION_2 && (ctx->hdr.flags & GVF_PER_FRAME)) {
        /* v2: load frame seek table only — no decompress yet */
        ctx->frame_table = malloc(f * sizeof(GeoVaultFrameEntry));
        if (fread(ctx->frame_table, sizeof(GeoVaultFrameEntry), f, fp) != f) goto fail;
        ctx->frame_cache = calloc(f, sizeof(uint8_t*));
        ctx->frames = NULL;
    } else {
        /* v1 compat */
        ctx->compressed_offset = (uint64_t)ftell(fp);
        ctx->frame_table = NULL;
        ctx->frame_cache = NULL;
        ctx->frames = NULL;
    }

    return ctx;
fail:
    fclose(fp);
    if (ctx->entries)     free(ctx->entries);
    if (ctx->frame_table) free(ctx->frame_table);
    free(ctx);
    return NULL;
}

/* forward declaration — defined below, needed by gv_open_rw */
void gv_close(GeoVaultCtx *ctx);

/* ════════════════════════════════════════════════════════════════════
 * OPEN_RW: open for writing — exclusive lock, r+b mode
 * Use for gv_update / gv_add / gv_compact.
 * ════════════════════════════════════════════════════════════════════ */
GeoVaultCtx *gv_open_rw(const char *path) {
    /* open read-only first to parse header, then reopen r+b with LOCK_EX */
    GeoVaultCtx *ctx = gv_open(path);
    if (!ctx) return NULL;

    /* upgrade: release shared lock, reopen r+b, acquire exclusive */
    flock(ctx->fd, LOCK_UN);
    fclose(ctx->fp);

    FILE *fp = fopen(path, "r+b");
    if (!fp) { ctx->fp = NULL; ctx->fd = -1; gv_close(ctx); return NULL; }
    ctx->fp = fp;
    ctx->fd = fileno(fp);

    if (flock(ctx->fd, LOCK_EX) != 0) {
        fclose(fp); ctx->fp = NULL; gv_close(ctx); return NULL;
    }
    return ctx;
}

/* ════════════════════════════════════════════════════════════════════
 * LOAD SINGLE FRAME (v2) — decompress exactly one frame on demand
 * ════════════════════════════════════════════════════════════════════ */
int gv_load_frame(GeoVaultCtx *ctx, uint32_t frame_idx) {
    if (!ctx->frame_cache)           return -1; /* v1 — use gv_load_frames */
    if (frame_idx >= ctx->hdr.n_frames) return -1;
    if (ctx->frame_cache[frame_idx]) return 0;  /* already in cache */

    GeoVaultFrameEntry *fe = &ctx->frame_table[frame_idx];
    fseek(ctx->fp, (long)fe->offset, SEEK_SET);

    uint8_t *cbuf = malloc(fe->compressed_sz);
    size_t nr = fread(cbuf, 1, fe->compressed_sz, ctx->fp);
    if (nr != fe->compressed_sz) { free(cbuf); return -2; }

    ctx->frame_cache[frame_idx] = malloc(GV_FRAME_BYTES);
    size_t r = ZSTD_decompress(ctx->frame_cache[frame_idx], GV_FRAME_BYTES,
                                cbuf, fe->compressed_sz);
    free(cbuf);
    if (ZSTD_isError(r)) {
        free(ctx->frame_cache[frame_idx]);
        ctx->frame_cache[frame_idx] = NULL;
        return -3;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * LOAD ALL FRAMES — bulk preload (or v1 single block)
 * ════════════════════════════════════════════════════════════════════ */
int gv_load_frames(GeoVaultCtx *ctx) {
    if (ctx->frame_table) {
        /* v2 */
        for (uint32_t f = 0; f < ctx->hdr.n_frames; f++) {
            int r = gv_load_frame(ctx, f);
            if (r < 0) return r;
        }
        return 0;
    }
    /* v1 */
    if (ctx->frames) return 0;
    fseek(ctx->fp, (long)ctx->compressed_offset, SEEK_SET);
    uint8_t *cbuf = malloc(ctx->hdr.compressed_sz);
    size_t nr = fread(cbuf, 1, ctx->hdr.compressed_sz, ctx->fp);
    if (nr != ctx->hdr.compressed_sz) { free(cbuf); return -1; }
    ctx->frames = malloc(ctx->frames_size);
    size_t r = ZSTD_decompress(ctx->frames, ctx->frames_size,
                                cbuf, ctx->hdr.compressed_sz);
    free(cbuf);
    if (ZSTD_isError(r)) { free(ctx->frames); ctx->frames = NULL; return -2; }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * GET FRAME BUFFER — transparent v1/v2, lazy load
 * ════════════════════════════════════════════════════════════════════ */
static inline uint8_t *gv_get_frame(GeoVaultCtx *ctx, uint32_t frame_idx) {
    if (ctx->frame_cache) {
        if (!ctx->frame_cache[frame_idx])
            if (gv_load_frame(ctx, frame_idx) < 0) return NULL;
        return ctx->frame_cache[frame_idx];
    }
    if (!ctx->frames) return NULL;
    return ctx->frames + (uint64_t)frame_idx * GV_FRAME_BYTES;
}

/* ════════════════════════════════════════════════════════════════════
 * EXTRACT ONE FILE by index
 * ════════════════════════════════════════════════════════════════════ */
uint8_t *gv_extract(GeoVaultCtx *ctx, uint32_t file_idx) {
    if (file_idx >= ctx->hdr.n_files) return NULL;
    GeoVaultFileEntry *e = &ctx->entries[file_idx];
    if (e->raw_size == 0) return calloc(1, 1);

    uint8_t *out = malloc(e->raw_size);
    uint32_t global_slot = e->frame_start * GV_RESIDUAL + e->slot_start;

    for (uint32_t ci = 0; ci < e->slot_count; ci++) {
        uint32_t frame    = global_slot / GV_RESIDUAL;
        uint32_t slot_inf = global_slot % GV_RESIDUAL;
        uint8_t *fbuf     = gv_get_frame(ctx, frame);
        if (!fbuf) { free(out); return NULL; }
        uint32_t dst_off = ci * GV_CHUNK_BYTES;
        uint32_t remain  = e->raw_size > dst_off ? e->raw_size - dst_off : 0;
        gv_unpack_chunk(fbuf, slot_inf, out + dst_off, remain);
        global_slot++;
    }
    return out;
}

/* ════════════════════════════════════════════════════════════════════
 * FIND FILE by path — O(1) via hash index
 * ════════════════════════════════════════════════════════════════════ */
int gv_find(GeoVaultCtx *ctx, const char *rel_path) {
    int r = _hidx_lookup(ctx, rel_path);
    if (r >= 0) return r;
    /* fallback linear scan (e.g. index not yet built) */
    uint32_t h = gv_fnv1a(rel_path);
    for (uint32_t i = 0; i < ctx->hdr.n_files; i++) {
        if (ctx->entries[i].name_hash == h &&
            strcmp(ctx->paths[i], rel_path) == 0)
            return (int)i;
    }
    return -1;
}

/* ════════════════════════════════════════════════════════════════════
 * LIST: print vault contents
 * ════════════════════════════════════════════════════════════════════ */
void gv_list(GeoVaultCtx *ctx) {
    uint32_t nfr = ctx->hdr.n_frames;
    uint64_t csz = ctx->hdr.compressed_sz;
    printf("GeoVault v%u  files=%u  frames=%u  raw=%llu  compressed=%llu\n",
           ctx->hdr.version, ctx->hdr.n_files, nfr,
           (unsigned long long)ctx->hdr.total_raw, (unsigned long long)csz);
    if (csz > 0)
        printf("  ratio=%.2fx  mode=%s\n",
               (double)((uint64_t)nfr * GV_FRAME_BYTES) / (double)csz,
               (ctx->hdr.version >= GV_VERSION_2) ? "per-frame" : "single-block");
    printf("  %-6s  %-8s  %-6s  %-5s  %-4s  %s\n",
           "idx","size","slot","frame","cmpd","path");
    printf("  %-6s  %-8s  %-6s  %-5s  %-4s  %s\n",
           "---","----","----","-----","----","----");
    for (uint32_t i = 0; i < ctx->hdr.n_files; i++) {
        GeoVaultFileEntry *e = &ctx->entries[i];
        GeoVaultAddr a = gv_addr(e->slot_start);
        printf("  %-6u  %-8u  %-6u  %-5u  %-4u  %s\n",
               i, e->raw_size, e->slot_start, e->frame_start,
               a.compound, ctx->paths[i]);
    }
    if (ctx->frame_table) {
        printf("\n  Frame seek table:\n");
        printf("  %-6s  %-14s  %s\n", "frame", "file_offset", "csz");
        for (uint32_t f = 0; f < nfr; f++)
            printf("  %-6u  %-14llu  %u\n", f,
                   (unsigned long long)ctx->frame_table[f].offset,
                   ctx->frame_table[f].compressed_sz);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * CLOSE
 * ════════════════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════════════════
 * XATTR API — in-memory extended attributes per file
 *   gv_xattr_set   — set (or replace) a key-value pair
 *   gv_xattr_get   — retrieve value; returns val_sz or -ENODATA
 *   gv_xattr_list  — fill buf with "key\0key\0..." style list
 *   gv_xattr_remove — delete an attribute
 *   All ops: O(n) where n ≤ GV_XATTR_MAX (16) → effectively O(1)
 * ════════════════════════════════════════════════════════════════════ */

/* ensure xattr array is allocated */
static GvXAttrSet * __attribute__((unused)) _xa_ensure(GeoVaultCtx *ctx, int file_idx) {
    if (file_idx < 0 || (uint32_t)file_idx >= ctx->hdr.n_files) return NULL;
    if (!ctx->xattrs) {
        ctx->xattrs = calloc(ctx->hdr.n_files, sizeof(GvXAttrSet));
        if (!ctx->xattrs) return NULL;
    }
    return &ctx->xattrs[file_idx];
}

/* set or replace xattr key */
static int __attribute__((unused)) gv_xattr_set(GeoVaultCtx *ctx, int file_idx,
                         const char *key, const void *val, uint32_t val_sz) {
    if (!key || !key[0])        return -1;
    if (val_sz > GV_XATTR_VAL_MAX) return -1;
    GvXAttrSet *xs = _xa_ensure(ctx, file_idx);
    if (!xs) return -1;

    /* look for existing key */
    for (uint32_t i = 0; i < xs->n; i++) {
        if (strncmp(xs->attrs[i].key, key, GV_XATTR_KEY_MAX) == 0) {
            memcpy(xs->attrs[i].val, val, val_sz);
            xs->attrs[i].val_sz = val_sz;
            return 0;
        }
    }
    /* insert new */
    if (xs->n >= GV_XATTR_MAX) return -1;  /* -ENOSPC */
    GvXAttr *a = &xs->attrs[xs->n++];
    strncpy(a->key, key, GV_XATTR_KEY_MAX - 1);
    a->key[GV_XATTR_KEY_MAX - 1] = 0;
    memcpy(a->val, val, val_sz);
    a->val_sz = val_sz;
    return 0;
}

/* get xattr; returns val_sz on success, -1 if not found */
static int __attribute__((unused)) gv_xattr_get(GeoVaultCtx *ctx, int file_idx,
                         const char *key, void *buf, uint32_t buf_sz) {
    if (!ctx->xattrs) return -1;
    if (file_idx < 0 || (uint32_t)file_idx >= ctx->hdr.n_files) return -1;
    GvXAttrSet *xs = &ctx->xattrs[file_idx];
    for (uint32_t i = 0; i < xs->n; i++) {
        if (strncmp(xs->attrs[i].key, key, GV_XATTR_KEY_MAX) == 0) {
            if (buf_sz == 0) return (int)xs->attrs[i].val_sz;  /* size query */
            if (buf_sz < xs->attrs[i].val_sz) return -1;       /* -ERANGE   */
            memcpy(buf, xs->attrs[i].val, xs->attrs[i].val_sz);
            return (int)xs->attrs[i].val_sz;
        }
    }
    return -1;  /* -ENODATA */
}

/* list xattr keys as null-terminated sequence; returns total bytes needed */
static int __attribute__((unused)) gv_xattr_list(GeoVaultCtx *ctx, int file_idx,
                          char *buf, uint32_t buf_sz) {
    if (!ctx->xattrs) return 0;
    if (file_idx < 0 || (uint32_t)file_idx >= ctx->hdr.n_files) return -1;
    GvXAttrSet *xs = &ctx->xattrs[file_idx];
    uint32_t total = 0;
    for (uint32_t i = 0; i < xs->n; i++)
        total += (uint32_t)strlen(xs->attrs[i].key) + 1;
    if (buf_sz == 0) return (int)total;  /* size query */
    if (buf_sz < total) return -1;       /* -ERANGE */
    uint32_t off = 0;
    for (uint32_t i = 0; i < xs->n; i++) {
        uint32_t klen = (uint32_t)strlen(xs->attrs[i].key) + 1;
        memcpy(buf + off, xs->attrs[i].key, klen);
        off += klen;
    }
    return (int)total;
}

/* remove xattr; returns 0 on success, -1 if not found */
static int __attribute__((unused)) gv_xattr_remove(GeoVaultCtx *ctx, int file_idx, const char *key) {
    if (!ctx->xattrs) return -1;
    if (file_idx < 0 || (uint32_t)file_idx >= ctx->hdr.n_files) return -1;
    GvXAttrSet *xs = &ctx->xattrs[file_idx];
    for (uint32_t i = 0; i < xs->n; i++) {
        if (strncmp(xs->attrs[i].key, key, GV_XATTR_KEY_MAX) == 0) {
            xs->attrs[i] = xs->attrs[--xs->n];  /* swap-remove */
            return 0;
        }
    }
    return -1;  /* -ENODATA */
}

void gv_close(GeoVaultCtx *ctx) {
    if (!ctx) return;
    if (ctx->fp) { flock(ctx->fd, LOCK_UN); fclose(ctx->fp); }
    if (ctx->frames)  free(ctx->frames);
    if (ctx->hidx)    free(ctx->hidx);
    if (ctx->frame_cache) {
        for (uint32_t f = 0; f < ctx->hdr.n_frames; f++)
            if (ctx->frame_cache[f]) free(ctx->frame_cache[f]);
        free(ctx->frame_cache);
    }
    if (ctx->frame_table) free(ctx->frame_table);
    if (ctx->entries)     free(ctx->entries);
    if (ctx->paths) {
        for (uint32_t i = 0; i < ctx->hdr.n_files; i++) free(ctx->paths[i]);
        free(ctx->paths);
    }
    if (ctx->xattrs) free(ctx->xattrs);
    free(ctx);
}

/* ════════════════════════════════════════════════════════════════════
 * INTERNAL: rewrite header + file table + frame table to open file
 *   fp must be opened "r+b", positioned doesn't matter.
 *   Caller owns frame data already on disk — we only update metadata.
 * ════════════════════════════════════════════════════════════════════ */
static int _gv_rewrite_header(GeoVaultCtx *ctx) {
    FILE *fp = ctx->fp;

    /* rebuild path block */
    uint32_t n = ctx->hdr.n_files;
    uint64_t path_block_size = 0;
    for (uint32_t i = 0; i < n; i++)
        path_block_size += strlen(ctx->paths[i]) + 1;

    fseek(fp, 0, SEEK_SET);
    fwrite(&ctx->hdr,    sizeof(GeoVaultHeader),   1, fp);
    fwrite(ctx->entries, sizeof(GeoVaultFileEntry), n, fp);
    fwrite(&path_block_size, 8, 1, fp);
    for (uint32_t i = 0; i < n; i++)
        fwrite(ctx->paths[i], 1, strlen(ctx->paths[i]) + 1, fp);
    fwrite(ctx->frame_table, sizeof(GeoVaultFrameEntry), ctx->hdr.n_frames, fp);
    return fflush(fp) == 0 ? 0 : -1;
}

/* ════════════════════════════════════════════════════════════════════
 * INTERNAL: compress a frame buffer and return malloc'd result + size
 * ════════════════════════════════════════════════════════════════════ */
static uint8_t *_gv_compress_frame(const uint8_t *fbuf, size_t *out_sz) {
    size_t   bound = ZSTD_compressBound(GV_FRAME_BYTES);
    uint8_t *cbuf  = malloc(bound);
    size_t   csz   = ZSTD_compress(cbuf, bound, fbuf, GV_FRAME_BYTES, GV_ZSTD_LEVEL);
    if (ZSTD_isError(csz)) { free(cbuf); return NULL; }
    *out_sz = csz;
    return cbuf;
}

/* ════════════════════════════════════════════════════════════════════
 * gv_update — IN-PLACE write (same size or smaller)
 *
 *   Overwrites existing file's slots in the frame buffer,
 *   recompresses only the affected frame(s), patches them back
 *   into the vault file at their current offsets.
 *
 *   Constraint: new_size <= original raw_size.
 *   For larger files use gv_add() (which replaces + appends).
 *
 *   Returns:
 *     0  = success
 *    -1  = file not found
 *    -2  = new_size > original (use gv_add instead)
 *    -3  = frame too large to fit back (compressed grew > original slot)
 *    -4  = I/O error
 * ════════════════════════════════════════════════════════════════════ */
int gv_update(GeoVaultCtx *ctx, const char *rel_path,
              const uint8_t *new_data, uint32_t new_size) {

    if (!ctx->frame_table) return -4;  /* v1 not supported */

    int idx = gv_find(ctx, rel_path);
    if (idx < 0) return -1;

    GeoVaultFileEntry *e = &ctx->entries[idx];
    if (new_size > e->raw_size) return -2;  /* caller: use gv_add */

    /* load all frames this file spans */
    uint32_t global_end = e->frame_start * GV_RESIDUAL + e->slot_start
                        + (e->slot_count > 0 ? e->slot_count - 1 : 0);
    uint32_t frame_end  = global_end / GV_RESIDUAL;
    for (uint32_t f = e->frame_start; f <= frame_end; f++)
        if (gv_load_frame(ctx, f) < 0) return -4;

    /* zero-out old slots, write new data */
    uint32_t global_slot = e->frame_start * GV_RESIDUAL + e->slot_start;
    uint32_t new_slots   = gv_slots_needed(new_size);

    for (uint32_t ci = 0; ci < e->slot_count; ci++) {
        uint32_t frame    = global_slot / GV_RESIDUAL;
        uint32_t slot_inf = global_slot % GV_RESIDUAL;
        uint8_t *fbuf     = ctx->frame_cache[frame];
        if (ci < new_slots) {
            uint32_t src_off = ci * GV_CHUNK_BYTES;
            uint32_t remain  = new_size > src_off ? new_size - src_off : 0;
            gv_pack_chunk(new_data + src_off, remain, slot_inf, fbuf);
        } else {
            /* zero pad unused trailing slots */
            memset(fbuf + slot_inf * GV_CHUNK_BYTES, 0, GV_CHUNK_BYTES);
        }
        global_slot++;
    }

    /* recompress & patch affected frames back to disk */
    FILE *fp = ctx->fp;
    for (uint32_t f = e->frame_start; f <= frame_end; f++) {
        size_t   csz;
        uint8_t *cbuf = _gv_compress_frame(ctx->frame_cache[f], &csz);
        if (!cbuf) return -4;

        /* in-place: compressed result must fit in original slot */
        if (csz > ctx->frame_table[f].compressed_sz) {
            free(cbuf);
            return -3;  /* grew — caller should use gv_add */
        }

        fseek(fp, (long)ctx->frame_table[f].offset, SEEK_SET);
        size_t written = fwrite(cbuf, 1, csz, fp);
        free(cbuf);
        if (written != csz) return -4;

        /* update seek table entry size (may have shrunk) */
        ctx->frame_table[f].compressed_sz = (uint32_t)csz;
    }

    /* update file entry metadata */
    e->raw_size   = new_size;
    e->slot_count = new_slots;
    ctx->hdr.total_raw = ctx->hdr.total_raw - (e->raw_size) + new_size;

    /* patch header + metadata in file */
    if (_gv_rewrite_header(ctx) < 0) return -4;

    fprintf(stderr, "gv_update: [in-place] %s  %u bytes  frames=%u..%u\n",
            rel_path, new_size, e->frame_start, frame_end);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * gv_add — APPEND file (new file or replacement when size grew)
 *
 *   Strategy:
 *     1. If rel_path exists → mark old slots as dead (zero, no repack)
 *     2. Pack new data into a fresh frame appended to vault
 *     3. Update/add file entry, grow frame table, rewrite header
 *
 *   The vault file grows. Call gv_compact() to reclaim dead space.
 *
 *   Returns:
 *     0  = success (new file added)
 *     1  = success (existing file replaced, old slots orphaned)
 *    -1  = I/O error
 *    -2  = too many files (> GV_MAX_FILES)
 * ════════════════════════════════════════════════════════════════════ */
int gv_add(GeoVaultCtx *ctx, const char *rel_path,
           const uint8_t *data, uint32_t data_size) {

    if (!ctx->frame_table) return -1;  /* v1 not supported */
    if (ctx->hdr.n_files >= GV_MAX_FILES) return -2;

    int replaced = 0;
    int old_idx  = gv_find(ctx, rel_path);

    /* ── find last used slot to know where to start appending ─── */
    /* scan all entries for highest global_slot used */
    uint32_t tail_slot = 0;  /* next free global slot */
    for (uint32_t i = 0; i < ctx->hdr.n_files; i++) {
        GeoVaultFileEntry *e2 = &ctx->entries[i];
        uint32_t end = e2->frame_start * GV_RESIDUAL
                     + e2->slot_start + e2->slot_count;
        if (end > tail_slot) tail_slot = end;
    }

    /* ── build new file's pixel buffer (may span multiple frames) ── */
    uint32_t new_slots   = gv_slots_needed(data_size);
    uint32_t first_frame = tail_slot / GV_RESIDUAL;
    uint32_t first_slot  = tail_slot % GV_RESIDUAL;

    /* how many frames does new data span? */
    uint32_t last_global = tail_slot + new_slots - 1;
    uint32_t last_frame  = last_global / GV_RESIDUAL;
    uint32_t n_new_frames = last_frame - first_frame + 1;

    /* alloc frame buffers (may overlap with existing frames) */
    /* existing frames in [first_frame .. ctx->hdr.n_frames-1] may partially fill */
    uint32_t total_frames_needed = last_frame + 1;
    /* grow frame_cache / frame_table if needed */
    if (total_frames_needed > ctx->hdr.n_frames) {
        uint32_t old_n = ctx->hdr.n_frames;
        uint32_t new_n = total_frames_needed;

        ctx->frame_table = realloc(ctx->frame_table,
                                   new_n * sizeof(GeoVaultFrameEntry));
        memset(ctx->frame_table + old_n, 0,
               (new_n - old_n) * sizeof(GeoVaultFrameEntry));

        ctx->frame_cache = realloc(ctx->frame_cache, new_n * sizeof(uint8_t*));
        for (uint32_t f = old_n; f < new_n; f++)
            ctx->frame_cache[f] = calloc(1, GV_FRAME_BYTES);  /* zeroed */

        ctx->hdr.n_frames  = new_n;
        ctx->frames_size   = (uint64_t)new_n * GV_FRAME_BYTES;
    }

    /* load existing frames that new data will partially fill */
    for (uint32_t f = first_frame; f <= last_frame; f++) {
        if (f < ctx->hdr.n_frames && !ctx->frame_cache[f])
            gv_load_frame(ctx, f);
        if (!ctx->frame_cache[f])
            ctx->frame_cache[f] = calloc(1, GV_FRAME_BYTES);
    }

    /* write new data into frame cache */
    uint32_t global_slot = tail_slot;
    for (uint32_t ci = 0; ci < new_slots; ci++) {
        uint32_t frame    = global_slot / GV_RESIDUAL;
        uint32_t slot_inf = global_slot % GV_RESIDUAL;
        uint32_t src_off  = ci * GV_CHUNK_BYTES;
        uint32_t remain   = data_size > src_off ? data_size - src_off : 0;
        gv_pack_chunk(data + src_off, remain, slot_inf, ctx->frame_cache[frame]);
        global_slot++;
    }

    /* ── compress & append new/changed frames to end of vault file ── */
    FILE *fp = ctx->fp;
    fseek(fp, 0, SEEK_END);
    uint64_t append_offset = (uint64_t)ftell(fp);

    uint64_t new_total_csz = ctx->hdr.compressed_sz;

    for (uint32_t f = first_frame; f <= last_frame; f++) {
        size_t   csz;
        uint8_t *cbuf = _gv_compress_frame(ctx->frame_cache[f], &csz);
        if (!cbuf) return -1;

        uint64_t this_offset = append_offset;
        fwrite(cbuf, 1, csz, fp);
        free(cbuf);

        /* if frame already existed, old compressed data stays (orphaned) */
        /* new seek table entry always points to freshly appended copy    */
        uint32_t old_csz = ctx->frame_table[f].compressed_sz;
        ctx->frame_table[f].offset        = this_offset;
        ctx->frame_table[f].compressed_sz = (uint32_t)csz;

        new_total_csz = new_total_csz - old_csz + csz;
        append_offset += csz;
    }
    ctx->hdr.compressed_sz = new_total_csz;

    /* ── update or append file entry ───────────────────────────── */
    uint32_t fnv = gv_fnv1a(rel_path);

    if (old_idx >= 0) {
        /* replace: update existing entry in-place */
        GeoVaultFileEntry *e = &ctx->entries[old_idx];
        ctx->hdr.total_raw  -= e->raw_size;
        e->name_hash  = fnv;
        e->raw_size   = data_size;
        e->slot_start = first_slot;
        e->slot_count = new_slots;
        e->frame_start= first_frame;
        /* file_sha */
        for (uint32_t b = 0; b < 12 && b < data_size; b++)
            e->file_sha[b] = data[b] ^ (uint8_t)b;
        ctx->hdr.total_raw += data_size;
        replaced = 1;
    } else {
        /* new file: append entry */
        uint32_t ni = ctx->hdr.n_files;
        ctx->entries = realloc(ctx->entries, (ni + 1) * sizeof(GeoVaultFileEntry));
        ctx->paths   = realloc(ctx->paths,   (ni + 1) * sizeof(char*));

        GeoVaultFileEntry *e = &ctx->entries[ni];
        memset(e, 0, sizeof(*e));
        e->name_hash  = fnv;
        e->raw_size   = data_size;
        e->slot_start = first_slot;
        e->slot_count = new_slots;
        e->frame_start= first_frame;
        for (uint32_t b = 0; b < 12 && b < data_size; b++)
            e->file_sha[b] = data[b] ^ (uint8_t)b;

        ctx->paths[ni] = strdup(rel_path);
        ctx->hdr.n_files++;
        ctx->hdr.total_raw += data_size;
        _hidx_insert(ctx, ni);  /* O(1) index update */
    }

    /* ── rewrite header section (metadata only) ─────────────────── */
    if (_gv_rewrite_header(ctx) < 0) return -1;

    fprintf(stderr, "gv_add: [append] %s  %u bytes  frames=%u..%u  %s\n",
            rel_path, data_size, first_frame, last_frame,
            replaced ? "(replaced)" : "(new)");
    (void)n_new_frames;
    return replaced ? 1 : 0;
}

/* ════════════════════════════════════════════════════════════════════
 * gv_compact — reclaim orphaned frame space (after gv_add replacements)
 *
 *   Rebuilds vault from scratch using current live entries only.
 *   Writes to a temp file, then renames over original.
 *
 *   Returns 0 on success, <0 on error.
 * ════════════════════════════════════════════════════════════════════ */
int gv_compact(GeoVaultCtx *ctx, const char *vault_path) {
    uint32_t n = ctx->hdr.n_files;

    /* extract all live files into memory */
    uint8_t  **bufs     = calloc(n, sizeof(uint8_t*));
    uint32_t  *sizes    = calloc(n, sizeof(uint32_t));
    const char **rpaths = calloc(n, sizeof(char*));

    for (uint32_t i = 0; i < n; i++) {
        bufs[i]   = gv_extract(ctx, i);
        sizes[i]  = ctx->entries[i].raw_size;
        rpaths[i] = ctx->paths[i];
        if (!bufs[i] && sizes[i] > 0) {
            for (uint32_t j = 0; j < i; j++) free(bufs[j]);
            free(bufs); free(sizes); free(rpaths);
            return -1;
        }
    }

    /* write to temp file */
    char tmp_path[GV_MAX_PATH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.compact.tmp", vault_path);

    /* gv_write needs file_paths[] but we have buffers — write to tmpfiles */
    char **tmpfiles = calloc(n, sizeof(char*));
    for (uint32_t i = 0; i < n; i++) {
        tmpfiles[i] = malloc(GV_MAX_PATH);
        snprintf(tmpfiles[i], GV_MAX_PATH, "%s.f%u.tmp", vault_path, i);
        FILE *tf = fopen(tmpfiles[i], "wb");
        if (tf) { fwrite(bufs[i] ? bufs[i] : (uint8_t*)"", 1,
                         sizes[i], tf); fclose(tf); }
        free(bufs[i]);
    }
    free(bufs);

    int r = gv_write((const char**)tmpfiles, rpaths, n, tmp_path);

    for (uint32_t i = 0; i < n; i++) {
        remove(tmpfiles[i]);
        free(tmpfiles[i]);
    }
    free(tmpfiles); free(sizes); free(rpaths);

    if (r < 0) { remove(tmp_path); return r; }

    /* atomic replace */
    if (rename(tmp_path, vault_path) != 0) { remove(tmp_path); return -2; }

    fprintf(stderr, "gv_compact: vault rebuilt  files=%u\n", n);
    return 0;
}

#endif /* GEO_VAULT_IO_H */

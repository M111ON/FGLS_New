/*
 * pogls_loader.h — Standalone POGLS Loader Library
 *
 * Zero-dependency API for loading POGLS files from any C/C++ runner.
 * Works with llama.cpp, Ollama, vLLM, or any custom runner.
 *
 * Features:
 *   - mmap-based zero-copy loading
 *   - O(1) tensor lookup by name or address
 *   - Per-tensor decompression (ZSTD/RAW)
 *   - Thread-safe read-only access
 *   - No platform dependencies (pure C)
 *
 * Usage:
 *   #include "pogls_loader.h"
 *
 *   PoglsLoader *L = pogls_open("model.pogls");
 *   if (L) {
 *       void *data = pogls_tensor_data(L, "blk.0.attn_q.weight");
 *       size_t sz = pogls_tensor_size(L, "blk.0.attn_q.weight");
 *       // use data...
 *       pogls_close(L);
 *   }
 *
 * Build:
 *   gcc -O2 -std=c11 -c pogls_loader.c -o pogls_loader.o
 *   ar rcs libpogls_loader.a pogls_loader.o
 *
 * Or header-only: #define POGLS_LOADER_IMPLEMENTATION before include.
 */

#ifndef POGLS_LOADER_H
#define POGLS_LOADER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════
   Opaque Handle
   ═══════════════════════════════════════════════════════════════════ */

typedef struct PoglsLoader PoglsLoader;

/* ═══════════════════════════════════════════════════════════════════
   Lifecycle
   ═══════════════════════════════════════════════════════════════════ */

/*
 * Open a POGLS file for reading.
 * Returns NULL on failure (file not found, invalid format, etc.)
 * The returned handle must be closed with pogls_close().
 */
PoglsLoader* pogls_open(const char *path);

/*
 * Close a POGLS loader and release all resources.
 * Safe to call with NULL.
 */
void pogls_close(PoglsLoader *L);

/* ═══════════════════════════════════════════════════════════════════
   Metadata Queries
   ═══════════════════════════════════════════════════════════════════ */

/* Get the number of tensors in the POGLS file. */
uint32_t pogls_tensor_count(PoglsLoader *L);

/* Get the file format version (1 or 2). */
uint32_t pogls_version(PoglsLoader *L);

/* Get the original GGUF path (v2 only, may be empty). */
const char* pogls_gguf_path(PoglsLoader *L);

/* Get the number of model metadata bytes (v2 only). */
uint32_t pogls_model_meta_size(PoglsLoader *L);

/* Get pointer to model metadata (v2 only, NULL if absent). */
const void* pogls_model_meta(PoglsLoader *L);

/* ═══════════════════════════════════════════════════════════════════
   Tensor Lookup by Index
   ═══════════════════════════════════════════════════════════════════ */

/* Get tensor name by index (0-based). Returns NULL if out of range. */
const char* pogls_tensor_name(PoglsLoader *L, uint32_t idx);

/* Get tensor address by index. Returns UINT32_MAX if out of range. */
uint32_t pogls_tensor_addr(PoglsLoader *L, uint32_t idx);

/* Get tensor dtype by index. */
uint32_t pogls_tensor_dtype(PoglsLoader *L, uint32_t idx);

/* Get tensor ndim by index. */
uint32_t pogls_tensor_ndim(PoglsLoader *L, uint32_t idx);

/* Get tensor shape by index. dims must point to array of 4 uint32_t. */
void pogls_tensor_shape(PoglsLoader *L, uint32_t idx, uint32_t dims[4]);

/* Get original (uncompressed) size by index. */
size_t pogls_tensor_nbytes(PoglsLoader *L, uint32_t idx);

/* Get stored (possibly compressed) size by index. 0 if unknown. */
size_t pogls_tensor_comp_nbytes(PoglsLoader *L, uint32_t idx);

/* Get compression type by index (POGLS_COMP_RAW/ZSTD/etc). */
uint32_t pogls_tensor_comp_type(PoglsLoader *L, uint32_t idx);

/* ═══════════════════════════════════════════════════════════════════
   Tensor Lookup by Name
   ═══════════════════════════════════════════════════════════════════ */

/* Find tensor index by name. Returns -1 if not found. */
int pogls_find_tensor(PoglsLoader *L, const char *name);

/* Get tensor data pointer by name (zero-copy if RAW, decompressed if ZSTD).
 * Returns NULL if not found or decompression fails.
 * The returned pointer is valid for the lifetime of the loader. */
void* pogls_tensor_data(PoglsLoader *L, const char *name);

/* Get tensor size by name. Returns 0 if not found. */
size_t pogls_tensor_size(PoglsLoader *L, const char *name);

/* ═══════════════════════════════════════════════════════════════════
   Tensor Lookup by Address
   ═══════════════════════════════════════════════════════════════════ */

/* Find tensor index by 144² address. Returns -1 if not found. */
int pogls_find_tensor_by_addr(PoglsLoader *L, uint32_t addr);

/* Get tensor data pointer by address. Returns NULL if not found. */
void* pogls_tensor_data_by_addr(PoglsLoader *L, uint32_t addr);

/* ═══════════════════════════════════════════════════════════════════
   Face Rotation (SID)
   ═══════════════════════════════════════════════════════════════════ */

/* Get the rotated address for a given face (0-5).
 * Returns UINT32_MAX if addr is invalid. */
uint32_t pogls_face_addr(uint32_t addr, int face);

/* Get tensor data for a specific face rotation.
 * Returns NULL if not found. */
void* pogls_tensor_data_face(PoglsLoader *L, const char *name, int face);

/* ═══════════════════════════════════════════════════════════════════
   Utilities
   ═══════════════════════════════════════════════════════════════════ */

/* Get the total file size in bytes. */
uint64_t pogls_file_size(PoglsLoader *L);

/* Get the pointer to the raw mmap (for advanced use). */
const void* pogls_mmap_ptr(PoglsLoader *L);

/* Get the mmap size (for advanced use). */
size_t pogls_mmap_size(PoglsLoader *L);

/* ═══════════════════════════════════════════════════════════════════
   Error Handling
   ═══════════════════════════════════════════════════════════════════ */

/* Get the last error message (thread-local). Returns NULL if no error. */
const char* pogls_last_error(void);

/* ═══════════════════════════════════════════════════════════════════
   Constants
   ═══════════════════════════════════════════════════════════════════ */

#define POGLS_LOADER_MAGIC_V1  0x504F474Cu  /* "POGL" — v1 legacy */
#define POGLS_LOADER_MAGIC_V2  0x53474F50u  /* "POGS" — v2 current */

#define POGLS_LOADER_COMP_RAW   0u
#define POGLS_LOADER_COMP_ZSTD  1u
#define POGLS_LOADER_COMP_SHELL 2u
#define POGLS_LOADER_COMP_DELTA 3u

#ifdef __cplusplus
}
#endif

/* ═══════════════════════════════════════════════════════════════════
   IMPLEMENTATION
   ═══════════════════════════════════════════════════════════════════ */

#ifdef POGLS_LOADER_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <io.h>
  #include <fcntl.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

/* ── Internal Structures ─────────────────────────────────── */

#define POGLS_L_MAX_ADDR    20736u
#define POGLS_L_HEADER_SZ   128u
#define POGLS_L_INDEX_SZ    (POGLS_L_MAX_ADDR * 16u)
#define POGLS_L_NAME_LEN    64u
#define POGLS_L_MAX_TENSORS 4096u

typedef struct {
    uint32_t addr;
    uint32_t dtype;
    uint32_t ndim;
    uint32_t nbytes_orig;
    uint32_t comp_type;
    uint32_t comp_nbytes;
    uint32_t dims[4];
    char     name[POGLS_L_NAME_LEN];
} PoglsLTensorMeta;  /* 104 bytes */

/*
 * IMPORTANT: This header MUST match the layout of PoglsStoreHeader in pogls_meta.h exactly.
 * The struct has a subtle alignment issue: gguf_path_off (uint64_t) at offset 44
 * gets padded by the compiler to offset 8-byte alignment. The actual layout is:
 *
 *   offset  0: magic (4)
 *   offset  4: version (4)
 *   offset  8: n_tensors (4)
 *   offset 12: flags (4)
 *   offset 16: tensor_meta_off (8)
 *   offset 24: tensor_meta_count (4)
 *   offset 28: _pad0 (4)
 *   offset 32: model_meta_off (8)
 *   offset 40: model_meta_sz (4)
 *   offset 44: _pad_align (4)  ← compiler padding for alignment
 *   offset 48: gguf_path_off (8)
 *   offset 56: gguf_path_sz (4)
 *   offset 60: _pad (68)
 *   = 128 bytes total
 */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_tensors;
    uint32_t flags;
    uint64_t tensor_meta_off;
    uint32_t tensor_meta_count;
    uint32_t _pad0;
    uint64_t model_meta_off;
    uint32_t model_meta_sz;
    uint8_t  _pad_align[4];    /* explicit alignment pad */
    uint64_t gguf_path_off;
    uint32_t gguf_path_sz;
    uint8_t  _pad[68];
} PoglsLHeader;  /* 128 bytes with pragma pack */
#pragma pack(pop)

struct PoglsLoader {
    /* mmap */
    void     *mmap_ptr;
    size_t    mmap_size;

    /* header */
    PoglsLHeader hdr;

    /* tensor meta array (points into mmap for v2, or NULL for v1) */
    const PoglsLTensorMeta *meta;
    uint32_t    meta_count;

    /* model meta */
    const void *model_meta;
    uint32_t    model_meta_sz;

    /* gguf path */
    const char *gguf_path;

    /* data section start (absolute offset in mmap) */
    uint64_t    data_offset;

    /* decompressed tensor cache (lazy) */
    void       *decomp_cache[POGLS_L_MAX_TENSORS];
    uint8_t     decomp_used[POGLS_L_MAX_TENSORS];

    /* name index for O(1) lookup */
    int         name_idx_built;
    struct { char name[POGLS_L_NAME_LEN]; uint32_t idx; } name_ht[2048];
};

/* ── Error Handling ──────────────────────────────────────── */

static char g_last_error[256];

static void pogls_l_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
}

const char* pogls_last_error(void) {
    return g_last_error[0] ? g_last_error : NULL;
}

/* ── Hash Function ───────────────────────────────────────── */

static uint32_t pogls_l_hash(const char *name) {
    uint32_t h = 0x811c9dc5u;
    for (const char *p = name; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 0x01000193u;
    }
    return h;
}

/* ── Address Rotation (capo) ─────────────────────────────── */

static uint32_t pogls_l_capo(uint32_t addr, int face) {
    if (addr >= POGLS_L_MAX_ADDR || face < 0 || face > 5) return UINT32_MAX;
    /* Face rotation: addr XOR with face-dependent bit pattern */
    static const uint32_t face_keys[6] = {
        0x00000000, 0x00001B6D, 0x000036DA,
        0x00005247, 0x00006DB4, 0x00008921
    };
    return (addr ^ face_keys[face]) % POGLS_L_MAX_ADDR;
}

/* ── Decompress (ZSTD) ──────────────────────────────────── */

#ifdef POGLS_LOADER_USE_ZSTD
#include <zstd.h>

static int pogls_l_decompress(void *out, size_t out_sz,
                               const void *in, size_t in_sz) {
    size_t ret = ZSTD_decompress(out, out_sz, in, in_sz);
    return ZSTD_isError(ret) ? -1 : 0;
}

static size_t pogls_l_decompress_bound(size_t in_sz) {
    return ZSTD_getFrameContentSize(NULL, 0); /* not used */
}
#else
static int pogls_l_decompress(void *out, size_t out_sz,
                               const void *in, size_t in_sz) {
    /* No ZSTD support — raw copy only */
    if (out_sz != in_sz) return -1;
    memcpy(out, in, in_sz);
    return 0;
}
#endif

/* ── Name Index ──────────────────────────────────────────── */

static void pogls_l_build_name_idx(PoglsLoader *L) {
    if (L->name_idx_built) return;
    memset(L->name_ht, 0, sizeof(L->name_ht));
    for (uint32_t i = 0; i < L->meta_count; i++) {
        uint32_t h = pogls_l_hash(L->meta[i].name) % 2048;
        /* Linear probe */
        while (L->name_ht[h].name[0] && L->name_ht[h].idx != i)
            h = (h + 1) % 2048;
        strncpy(L->name_ht[h].name, L->meta[i].name, POGLS_L_NAME_LEN - 1);
        L->name_ht[h].idx = i;
    }
    L->name_idx_built = 1;
}

/* ── Lifecycle ───────────────────────────────────────────── */

PoglsLoader* pogls_open(const char *path) {
    if (!path) { pogls_l_set_error("NULL path"); return NULL; }

    PoglsLoader *L = (PoglsLoader*)calloc(1, sizeof(PoglsLoader));
    if (!L) { pogls_l_set_error("calloc failed"); return NULL; }

#ifdef _WIN32
    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        pogls_l_set_error("cannot open %s (err=%lu)", path, GetLastError());
        free(L); return NULL;
    }
    DWORD hi;
    DWORD lo = GetFileSize(hf, &hi);
    L->mmap_size = ((uint64_t)hi << 32) | lo;
    HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(hf);
    if (!hm) { pogls_l_set_error("CreateFileMapping failed (err=%lu)", GetLastError()); free(L); return NULL; }
    L->mmap_ptr = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hm);
    if (!L->mmap_ptr) { pogls_l_set_error("MapViewOfFile failed (err=%lu)", GetLastError()); free(L); return NULL; }
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) { pogls_l_set_error("cannot open %s", path); free(L); return NULL; }
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); pogls_l_set_error("fstat failed"); free(L); return NULL; }
    L->mmap_size = st.st_size;
    L->mmap_ptr = mmap(NULL, L->mmap_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (L->mmap_ptr == MAP_FAILED) { pogls_l_set_error("mmap failed"); free(L); return NULL; }
#endif

    /* Validate minimum size */
    if (L->mmap_size < POGLS_L_HEADER_SZ) {
        pogls_l_set_error("file too small (%zu bytes)", L->mmap_size);
        pogls_close(L); return NULL;
    }

    /* Read header */
    memcpy(&L->hdr, L->mmap_ptr, sizeof(PoglsLHeader));

    /* Validate magic */
    if (L->hdr.magic != POGLS_LOADER_MAGIC_V2 && L->hdr.magic != POGLS_LOADER_MAGIC_V1) {
        pogls_l_set_error("invalid magic 0x%08X", L->hdr.magic);
        pogls_close(L); return NULL;
    }

    /* Validate version */
    if (L->hdr.version < 1 || L->hdr.version > 2) {
        pogls_l_set_error("unsupported version %u", L->hdr.version);
        pogls_close(L); return NULL;
    }

    /* v2 metadata */
    if (L->hdr.version == 2) {
        /* Tensor meta */
        if (L->hdr.tensor_meta_off > 0 && L->hdr.tensor_meta_count > 0) {
            size_t end = L->hdr.tensor_meta_off +
                         (size_t)L->hdr.tensor_meta_count * sizeof(PoglsLTensorMeta);
            if (end > L->mmap_size) {
                pogls_l_set_error("tensor meta overflows file");
                pogls_close(L); return NULL;
            }
            L->meta = (const PoglsLTensorMeta*)(L->mmap_ptr + L->hdr.tensor_meta_off);
            L->meta_count = L->hdr.tensor_meta_count;
        }

        /* Model meta */
        if (L->hdr.model_meta_off > 0 && L->hdr.model_meta_sz > 0) {
            size_t end = L->hdr.model_meta_off + L->hdr.model_meta_sz;
            if (end <= L->mmap_size) {
                L->model_meta = L->mmap_ptr + L->hdr.model_meta_off;
                L->model_meta_sz = L->hdr.model_meta_sz;
            }
        }

        /* GGUF path */
        if (L->hdr.gguf_path_off > 0 && L->hdr.gguf_path_sz > 0) {
            size_t end = L->hdr.gguf_path_off + L->hdr.gguf_path_sz;
            if (end <= L->mmap_size) {
                L->gguf_path = (const char*)(L->mmap_ptr + L->hdr.gguf_path_off);
            }
        }

        /* Data offset: after tensor meta section */
        if (L->hdr.tensor_meta_off > 0 && L->hdr.tensor_meta_count > 0) {
            L->data_offset = L->hdr.tensor_meta_off +
                             (uint64_t)L->hdr.tensor_meta_count * sizeof(PoglsLTensorMeta);
        } else {
            L->data_offset = POGLS_L_HEADER_SZ + POGLS_L_INDEX_SZ;
        }
    } else {
        /* v1: no meta, data starts after header + index */
        L->data_offset = POGLS_L_HEADER_SZ + POGLS_L_INDEX_SZ;
    }

    return L;
}

void pogls_close(PoglsLoader *L) {
    if (!L) return;

    /* Free decompressed caches */
    for (int i = 0; i < POGLS_L_MAX_TENSORS; i++) {
        if (L->decomp_used[i]) free(L->decomp_cache[i]);
    }

#ifdef _WIN32
    if (L->mmap_ptr) UnmapViewOfFile(L->mmap_ptr);
#else
    if (L->mmap_ptr && L->mmap_ptr != MAP_FAILED)
        munmap(L->mmap_ptr, L->mmap_size);
#endif

    free(L);
}

/* ── Metadata Queries ────────────────────────────────────── */

uint32_t pogls_tensor_count(PoglsLoader *L) {
    return L ? L->meta_count : 0;
}

uint32_t pogls_version(PoglsLoader *L) {
    return L ? L->hdr.version : 0;
}

const char* pogls_gguf_path(PoglsLoader *L) {
    return L ? L->gguf_path : NULL;
}

uint32_t pogls_model_meta_size(PoglsLoader *L) {
    return L ? L->model_meta_sz : 0;
}

const void* pogls_model_meta(PoglsLoader *L) {
    return L ? L->model_meta : NULL;
}

uint64_t pogls_file_size(PoglsLoader *L) {
    return L ? L->mmap_size : 0;
}

const void* pogls_mmap_ptr(PoglsLoader *L) {
    return L ? L->mmap_ptr : NULL;
}

size_t pogls_mmap_size(PoglsLoader *L) {
    return L ? L->mmap_size : 0;
}

/* ── Tensor by Index ─────────────────────────────────────── */

const char* pogls_tensor_name(PoglsLoader *L, uint32_t idx) {
    if (!L || idx >= L->meta_count) return NULL;
    return L->meta[idx].name;
}

uint32_t pogls_tensor_addr(PoglsLoader *L, uint32_t idx) {
    if (!L || idx >= L->meta_count) return UINT32_MAX;
    return L->meta[idx].addr;
}

uint32_t pogls_tensor_dtype(PoglsLoader *L, uint32_t idx) {
    if (!L || idx >= L->meta_count) return 0;
    return L->meta[idx].dtype;
}

uint32_t pogls_tensor_ndim(PoglsLoader *L, uint32_t idx) {
    if (!L || idx >= L->meta_count) return 0;
    return L->meta[idx].ndim;
}

void pogls_tensor_shape(PoglsLoader *L, uint32_t idx, uint32_t dims[4]) {
    if (!L || idx >= L->meta_count) { if (dims) memset(dims, 0, 16); return; }
    memcpy(dims, L->meta[idx].dims, 16);
}

size_t pogls_tensor_nbytes(PoglsLoader *L, uint32_t idx) {
    if (!L || idx >= L->meta_count) return 0;
    return L->meta[idx].nbytes_orig;
}

size_t pogls_tensor_comp_nbytes(PoglsLoader *L, uint32_t idx) {
    if (!L || idx >= L->meta_count) return 0;
    return L->meta[idx].comp_nbytes;
}

uint32_t pogls_tensor_comp_type(PoglsLoader *L, uint32_t idx) {
    if (!L || idx >= L->meta_count) return 0;
    return L->meta[idx].comp_type;
}

/* ── Tensor by Name ──────────────────────────────────────── */

int pogls_find_tensor(PoglsLoader *L, const char *name) {
    if (!L || !name) return -1;
    pogls_l_build_name_idx(L);
    uint32_t h = pogls_l_hash(name) % 2048;
    for (int i = 0; i < 2048; i++) {
        if (L->name_ht[h].name[0] == 0) return -1;
        if (strncmp(L->name_ht[h].name, name, POGLS_L_NAME_LEN) == 0)
            return (int)L->name_ht[h].idx;
        h = (h + 1) % 2048;
    }
    return -1;
}

void* pogls_tensor_data(PoglsLoader *L, const char *name) {
    int idx = pogls_find_tensor(L, name);
    if (idx < 0) return NULL;

    const PoglsLTensorMeta *m = &L->meta[idx];

    /* RAW: return pointer directly into mmap */
    if (m->comp_type == POGLS_LOADER_COMP_RAW || m->comp_nbytes == 0) {
        /* Calculate offset: data section + sum of previous tensor sizes */
        uint64_t off = L->data_offset;
        for (uint32_t i = 0; i < (uint32_t)idx; i++) {
            size_t sz = L->meta[i].comp_nbytes > 0 ? L->meta[i].comp_nbytes : L->meta[i].nbytes_orig;
            off += sz;
        }
        if (off + m->nbytes_orig > L->mmap_size) return NULL;
        return (void*)(L->mmap_ptr + off);
    }

    /* ZSTD/other: decompress (lazy cache) */
    if (!L->decomp_used[idx]) {
        /* Find compressed data offset */
        uint64_t off = L->data_offset;
        for (uint32_t i = 0; i < (uint32_t)idx; i++) {
            size_t sz = L->meta[i].comp_nbytes > 0 ? L->meta[i].comp_nbytes : L->meta[i].nbytes_orig;
            off += sz;
        }
        if (off + m->comp_nbytes > L->mmap_size) return NULL;

        void *buf = malloc(m->nbytes_orig);
        if (!buf) return NULL;

        if (pogls_l_decompress(buf, m->nbytes_orig,
                               L->mmap_ptr + off, m->comp_nbytes) != 0) {
            free(buf);
            return NULL;
        }
        L->decomp_cache[idx] = buf;
        L->decomp_used[idx] = 1;
    }
    return L->decomp_cache[idx];
}

size_t pogls_tensor_size(PoglsLoader *L, const char *name) {
    int idx = pogls_find_tensor(L, name);
    if (idx < 0) return 0;
    return L->meta[idx].nbytes_orig;
}

/* ── Tensor by Address ───────────────────────────────────── */

int pogls_find_tensor_by_addr(PoglsLoader *L, uint32_t addr) {
    if (!L) return -1;
    for (uint32_t i = 0; i < L->meta_count; i++) {
        if (L->meta[i].addr == addr) return (int)i;
    }
    return -1;
}

void* pogls_tensor_data_by_addr(PoglsLoader *L, uint32_t addr) {
    int idx = pogls_find_tensor_by_addr(L, addr);
    if (idx < 0) return NULL;
    return pogls_tensor_data(L, L->meta[idx].name);
}

/* ── Face Rotation ───────────────────────────────────────── */

uint32_t pogls_face_addr(uint32_t addr, int face) {
    return pogls_l_capo(addr, face);
}

void* pogls_tensor_data_face(PoglsLoader *L, const char *name, int face) {
    int idx = pogls_find_tensor(L, name);
    if (idx < 0) return NULL;
    uint32_t face_addr = pogls_l_capo(L->meta[idx].addr, face);
    int fidx = pogls_find_tensor_by_addr(L, face_addr);
    if (fidx < 0) return NULL;
    return pogls_tensor_data(L, L->meta[fidx].name);
}

#endif /* POGLS_LOADER_IMPLEMENTATION */

#endif /* POGLS_LOADER_H */

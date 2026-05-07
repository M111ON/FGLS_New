/*
 * geo_vault.h — GeoVault: Passive Mount-on-Access File Container
 * ═══════════════════════════════════════════════════════════════
 *
 * Concept:
 *   folder/files → packed into Tring residual zone (6912 slots)
 *   each slot = 3 bytes (R,G,B pixel)
 *   stored as raw pixel buffer → compress with zstd
 *   output: .geovault file
 *
 *   Mount = open file, compute tring_addr(file_id, chunk) → seek → read
 *   No full decompress. No index scan. O(1) per chunk.
 *
 * Geometry chain:
 *   5-tetra compound × 12 origins → 144 configs → 12³ = 1728
 *   × 2 invert (6 opposite pentagon pairs)        = 3456  (= frustum pre-fold)
 *   × 6 frustum → virtual apex convergence        = 20736 (hilbert)
 *   + 6912 residual (edge skip)                   = 27648 (sync boundary)
 *   6912 = 48×48×3 = image tile space             ← FILE DATA LIVES HERE
 *
 * Layout:
 *   [HEADER 64B]
 *   [FILE TABLE  N×32B]  — one entry per file
 *   [PIXEL BUFFER  6912×3B = 20736B per frame]    — zstd compressed
 *
 * No malloc in hot path. No float. Header-only for seek.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef GEO_VAULT_H
#define GEO_VAULT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── Sacred constants (DO NOT CHANGE) ───────────────────────────── */
#define GV_COMPOUNDS      12u
#define GV_PATTERNS      144u    /* 12² = self-similar replication   */
#define GV_TRING        1728u    /* 12³                              */
#define GV_FULL         3456u    /* ×2 invert (6 opposite pairs)     */
#define GV_HILBERT     20736u    /* ×6 frustum → virtual apex        */
#define GV_RESIDUAL     6912u    /* edge skip = file data zone       */
#define GV_SYNC        27648u    /* convergence boundary             */
#define GV_TILE_DIM       48u    /* 1536/32 tiles per axis           */
#define GV_CHANNELS        3u    /* R,G,B                            */
#define GV_CHUNK_BYTES     3u    /* bytes per slot (one pixel)       */
#define GV_FRAME_BYTES (GV_RESIDUAL * GV_CHUNK_BYTES)  /* 20736B    */

/* Virtual Apex = mathematical convergence, not stored */
#define GV_VIRTUAL_APEX_ID  0xFFFFFFFFu

/* ── Header (64 bytes, fixed) ────────────────────────────────────── */
#define GV_MAGIC       0x47454F56u   /* "GEOV"                       */
#define GV_VERSION     0x0001u       /* v1: single zstd block        */
#define GV_VERSION_2   0x0002u       /* v2: per-frame zstd + seek table */

/* flags */
#define GVF_COMPRESSED  0x01u
#define GVF_MULTIFRAME  0x02u
#define GVF_PER_FRAME   0x04u        /* v2: per-frame compression    */

typedef struct __attribute__((packed)) {
    uint32_t magic;          /* 0x47454F56 "GEOV"                    */
    uint16_t version;        /* GV_VERSION or GV_VERSION_2           */
    uint16_t flags;          /* GVF_* bitmask                        */
    uint32_t n_files;        /* number of files packed               */
    uint32_t n_frames;       /* pixel buffer frames needed           */
    uint64_t total_raw;      /* original bytes (all files)           */
    uint64_t compressed_sz;  /* total compressed size (all frames)   */
    uint32_t frame_bytes;    /* GV_FRAME_BYTES (sanity check)        */
    uint8_t  folder_sha[20]; /* SHA1 of all file data                */
    uint8_t  _pad[8];        /* pad to 64B                           */
} GeoVaultHeader;            /* sizeof = 64 */

_Static_assert(sizeof(GeoVaultHeader) == 64, "header must be 64B");

/* ── Frame Seek Table Entry (16 bytes) ───────────────────────────── */
/* v2 only: one entry per frame, stored after file table             */
typedef struct __attribute__((packed)) {
    uint64_t offset;         /* byte offset of this frame's zstd data in file */
    uint32_t compressed_sz;  /* compressed size of this frame        */
    uint32_t _pad;           /* reserved                             */
} GeoVaultFrameEntry;        /* sizeof = 16 */

_Static_assert(sizeof(GeoVaultFrameEntry) == 16, "frame entry must be 16B");

/* ── File Table Entry (32 bytes, fixed) ─────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t name_hash;      /* FNV1a of relative path               */
    uint32_t raw_size;       /* original file size in bytes          */
    uint32_t slot_start;     /* first residual slot (0..6911)        */
    uint32_t slot_count;     /* number of slots used (ceil(size/3))  */
    uint32_t frame_start;    /* which frame slot_start is in         */
    uint8_t  file_sha[12];   /* first 12B of SHA1 (integrity check)  */
} GeoVaultFileEntry;         /* sizeof = 32 */

_Static_assert(sizeof(GeoVaultFileEntry) == 32, "file entry must be 32B");

/* ── Tring Address (seek key) ────────────────────────────────────── */
typedef struct {
    uint16_t slot;           /* 0..6911 residual zone slot           */
    uint8_t  compound;       /* 0..11  pentagon face                 */
    uint8_t  pattern;        /* 0..143 self-similar config           */
    uint8_t  invert;         /* 0..1   opposite pair                 */
    uint8_t  ch;             /* 0..2   RGB channel                   */
    /* geometric routing fields */
    uint8_t  trit;           /* slot % 27  — 3³ index                */
    uint8_t  spoke;          /* slot % 6   — frustum direction       */
    uint8_t  coset;          /* slot % 9   — giant cube              */
    uint8_t  fibo;           /* slot % 144 — pattern position        */
} GeoVaultAddr;

/* ════════════════════════════════════════════════════════════════
 * TRING ENCODE: (file_slot_index) → GeoVaultAddr
 * file_slot_index = absolute slot in residual zone (0..6911)
 * ════════════════════════════════════════════════════════════════ */
static inline GeoVaultAddr gv_addr(uint32_t slot) {
    /* decompose slot into tring geometry */
    uint32_t compound = slot / (GV_PATTERNS * 4u);   /* 0..11        */
    uint32_t rem      = slot % (GV_PATTERNS * 4u);
    uint32_t pattern  = rem  / 4u;                   /* 0..143       */
    uint32_t rem2     = rem  % 4u;
    uint32_t invert   = rem2 / 2u;                   /* 0..1         */
    uint32_t ch       = rem2 % 2u;                   /* 0..1 (fine)  */

    GeoVaultAddr a;
    a.slot     = (uint16_t)slot;
    a.compound = (uint8_t)compound;
    a.pattern  = (uint8_t)pattern;
    a.invert   = (uint8_t)invert;
    a.ch       = (uint8_t)ch;
    a.trit     = (uint8_t)(slot % 27u);
    a.spoke    = (uint8_t)(slot % 6u);
    a.coset    = (uint8_t)(slot % 9u);
    a.fibo     = (uint8_t)(slot % 144u);
    return a;
}

/* ════════════════════════════════════════════════════════════════
 * SEEK: slot → byte offset in uncompressed pixel buffer
 * offset = frame * GV_FRAME_BYTES + slot_in_frame * GV_CHUNK_BYTES
 * ════════════════════════════════════════════════════════════════ */
static inline uint64_t gv_seek(uint32_t frame, uint32_t slot_in_frame) {
    return (uint64_t)frame * GV_FRAME_BYTES
         + (uint64_t)slot_in_frame * GV_CHUNK_BYTES;
}

/* ════════════════════════════════════════════════════════════════
 * PACK: raw file bytes → slot(s) in pixel buffer
 * Each slot holds GV_CHUNK_BYTES (3) bytes.
 * Last slot zero-padded.
 * Returns: number of slots written
 * ════════════════════════════════════════════════════════════════ */
static inline uint32_t gv_slots_needed(uint32_t raw_bytes) {
    return (raw_bytes + GV_CHUNK_BYTES - 1u) / GV_CHUNK_BYTES;
}

static inline void gv_pack_chunk(const uint8_t *src, uint32_t src_len,
                                  uint32_t slot_idx,
                                  uint8_t *frame_buf) {
    /* write up to 3 bytes into frame_buf at slot position */
    uint32_t off  = slot_idx * GV_CHUNK_BYTES;
    uint32_t left = src_len > GV_CHUNK_BYTES ? GV_CHUNK_BYTES : src_len;
    memcpy(frame_buf + off, src, left);
    if (left < GV_CHUNK_BYTES)
        memset(frame_buf + off + left, 0, GV_CHUNK_BYTES - left);
}

/* ════════════════════════════════════════════════════════════════
 * UNPACK: slot(s) → raw file bytes
 * ════════════════════════════════════════════════════════════════ */
static inline void gv_unpack_chunk(const uint8_t *frame_buf,
                                    uint32_t slot_idx,
                                    uint8_t *dst, uint32_t dst_len) {
    uint32_t off  = slot_idx * GV_CHUNK_BYTES;
    uint32_t take = dst_len > GV_CHUNK_BYTES ? GV_CHUNK_BYTES : dst_len;
    memcpy(dst, frame_buf + off, take);
}

/* ════════════════════════════════════════════════════════════════
 * VIRTUAL APEX ROUTE
 * Given file_id and chunk_idx → compute which spoke (0..5) handles it
 * This is the "routing" through the 6-frustum virtual apex system
 * spoke = (file_id * 7 + chunk_idx) % 6  — deterministic, no table
 * ════════════════════════════════════════════════════════════════ */
static inline uint8_t gv_apex_spoke(uint32_t file_id, uint32_t chunk_idx) {
    return (uint8_t)((file_id * 7u + chunk_idx) % 6u);
}

/* ════════════════════════════════════════════════════════════════
 * VERIFY: roundtrip slot → addr → slot
 * Returns 0 = perfect, >0 = errors
 * ════════════════════════════════════════════════════════════════ */
static inline uint32_t gv_verify_roundtrip(void) {
    uint32_t errors = 0;
    for (uint32_t slot = 0; slot < GV_RESIDUAL; slot++) {
        GeoVaultAddr a = gv_addr(slot);
        /* reconstruct slot from fields */
        uint32_t reconstructed = (uint32_t)a.compound * (GV_PATTERNS * 4u)
                               + (uint32_t)a.pattern  * 4u
                               + (uint32_t)a.invert   * 2u
                               + (uint32_t)a.ch;
        if (reconstructed != slot) errors++;
        if (a.compound >= GV_COMPOUNDS)  errors++;
        if (a.pattern  >= GV_PATTERNS)   errors++;
        if (a.invert   >= 2u)            errors++;
        if (a.trit     >= 27u)           errors++;
        if (a.spoke    >= 6u)            errors++;
        if (a.coset    >= 9u)            errors++;
        if (a.fibo     >= GV_PATTERNS)   errors++;
    }
    return errors;
}

/* ════════════════════════════════════════════════════════════════
 * FNV1a hash for file names
 * ════════════════════════════════════════════════════════════════ */
static inline uint32_t gv_fnv1a(const char *s) {
    uint32_t h = 0x811c9dc5u;
    while (*s) { h ^= (uint8_t)*s++; h *= 0x01000193u; }
    return h;
}

#endif /* GEO_VAULT_H */

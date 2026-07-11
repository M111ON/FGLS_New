/*
 * geofield_cover.h — GeoField Combined Cover (56B, fits 64B cache line)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Combines two complementary headers at the start of every .geof file:
 *
 *   GeoFieldHeader     24B  — virtual identity
 *     "นี่คือ geometry format อะไร"
 *     shape + codec + seed → reconstructible without reading data
 *
 *   GeoFieldFileHeader 32B  — physical manifest
 *     "physical data ที่เก็บอยู่มีอะไรบ้าง"
 *     gp_level + n_blocks + orig_size + xxh64
 *
 *   GeoFieldCover      56B  — single entry point for all I/O
 *     อ่าน/เขียนได้ใน 1 cache miss (64B line, เหลือ 8B)
 *
 * File layout:
 *   [GeoFieldCover  56B]  ← อ่าน 1 read() ได้ทั้งหมด
 *   [pad            8B]   ← align ถัดไปที่ 64B boundary (optional)
 *   [FrustumBlocks  n×4896B]
 *
 * Performance note:
 *   Cover อยู่ cache line เดียว → ไม่มี overhead เพิ่มจากการ combine
 *   56B vs 32B (เดิม) = +24B per file open = negligible
 *   การอ่าน: fread(&cover, 56, 1, f) = 1 syscall เหมือนเดิม
 *
 * Include order:
 *   geofield_header.h  (GeoFieldHeader 24B)
 *   geo_field_core.h   (GeoFieldFileHeader 32B)
 *   geofield_cover.h   ← this file
 *
 * No float. No heap. No global state.
 * ══════════════════════════════════════════════════════════════════════
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "geofield_header.h"

/* ── Re-declare FileHeader locally (avoids circular include) ─── */
/*
 * Mirror of GeoFieldFileHeader from geo_field_core.h.
 * Keep in sync manually — only fields used for I/O.
 */
#ifndef GEOF_FILE_HEADER_DEFINED
#define GEOF_FILE_HEADER_DEFINED

#define GF_COVER_FILE_VERSION 1u

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];     /* "GCOV" — cover-aware file (not "GEOF")     */
    uint8_t  version;      /* GF_COVER_FILE_VERSION                      */
    uint8_t  gp_level;     /* GP subdivision level (1..8)                */
    uint8_t  _pad1[2];     /* reserved                                   */
    uint32_t n_blocks;     /* number of FrustumBlocks                    */
    uint64_t orig_size;    /* original data size in bytes                */
    uint64_t digest;       /* xxh64 of original data                     */
    uint8_t  _pad2[4];     /* pad to 32B                                 */
} GeoFieldManifest;        /* 32B physical manifest                      */

#endif /* GEOF_FILE_HEADER_DEFINED */

/* ── GeoFieldCover — 56B combined ───────────────────────────── */
typedef struct {
    GeoFieldHeader   geo;   /* 24B — virtual identity (shape+codec+seed) */
    GeoFieldManifest file;  /* 32B — physical manifest (blocks+digest)   */
} GeoFieldCover;            /* 56B — 1 cache line read                   */

_Static_assert(sizeof(GeoFieldHeader)   == 24, "geo header must be 24B");
_Static_assert(sizeof(GeoFieldManifest) == 32, "manifest must be 32B");
_Static_assert(sizeof(GeoFieldCover)    == 56, "cover must be 56B");

/* Magic for manifest — distinct from legacy "GEOF" */
#define GF_MANIFEST_MAGIC "GCOV"

/* ── Init ────────────────────────────────────────────────────── */

/*
 * geof_cover_init — populate both headers, compute digests
 *
 * geo params:  shape_id, codec_id, flags, floor_count, seed
 * file params: gp_level, n_blocks, orig_size, data_digest (xxh64)
 */
static inline void geof_cover_init(GeoFieldCover  *c,
                                    /* geo */
                                    uint8_t  shape_id,
                                    uint8_t  codec_id,
                                    uint8_t  flags,
                                    uint16_t floor_count,
                                    uint32_t seed,
                                    /* file */
                                    uint8_t  gp_level,
                                    uint32_t n_blocks,
                                    uint64_t orig_size,
                                    uint64_t data_digest)
{
    memset(c, 0, sizeof(*c));

    /* geo header */
    geof_header_init(&c->geo, shape_id, codec_id, flags, floor_count, seed);

    /* manifest */
    memcpy(c->file.magic, GF_MANIFEST_MAGIC, 4);
    c->file.version   = GF_COVER_FILE_VERSION;
    c->file.gp_level  = (gp_level < 1) ? 1 :
                        (gp_level > 8)  ? 8 : gp_level;
    c->file.n_blocks  = n_blocks;
    c->file.orig_size = orig_size;
    c->file.digest    = data_digest;
}

/* ── Validate ────────────────────────────────────────────────── */

/*
 * geof_cover_valid — check both magic bytes and geo header digest
 * Returns 1 if valid, 0 if corrupt.
 */
static inline int geof_cover_valid(const GeoFieldCover *c)
{
    if (!geof_header_valid(&c->geo)) return 0;
    if (memcmp(c->file.magic, GF_MANIFEST_MAGIC, 4) != 0) return 0;
    if (c->file.version != GF_COVER_FILE_VERSION) return 0;
    return 1;
}

/* ── I/O helpers ─────────────────────────────────────────────── */

/*
 * geof_cover_write — write 56B cover to FILE* at current position
 * Returns 1 on success, 0 on error.
 */
static inline int geof_cover_write(const GeoFieldCover *c, FILE *f)
{
    return (fwrite(c, sizeof(*c), 1, f) == 1) ? 1 : 0;
}

/*
 * geof_cover_read — read 56B cover from FILE* at current position
 * Returns 1 on success + valid, 0 on error or invalid.
 */
static inline int geof_cover_read(GeoFieldCover *c, FILE *f)
{
    if (fread(c, sizeof(*c), 1, f) != 1) return 0;
    return geof_cover_valid(c);
}

/* ── Convenience accessors ───────────────────────────────────── */

static inline uint32_t geof_cover_seed(const GeoFieldCover *c) {
    return c->geo.seed;
}
static inline uint8_t geof_cover_shape(const GeoFieldCover *c) {
    return c->geo.shape_id;
}
static inline uint8_t geof_cover_gp_level(const GeoFieldCover *c) {
    return c->file.gp_level;
}
static inline uint64_t geof_cover_orig_size(const GeoFieldCover *c) {
    return c->file.orig_size;
}

/*
 * geof_cover_seed_space — virtual address space size
 * = floor_count × GEOF_CONVERGENCE (≤ GEOF_ALIGN_POINT = 20736)
 */
static inline uint32_t geof_cover_seed_space(const GeoFieldCover *c) {
    return geof_header_seed_space(&c->geo);
}

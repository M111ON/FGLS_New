/*
 * geofield_dramtile.h — DRamTile-backed GeoField Pipeline
 *
 * Replaces heap alloc per-segment with dt_put → mmap pointer.
 * Zero-copy: segment data lives in mmap, processed via pointer.
 *
 * Compile: include after dramtile_store.h in the DLL or standalone tool.
 */

#pragma once
#ifndef GEOFIELD_DRAMTILE_H
#define GEOFIELD_DRAMTILE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "dramtile_store.h"

/* ── Pipeline DRamTile Context ──────────────────────────── */

typedef struct {
    DRamTileStore store;
    size_t        capacity;       /* total mmap bytes */
    int           initialized;
    uint32_t      n_stored;       /* segments stored */
    size_t        total_bytes;    /* total bytes stored */
    double        structure_time_ms; /* last structure time */
} GeoFieldDT;

/* ── Segment naming ─────────────────────────────────────── */

static inline void gfdt_seg_name(char *buf, size_t bufsz, uint32_t idx) {
    snprintf(buf, bufsz, "gf.seg.%u", idx);
}

static inline void gfdt_block_name(char *buf, size_t bufsz, uint32_t seg, uint32_t blk) {
    snprintf(buf, bufsz, "gf.blk.%u.%u", seg, blk);
}

static inline void gfdt_lc_name(char *buf, size_t bufsz, uint32_t idx) {
    snprintf(buf, bufsz, "gf.lc.%u", idx);
}

static inline void gfdt_cube_name(char *buf, size_t bufsz, uint32_t depth, uint32_t idx) {
    snprintf(buf, bufsz, "gf.cube.%u.%u", depth, idx);
}

/* ── Init / Destroy ─────────────────────────────────────── */

static inline int gfdt_init(GeoFieldDT *ctx, size_t capacity) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->capacity = capacity;
    int rc = dt_store_init(&ctx->store, capacity);
    ctx->initialized = (rc == 0);
    return rc;
}

static inline void gfdt_destroy(GeoFieldDT *ctx) {
    if (ctx->initialized) {
        dt_store_destroy(&ctx->store);
        ctx->initialized = 0;
    }
}

/* ── Store operations ───────────────────────────────────── */

/* Store a segment's raw data into DRamTile.
 * Returns pointer into mmap (zero-copy), or NULL on failure. */
static inline uint8_t *gfdt_store_segment(
    GeoFieldDT *ctx, uint32_t seg_idx,
    const uint8_t *data, size_t size)
{
    char name[32];
    gfdt_seg_name(name, sizeof(name), seg_idx);

    uint8_t *ptr = dt_put(&ctx->store, name, data, size);
    if (ptr) {
        ctx->n_stored++;
        ctx->total_bytes += size;
    }
    return ptr;
}

/* Retrieve a segment's data pointer (O(1) lookup). */
static inline uint8_t *gfdt_get_segment(GeoFieldDT *ctx, uint32_t seg_idx) {
    char name[32];
    gfdt_seg_name(name, sizeof(name), seg_idx);
    return dt_get(&ctx->store, name);
}

/* Get segment size. */
static inline size_t gfdt_segment_size(GeoFieldDT *ctx, uint32_t seg_idx) {
    char name[32];
    gfdt_seg_name(name, sizeof(name), seg_idx);
    return dt_get_size(&ctx->store, name);
}

/* Store a LetterCube state. */
static inline uint8_t *gfdt_store_lc(
    GeoFieldDT *ctx, uint32_t seg_idx,
    const uint8_t *lc_data, size_t lc_size)
{
    char name[32];
    gfdt_lc_name(name, sizeof(name), seg_idx);
    return dt_put(&ctx->store, name, lc_data, lc_size);
}

/* Retrieve a LetterCube state. */
static inline uint8_t *gfdt_get_lc(GeoFieldDT *ctx, uint32_t seg_idx) {
    char name[32];
    gfdt_lc_name(name, sizeof(name), seg_idx);
    return dt_get(&ctx->store, name);
}

/* Store a CubeCtx. */
static inline uint8_t *gfdt_store_cube(
    GeoFieldDT *ctx, uint32_t depth, uint32_t cube_idx,
    const uint8_t *cube_data, size_t cube_size)
{
    char name[48];
    gfdt_cube_name(name, sizeof(name), depth, cube_idx);
    return dt_put(&ctx->store, name, cube_data, cube_size);
}

/* Retrieve a CubeCtx. */
static inline uint8_t *gfdt_get_cube(GeoFieldDT *ctx, uint32_t depth, uint32_t cube_idx) {
    char name[48];
    gfdt_cube_name(name, sizeof(name), depth, cube_idx);
    return dt_get(&ctx->store, name);
}

/* ── Stats ──────────────────────────────────────────────── */

static inline void gfdt_print_stats(const GeoFieldDT *ctx) {
    printf("DRamTile: %u segments, %zu bytes stored, capacity %zu (%.1f%% used)\n",
           ctx->n_stored, ctx->total_bytes, ctx->capacity,
           ctx->capacity > 0 ? 100.0 * ctx->total_bytes / ctx->capacity : 0);
    printf("  mmap base: %p, free slots: %d\n",
           (void *)ctx->store.base, ctx->store.free_count);
}

#endif /* GEOFIELD_DRAMTILE_H */

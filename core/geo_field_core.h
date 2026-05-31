/*
 * geo_field_core.h — Unified Geometric Encode/Decode Field
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Integrates:
 *   GpSphere (Goldberg polyhedron)  — tile_id × dim coordinate system
 *   FrustumBlock (4896B container)  — geometric storage backend
 *   Metatron routing                — shape dimension access / navigation
 *   Trit decomposition              — address encoding
 *   Flow chunking                   — content-driven boundary detection
 *
 * Complete loop:
 *   encode: file → chunks → GpAddr → FrustumBlock → serialize
 *   decode: serialize → FrustumBlock → GpAddr → chunks → file
 *
 * Scale (zoom in/out):  change gp_level (1..8) → more/fewer tiles
 *   → more tiles = finer subdivision = "zoomed in"
 *   → fewer tiles = coarser subdivision = "zoomed out"
 *
 * Shape dimension access:  navigate between pentagon faces (0..11)
 *   via Metatron's 4 route types:
 *     ORBITAL — stay on same face, slot+1
 *     CHIRAL  — jump to opposite face (face ↔ face+6)
 *     CROSS   — inter-ring non-chiral
 *     HUB     — any face via center
 *
 * All headers are local — just -I. or copy geofield/ to your project.
 *
 * No malloc in hot path. No float. All O(1) operations.
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef GEO_FIELD_CORE_H
#define GEO_FIELD_CORE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Dependencies ─────────────────────────────────────────────────── */

/* Core geometric primitives */
#include "tring.h"
#include "pogls_fold.h"

/* FrustumBlock storage backend (Metatron) */
#include "frustum_layout_v2.h"
#include "frustum_trit.h"
#include "frustum_slot64.h"
#include "frustum_gcfs.h"

/* Goldberg sphere coordinate system (ZIP source) */
#include "geo_goldberg_sphere.h"
#include "geo_gp_frustum_bridge.h"

/* Metatron routing — shape dimension access */
#include "geo_metatron_route.h"
#include "geo_temporal_lut.h"

/* Skeleton index — O(1) addr→geometry lookup */
#include "skeleton_index.h"

/* Flow chunker — content-driven boundaries */
#include "geo_flow_chunker_v8.h"

/* Fabric wire — Switch Gate, classification, fence */
#include "fabric_wire.h"

/* Goldberg shutter — ring confidence system */
#include "goldberg_shutter.h"

/* ═══════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════ */

#define GF_CHUNK_SZ         64u        /* Diamond lens = 1 cache line    */
#define GF_BLOCK_DATA_SZ    FGLS_DATA_BYTES  /* 3456 = 54×64B           */
#define GF_BLOCK_TOTAL_SZ   FGLS_TOTAL_BYTES /* 4896 = full FrustumBlock */
#define GF_CHUNKS_PER_BLOCK (GF_BLOCK_DATA_SZ / GF_CHUNK_SZ)  /* 54    */

/* ═══════════════════════════════════════════════════════════════════════
   GeoField — main context
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t       gp_level;          /* 1..8 — subdivision depth        */
    uint32_t      face_max;          /* gp_face_count(gp_level) cached  */
    uint32_t      n_blocks;          /* total FrustumBlocks used        */
    FrustumBlock *blocks;            /* array of FrustumBlocks          */
    Tring         tring;             /* shared Tring for GpSphere lens  */
    GpSphere      sphere;            /* coordinate system               */
} GeoField;

/* ── Init/Free ────────────────────────────────────────────────────── */

static inline int geo_field_init(GeoField *gf, uint8_t gp_level,
                                  uint32_t n_blocks)
{
    memset(gf, 0, sizeof(*gf));
    gf->gp_level = (gp_level < 1) ? 1 : (gp_level > GP_MAX_LEVEL ? GP_MAX_LEVEL : gp_level);
    gf->face_max = gp_face_count(gf->gp_level);
    gf->n_blocks = n_blocks;

    /* Allocate FrustumBlocks */
    gf->blocks = (FrustumBlock *)calloc(n_blocks, sizeof(FrustumBlock));
    if (!gf->blocks) return -1;

    /* Init Tring (len = face_max × GP_MAX_DIM for worst case) */
    uint32_t tring_cap = gf->face_max * GP_MAX_DIM;
    if (tring_cap < 1024) tring_cap = 1024;
    if (tring_init(&gf->tring, tring_cap) != 0) {
        free(gf->blocks);
        gf->blocks = NULL;
        return -1;
    }

    /* Init GpSphere */
    gp_sphere_init(&gf->sphere, &gf->tring, gf->gp_level);

    return 0;
}

static inline void geo_field_free(GeoField *gf)
{
    if (gf->blocks) {
        free(gf->blocks);
        gf->blocks = NULL;
    }
    tring_destroy(&gf->tring);
    memset(gf, 0, sizeof(*gf));
}

/* ═══════════════════════════════════════════════════════════════════════
   ENCODE — data → FrustumBlocks
   ═══════════════════════════════════════════════════════════════════════
 *
 * Maps each 64B chunk of input data to a GpAddr {tile_id, dim}:
 *   tile_id = chunk_idx % face_max
 *   dim     = (chunk_idx / face_max) & 0x7F
 *
 * Then writes through gp_blk_write() into the appropriate FrustumBlock.
 * Zone boundaries (pentagon tiles) trigger skel_enc_zone_reset().
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t  total_chunks;     /* total 64B chunks processed           */
    uint64_t  total_blocks;     /* total FrustumBlocks written          */
    uint64_t  zone_resets;      /* pentagon zone boundary crossings     */
    uint32_t  skel_hits[6];     /* skeleton strategy histogram          */
    uint32_t  spoke_imbalance;  /* TRing spoke distribution imbalance   */
} GeoFieldEncodeStats;

/*
 * Encode a single 64B chunk into the GeoField.
 * Returns the block index written to, or -1 on error.
 */
static inline int geo_field_encode_chunk(GeoField         *gf,
                                          uint64_t          chunk_idx,
                                          const uint8_t     chunk[64],
                                          SkelEncCtx       *skel,
                                          GeoFieldEncodeStats *stats)
{
    if (!gf || !gf->blocks || !chunk) return -1;

    /* Map chunk_idx → GpAddr */
    GpAddr a = gp_chunk_to_addr(gf->gp_level, chunk_idx);

    /* SEAM 3 — TRing walk enc (parallel path, raster coords unchanged)
     * tring_enc : dispatch position 0..719 via stride-37 walk
     * tring_spk : spoke 0..5  → B2 secondary drain slot
     */
    uint16_t tring_enc = tring_walk_enc(a.tile_id);
    uint8_t  tring_spk = tring_walk_spoke(a.tile_id);
    (void)tring_enc;   /* available for future dispatch; not gating store yet */

    /*
     * Block addressing: each dim layer needs face_max tiles, but each
     * FrustumBlock holds only 54 DiamondBlocks.  So we chunk tiles into
     * groups of 54 per dim layer:
     *   block_idx = dim * tiles_per_layer_blocks + tile_group
     *   slot      = tile_id % 54  (diamond slot within block)
     */
    uint32_t tiles_per_layer = gf->face_max;
    uint32_t blocks_per_layer = (tiles_per_layer + GF_CHUNKS_PER_BLOCK - 1)
                                / GF_CHUNKS_PER_BLOCK;
    uint32_t tile_group = a.tile_id / GF_CHUNKS_PER_BLOCK;
    uint32_t block_idx = (uint32_t)a.dim * blocks_per_layer + tile_group;
    if (block_idx >= gf->n_blocks) return -1;

    FrustumBlock *blk = &gf->blocks[block_idx];

    /* Zone boundary check — pentagon tile = reset skeleton context */
    if (gp_is_zone_boundary(a.tile_id)) {
        skel_enc_zone_reset(skel);
        stats->zone_resets++;
    }

    /*
     * Write into FrustumBlock at the correct diamond slot.
     * We write directly into blk->data at offset (slot * 64) and mark
     * the drain active so gp_blk_read can find it.
     */
    uint8_t diamond_slot = (uint8_t)(a.tile_id % FGLS_DIAMOND_COUNT);
    memcpy(blk->data + (size_t)diamond_slot * FGLS_DIAMOND_BYTES, chunk, FGLS_DIAMOND_BYTES);

    /* Primary drain — pentagon anchor (original path) */
    uint8_t drain_idx = gp_is_pentagon(a.tile_id)
                      ? (uint8_t)a.tile_id
                      : gp_tile_to_pent(gf->gp_level, a.tile_id);
    blk->meta.drain_state[drain_idx % FGLS_DRAIN_COUNT] |= 0x01u;

    /* B2 — secondary drain via TRing spoke (parallel, non-overlapping guard)
     * tring_spk ∈ [0..5]; only write if differs from primary drain_idx
     * to avoid double-marking the same slot with different semantics.
     */
    uint8_t sec_drain = tring_spk % FGLS_DRAIN_COUNT;
    if (sec_drain != (drain_idx % FGLS_DRAIN_COUNT))
        blk->meta.drain_state[sec_drain] |= 0x01u;

    /* Mark shadow zone occupied (primary anchor drives shadow) */
    uint8_t shadow_idx = (uint8_t)((drain_idx * 2u + a.dim) & 0x1Fu);
    blk->meta.shadow_state[shadow_idx] |= 0x01u;

    /* Skeleton encode decision (for stats) */
    SkeletonIdx sk;
    SkelStrategy ss = skel_encode_chunk(skel, chunk, &sk, chunk_idx * GF_CHUNK_SZ);
    stats->skel_hits[ss]++;
    stats->total_chunks++;

    return (int)block_idx;
}

/*
 * Encode an entire file/array into GeoField.
 * data     : input bytes
 * data_sz  : input size in bytes
 * Returns 0 on success, -1 on error.
 */
static inline int geo_field_encode(GeoField           *gf,
                                    const uint8_t      *data,
                                    size_t              data_sz,
                                    GeoFieldEncodeStats *stats)
{
    if (!gf || !data || !stats) return -1;

    memset(stats, 0, sizeof(*stats));

    uint64_t n_chunks = (uint64_t)((data_sz + GF_CHUNK_SZ - 1) / GF_CHUNK_SZ);

    SkelEncCtx skel;
    skel_enc_init(&skel);

    for (uint64_t ci = 0; ci < n_chunks; ci++) {
        size_t offset = (size_t)(ci * GF_CHUNK_SZ);
        size_t remain = (offset < data_sz) ? data_sz - offset : 0;
        uint8_t chunk[GF_CHUNK_SZ] = {0};
        size_t cp = (remain < GF_CHUNK_SZ) ? remain : GF_CHUNK_SZ;
        if (cp > 0) memcpy(chunk, data + offset, cp);

        int ret = geo_field_encode_chunk(gf, ci, chunk, &skel, stats);
        if (ret < 0) return -1;
    }

    /* TRing spoke imbalance across all encoded chunks */
    stats->spoke_imbalance = tring_walk_spoke_imbalance((uint32_t)n_chunks);

    stats->total_blocks = gf->n_blocks;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   DECODE — FrustumBlocks → data
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t chunks_decoded;    /* successful reads                     */
    uint64_t chunks_missing;    /* tile_id+dim not found in field       */
    uint64_t bytes_written;     /* total output bytes                   */
} GeoFieldDecodeStats;

/*
 * Decode a single 64B chunk from the GeoField.
 * chunk_idx : which chunk (0..n_chunks-1)
 * out       : output buffer (must be >= 64 bytes)
 * Returns 0 on success, -1 if chunk not found.
 */
static inline int geo_field_decode_chunk(const GeoField *gf,
                                          uint64_t        chunk_idx,
                                          uint8_t         out[64])
{
    if (!gf || !gf->blocks || !out) return -1;

    GpAddr a = gp_chunk_to_addr(gf->gp_level, chunk_idx);

    uint32_t blocks_per_layer = (gf->face_max + GF_CHUNKS_PER_BLOCK - 1)
                                / GF_CHUNKS_PER_BLOCK;
    uint32_t tile_group = a.tile_id / GF_CHUNKS_PER_BLOCK;
    uint32_t block_idx = (uint32_t)a.dim * blocks_per_layer + tile_group;
    if (block_idx >= gf->n_blocks) {
        memset(out, 0, GF_CHUNK_SZ);
        return -1;
    }

    const FrustumBlock *blk = &gf->blocks[block_idx];
    uint8_t diamond_slot = (uint8_t)(a.tile_id % FGLS_DIAMOND_COUNT);

    /* Check that data was actually written */
    uint8_t drain_idx = gp_is_pentagon(a.tile_id)
                      ? (uint8_t)a.tile_id
                      : gp_tile_to_pent(gf->gp_level, a.tile_id);
    if (!(blk->meta.drain_state[drain_idx] & 0x01u)) {
        memset(out, 0, GF_CHUNK_SZ);
        return -1;
    }

    memcpy(out, blk->data + (size_t)diamond_slot * FGLS_DIAMOND_BYTES,
           GF_CHUNK_SZ);
    return 0;
}

/*
 * Decode entire GeoField into output buffer.
 * out       : output buffer (must be large enough)
 * max_sz   : max bytes to decode
 * Returns number of bytes written, or -1 on error.
 */
static inline int64_t geo_field_decode(const GeoField  *gf,
                                        uint8_t         *out,
                                        size_t           max_sz,
                                        GeoFieldDecodeStats *stats)
{
    if (!gf || !out || !stats) return -1;

    memset(stats, 0, sizeof(*stats));

    uint64_t n_chunks = (uint64_t)((max_sz + GF_CHUNK_SZ - 1) / GF_CHUNK_SZ);
    int64_t written = 0;

    for (uint64_t ci = 0; ci < n_chunks; ci++) {
        uint8_t chunk[GF_CHUNK_SZ];
        int ret = geo_field_decode_chunk(gf, ci, chunk);
        if (ret == 0) {
            size_t offset = (size_t)(ci * GF_CHUNK_SZ);
            size_t to_write = GF_CHUNK_SZ;
            if (offset + to_write > max_sz) to_write = max_sz - offset;
            memcpy(out + offset, chunk, to_write);
            written += (int64_t)to_write;
            stats->chunks_decoded++;
        } else {
            stats->chunks_missing++;
        }
    }

    stats->bytes_written = (uint64_t)written;
    return written;
}

/* ═══════════════════════════════════════════════════════════════════════
   SCALE — zoom in/out by changing gp_level
   ═══════════════════════════════════════════════════════════════════════
 *
 * "Zoom in"  = higher gp_level → more tiles/finer subdivision
 * "Zoom out" = lower gp_level → fewer tiles/coarser subdivision
 *
 * Data at dim=0 stays at the same tile_id across levels (pentagon anchors
 * are fixed).  But the hexagon count changes: gp_face_count(level) grows
 * as 10n²+2.  So more dims get different tile mappings as level changes.
 *
 * This function converts an address from one level to another, preserving
 * the relative position within the dimension layer.
 */

/*
 * Convert GpAddr from one gp_level to another (scale).
 * Preserves tile_id proportionally scaled.
 */
static inline GpAddr geo_field_scale_addr(const GpAddr *src,
                                           uint8_t       src_level,
                                           uint8_t       dst_level)
{
    GpAddr dst;
    uint32_t src_faces = gp_face_count(src_level);
    uint32_t dst_faces = gp_face_count(dst_level);

    /* Scale tile_id proportionally, always keep pentagons fixed */
    if (src->tile_id < GP_PENT_COUNT) {
        dst.tile_id = src->tile_id;  /* pentagons are universal anchors */
    } else {
        /* Scale hex tile proportionally into new level */
        dst.tile_id = (uint32_t)((uint64_t)src->tile_id * dst_faces / src_faces);
        if (dst.tile_id >= dst_faces) dst.tile_id = dst_faces - 1;
    }

    dst.dim = src->dim;  /* dim stays the same (which sphere layer) */
    return dst;
}

/*
 * "Zoom in" — increase gp_level, return number of new tiles at dim.
 * Returns the new face count at the higher level.
 */
static inline uint32_t geo_field_zoom_in(uint8_t current_level,
                                          uint8_t *out_new_level)
{
    uint8_t nl = current_level;
    if (current_level < GP_MAX_LEVEL) nl = (uint8_t)(current_level + 1);
    if (out_new_level) *out_new_level = nl;
    return gp_face_count(nl);
}

/*
 * "Zoom out" — decrease gp_level, return number of new tiles at dim.
 */
static inline uint32_t geo_field_zoom_out(uint8_t current_level,
                                           uint8_t *out_new_level)
{
    uint8_t nl = (current_level > 1) ? current_level - 1 : 1;
    if (out_new_level) *out_new_level = nl;
    return gp_face_count(nl);
}

/*
 * Encode same data at multiple gp_levels for multi-resolution access.
 * Returns number of levels encoded, or -1 on error.
 */
static inline int geo_field_encode_multires(const uint8_t  *data,
                                              size_t          data_sz,
                                              uint8_t         min_level,
                                              uint8_t         max_level,
                                              GeoField       *fields_out,
                                              GeoFieldEncodeStats *stats_out)
{
    if (!data || !fields_out || !stats_out) return -1;
    if (min_level < 1) min_level = 1;
    if (max_level > GP_MAX_LEVEL) max_level = GP_MAX_LEVEL;

    int n_levels = 0;

    for (uint8_t lv = min_level; lv <= max_level; lv++) {
        uint32_t face_max = gp_face_count(lv);
        uint32_t blocks_per_layer = (face_max + GF_CHUNKS_PER_BLOCK - 1)
                                    / GF_CHUNKS_PER_BLOCK;
        uint32_t n_blocks = (uint32_t)GP_MAX_DIM * blocks_per_layer;
        if (n_blocks < 1) n_blocks = 1;

        if (geo_field_init(&fields_out[n_levels], lv, n_blocks) != 0)
            return -1;

        if (geo_field_encode(&fields_out[n_levels], data, data_sz,
                              &stats_out[n_levels]) != 0)
            return -1;

        n_levels++;
    }

    return n_levels;
}

/* ═══════════════════════════════════════════════════════════════════════
   SHAPE DIMENSION ACCESS — Metatron routing between faces
   ═══════════════════════════════════════════════════════════════════════
 *
 * Navigate between pentagon faces (0..11) using Metatron's Cube routing.
 * Any GpAddr.tile_id maps to a face via gp_tile_to_pent().
 * From that face, you can reach any other face via:
 *
 *   ORBITAL — stay on same face, circular slot progression
 *   CHIRAL  — jump to opposite face (face f ↔ face f+6)
 *   CROSS   — inter-ring 3-step (face 0→9, 1→10, etc.)
 *   HUB     — any face via center node
 *
 * "Dimension access between shapes" means:
 *   Read data at face A, then use Metatron routing to find the
 *   corresponding data at face B (same relative slot position).
 */

/* Result of a shape dimension access operation */
typedef struct {
    uint16_t      src_enc;      /* source encoding (0..719)             */
    uint16_t      dst_enc;      /* destination encoding (0..719)        */
    uint8_t       src_face;     /* source face (0..11)                  */
    uint8_t       dst_face;     /* destination face (0..11)             */
    MetaRouteType route_type;   /* how we got from src to dst           */
    uint8_t       src_slot;     /* slot within source face (0..59)       */
    uint8_t       dst_slot;     /* slot within destination face (0..59)  */
} GeoFieldShapeAccess;

/*
 * Convert a tile_id to a Metatron enc value (0..719).
 * Uses the tile_id's position in the Goldberg sphere.
 */
static inline uint16_t geo_field_tile_to_enc(uint32_t tile_id,
                                              uint8_t  gp_level)
{
    if (tile_id >= GP_PENT_COUNT) {
        uint8_t pent = gp_tile_to_pent(gp_level, tile_id);
        uint32_t base = gp_sector_base(gp_level, pent);
        uint32_t hex_offset = tile_id - base;
        uint32_t slot = (hex_offset * 60) / gp_face_count(gp_level);
        return (uint16_t)(pent * 60 + (slot % 60));
    }
    return (uint16_t)(tile_id * 60);  /* pentagon anchors at slot 0 of each face */
}

/*
 * Navigate from one tile to another via Metatron routing.
 * src_tile_id  : source tile
 * dst_face     : destination face (0..11), 0xFF = orbital (stay on same face)
 * gp_level     : current sphere subdivision level
 */
static inline GeoFieldShapeAccess geo_field_shape_route(uint32_t     src_tile_id,
                                                         uint8_t      dst_face,
                                                         uint8_t      gp_level)
{
    GeoFieldShapeAccess r;
    r.src_enc  = geo_field_tile_to_enc(src_tile_id, gp_level);
    r.src_face = (uint8_t)(r.src_enc / 60);
    r.src_slot = (uint8_t)(r.src_enc % 60);

    MetaDecision md = meta_route(r.src_enc, dst_face);
    r.dst_enc  = md.next_enc;
    r.route_type = md.type;
    r.dst_face = (uint8_t)(r.dst_enc / 60);
    r.dst_slot = (uint8_t)(r.dst_enc % 60);

    return r;
}

/*
 * Access data at a shape-routed destination.
 * Reads the 64B chunk from the field at the destination face slot.
 * chunk_idx : the original chunk index in the field
 * Returns 0 on success, -1 if not found.
 */
static inline int geo_field_shape_read(const GeoField     *gf,
                                         uint64_t            chunk_idx,
                                         MetaRouteType       route,
                                         uint8_t             dst_face,
                                         uint8_t             out_chunk[64])
{
    (void)route;
    GpAddr a = gp_chunk_to_addr(gf->gp_level, chunk_idx);
    uint16_t enc = geo_field_tile_to_enc(a.tile_id, gf->gp_level);

    MetaDecision md = meta_route(enc, dst_face);
    uint16_t dst_enc = md.next_enc;

    /* Convert destination enc back to tile_id approximation */
    uint8_t dface = (uint8_t)(dst_enc / 60);
    uint8_t dslot = (uint8_t)(dst_enc % 60);

    /* Find a tile_id near this face+slot that has data */
    uint32_t dtile;
    if (dface < GP_PENT_COUNT) {
        dtile = dface;  /* pentagon tiles are direct */
    } else {
        /* Scale slot to find a hex tile in this pentagon's sector */
        uint32_t base = gp_sector_base(gf->gp_level, dface);
        uint32_t hex_sz = gp_hex_in_sector(gf->gp_level, dface);
        if (hex_sz > 0) {
            dtile = base + ((uint32_t)dslot * hex_sz / 60);
        } else {
            dtile = base;
        }
        if (dtile >= gf->face_max) dtile = gf->face_max - 1;
    }

    /* Read from destination */
    GpAddr da;
    da.tile_id = dtile;
    da.dim     = a.dim;
    return geo_field_decode_chunk(gf,
                                   (uint64_t)da.dim * gf->face_max + da.tile_id,
                                   out_chunk);
}

/* ═══════════════════════════════════════════════════════════════════════
   FIBER — traverse all dimensions of a tile (gp_fiber_next wrapper)
   ═══════════════════════════════════════════════════════════════════════
 *
 * A "fiber" is the perpendicular channel: all dim values for the same
 * tile_id.  This lets you see how a tile's data evolves across layers.
 * (The GP analogy: same (x,y) at different z-levels.)
 */

/*
 * Count how many dimension layers have data for a given tile.
 */
static inline int geo_field_fiber_count(const GeoField *gf, uint32_t tile_id)
{
    int count = 0;
    for (uint8_t d = 0; d < GP_MAX_DIM; d++) {
        if (gp_lens_read(&gf->sphere, tile_id, d) != NULL)
            count++;
    }
    return count;
}

/*
 * Traverse fiber, calling a callback for each dimension with data.
 * cb(context, tile_id, dim, chunk_data)
 */
typedef void (*geo_fiber_cb)(void *ctx, uint32_t tile_id,
                              uint8_t dim, const uint8_t chunk[64]);

static inline int geo_field_fiber_walk(const GeoField *gf,
                                        uint32_t        tile_id,
                                        geo_fiber_cb    cb,
                                        void           *ctx)
{
    int found = 0;
    for (uint8_t d = 0; d < GP_MAX_DIM; d++) {
        const uint8_t *chunk = gp_lens_read(&gf->sphere, tile_id, d);
        if (chunk) {
            if (cb) cb(ctx, tile_id, d, chunk);
            found++;
        }
    }
    return found;
}

/* ═══════════════════════════════════════════════════════════════════════
   SERIALIZE / DESERIALIZE — full field ↔ file
   ═══════════════════════════════════════════════════════════════════════
 *
 * Each FrustumBlock is 4896B.
 * File format:
 *   [header: 32B]     — magic "GEOF", version, gp_level, n_blocks, etc.
 *   [blocks: n×4896B] — serialized FrustumBlocks via gcfs_serialize
 */

#define GF_FILE_MAGIC   "GEOF"
#define GF_FILE_VERSION 1u

typedef struct {
    uint8_t  magic[4];         /* "GEOF"                              */
    uint8_t  version;          /* format version                      */
    uint8_t  gp_level;         /* subdivision level (1..8)            */
    uint8_t  _pad1;
    uint32_t n_blocks;         /* number of FrustumBlocks             */
    uint64_t orig_size;        /* original data size                  */
    uint64_t digest;           /* xxh64 of original data              */
    uint8_t  _pad2[4];         /* total = 32B                         */
} GeoFieldFileHeader;

/* Simple xxh64 for digest */
#define _GF_H1 0x9e3779b97f4a7c15ULL
#define _GF_H2 0x6c62272e07bb0142ULL
static inline uint64_t _gf_rot(uint64_t x, int r) { return (x<<r)|(x>>(64-r)); }
static inline uint64_t _gf_hu(uint64_t a, uint64_t w) {
    a ^= (w*_GF_H1); a = _gf_rot(a,27); a = a*_GF_H2 + 0x94d049bb133111ebULL; return a;
}
static inline uint64_t geo_field_xxh64(const uint8_t *d, size_t n) {
    uint64_t a = _GF_H1 ^ (uint64_t)n; size_t i = 0;
    for (; i+8 <= n; i+=8) { uint64_t w; memcpy(&w, d+i, 8); a = _gf_hu(a,w); }
    if (i < n) { uint64_t t = 0; memcpy(&t, d+i, n-i); a = _gf_hu(a,t); }
    a ^= (a>>33); a *= _GF_H1; a ^= (a>>29); a *= _GF_H2; a ^= (a>>32); return a;
}

/*
 * Serialize GeoField to a file.
 * data_orig : original input data (for digest + size tracking)
 * data_sz   : original size
 */
static inline int geo_field_save(const GeoField *gf,
                                  const uint8_t  *data_orig,
                                  size_t          data_sz,
                                  const char     *path)
{
    if (!gf || !path) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    /* Header */
    GeoFieldFileHeader hdr;
    memcpy(hdr.magic, GF_FILE_MAGIC, 4);
    hdr.version   = GF_FILE_VERSION;
    hdr.gp_level  = gf->gp_level;
    hdr._pad1     = 0;
    hdr.n_blocks  = gf->n_blocks;
    hdr.orig_size = data_sz;
    hdr.digest    = data_orig ? geo_field_xxh64(data_orig, data_sz) : 0;
    memset(hdr._pad2, 0, 4);

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return -1; }

    /* Write each FrustumBlock directly (preserves all meta) */
    for (uint32_t bi = 0; bi < gf->n_blocks; bi++) {
        if (fwrite(&gf->blocks[bi], 1, sizeof(FrustumBlock), f)
            != sizeof(FrustumBlock)) {
            fclose(f); return -1;
        }
    }

    fclose(f);
    return 0;
}

/*
 * Load GeoField from a file.
 * Returns the original size stored in header, or -1 on error.
 */
static inline int64_t geo_field_load(GeoField *gf, const char *path)
{
    if (!gf || !path) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    GeoFieldFileHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return -1; }
    if (memcmp(hdr.magic, GF_FILE_MAGIC, 4) != 0) { fclose(f); return -1; }
    if (hdr.version != GF_FILE_VERSION) { fclose(f); return -1; }

    /* Init field with stored parameters */
    if (geo_field_init(gf, hdr.gp_level, hdr.n_blocks) != 0) {
        fclose(f); return -1;
    }

    /* Read each FrustumBlock directly */
    for (uint32_t bi = 0; bi < hdr.n_blocks; bi++) {
        if (fread(&gf->blocks[bi], 1, sizeof(FrustumBlock), f)
            != sizeof(FrustumBlock)) {
            geo_field_free(gf); fclose(f); return -1;
        }
    }

    fclose(f);
    return (int64_t)hdr.orig_size;
}

/* ═══════════════════════════════════════════════════════════════════════
   COMPLETE ROUND-TRIP VERIFY
   ═══════════════════════════════════════════════════════════════════════
 *
 * Returns:
 *   0  = round-trip OK (data matches after encode→decode)
 *  -1  = encode failure
 *  -2  = decode failure
 *  -3  = data mismatch
 */

static inline int geo_field_roundtrip(const uint8_t *data, size_t data_sz,
                                       uint8_t gp_level,
                                       GeoFieldEncodeStats *enc_stats,
                                       GeoFieldDecodeStats *dec_stats)
{
    uint32_t tiles_per_layer = gp_face_count(gp_level);
    uint32_t blocks_per_layer = (tiles_per_layer + GF_CHUNKS_PER_BLOCK - 1)
                                / GF_CHUNKS_PER_BLOCK;
    uint32_t n_blocks = (uint32_t)GP_MAX_DIM * blocks_per_layer;
    if (n_blocks < 1) n_blocks = 1;

    GeoField gf;
    if (geo_field_init(&gf, gp_level, n_blocks) != 0) return -1;

    if (geo_field_encode(&gf, data, data_sz, enc_stats) != 0) {
        geo_field_free(&gf);
        return -1;
    }

    uint8_t *decoded = (uint8_t *)malloc(data_sz ? data_sz : 1);
    if (!decoded) { geo_field_free(&gf); return -1; }

    int64_t written = geo_field_decode(&gf, decoded, data_sz, dec_stats);
    if (written < 0 || (size_t)written != data_sz) {
        free(decoded);
        geo_field_free(&gf);
        return -2;
    }

    int match = (memcmp(data, decoded, data_sz) == 0) ? 0 : -3;
    free(decoded);
    geo_field_free(&gf);
    return match;
}

#endif /* GEO_FIELD_CORE_H */

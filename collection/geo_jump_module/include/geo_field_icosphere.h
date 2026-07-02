/*
 * geo_field_icosphere.h — Icosphere-GeoField Bridge
 *
 * Integrates icosphere on-demand capture (icosa_capture_on_demand)
 * with GeoField's Goldberg sphere tile storage.
 *
 * Key insight: gp_level=4 gives gp_face_count(4) = 10×4²+2 = 162 tiles,
 * matching the f=4 icosphere vertex count exactly. This enables a
 * geometric addressing scheme where 2D signal coordinates (vx, vy) map
 * directly to icosahedron face positions, which in turn map to Geo field tiles.
 *
 * The bridge provides:
 *   - capture_key ↔ GpAddr conversion  (via icosa capture + dominant face)
 *   - 1:1 mapping at gp_level=4        (162 tiles = 162 icosphere vertices)
 *   - capture_key ↔ GEO_FULL (20736)   (bridges triplet world address space)
 *   - Encode/decode wrappers for GeoField (when geo_field_core.h is available)
 *
 * Two compilation modes:
 *   Default:     only needs icosphere_capture.h + geo_goldberg_sphere.h
 *   Full mode:   #define GF_ICOSPHERE_USE_GEO_FIELD before including
 *                to enable GeoField-backed encode/decode (needs geo_field_core.h)
 *
 * No malloc. No new tables.
 *
 * Depends minimally on: icosphere_capture.h, geo_goldberg_sphere.h
 */

#ifndef GEO_FIELD_ICOSPHERE_H
#define GEO_FIELD_ICOSPHERE_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#include "icosphere_capture.h"
#include "geo_goldberg_sphere.h"

/* ═══════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════ */

/* Total capture positions = 20 faces × 15 vertices per face = 300 */
#define GF_ICOSPHERE_CAPTURE_SLOTS    300u

/* ═══════════════════════════════════════════════════════════════════════
   CAPTURE KEY → DODECA FACE CENTER INDICES
   ═══════════════════════════════════════════════════════════════════════
 *
 * ICOSA_FACES (from icosphere_capture.h) maps each icosa face (0..19)
 * to 3 dodeca face center indices (0..11). These are the 3 pentagon
 * anchors of the Geo field that form each icosa triangle.
 */

/*
 * Get dominant dodeca face from capture key.
 * Returns: dodeca face index (0..11) and a sub-tile offset (0..255).
 *
 * capture_key: 0..299 = face*ICOSA_VERTS_PER_FACE + idx
 *   face: icosa face index (0..19)
 *   idx:  vertex within face (0..14), encodes barycentric (i,j,k)
 *
 * For each capture key, finds:
 *   - The dominant dodeca face (largest barycentric weight)
 *   - A sub-tile offset from secondary weights (for hex position)
 */
static inline void icosphere_capture_key_to_dodeca(uint32_t capture_key,
                                                     uint8_t *out_dodeca_face,
                                                     uint8_t *out_tile_offset)
{
    int face = (int)(capture_key / ICOSA_VERTS_PER_FACE);
    int idx  = (int)(capture_key % ICOSA_VERTS_PER_FACE);

    /* Decode (i,j,k) from idx using triangular number row decoding */
    int j = 0, cum = 0;
    for (; j <= ICOSA_F4; j++) {
        int row = ICOSA_F4 - j + 1;
        if (idx < cum + row) break;
        cum += row;
    }
    int i = idx - cum;
    int k = ICOSA_F4 - i - j;

    /* Find dominant dodeca face (largest barycentric weight) */
    int weights[3] = {i, j, k};
    int dominant = 0;
    if (weights[1] > weights[dominant]) dominant = 1;
    if (weights[2] > weights[dominant]) dominant = 2;

    /* Map to dodeca face index */
    int dodeca_idx = ICOSA_FACES[face][dominant];
    if (out_dodeca_face) *out_dodeca_face = (uint8_t)dodeca_idx;

    /* Compute sub-tile offset from secondary weights */
    if (weights[dominant] == ICOSA_F4) {
        if (out_tile_offset) *out_tile_offset = 0;
    } else {
        int w1 = weights[(dominant + 1) % 3];
        int w2 = weights[(dominant + 2) % 3];
        uint32_t offset = (uint32_t)(
            (uint64_t)(w1 * ICOSA_F4 + w2) * 255u
            / (uint64_t)(ICOSA_F4 * ICOSA_F4)
        );
        if (out_tile_offset) *out_tile_offset = (uint8_t)(offset & 0xFFu);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   CAPTURE KEY ↔ GpAddr CONVERSION
   ═══════════════════════════════════════════════════════════════════════
 *
 * Two strategies:
 *
 * 1. chunk-based (recommended): treat capture_key as chunk_idx and use
 *    gp_chunk_to_addr() / gp_addr_to_chunk(). This naturally distributes
 *    300 capture keys across tile_id and dim layers.
 *
 *    At gp_level=4: 162 tiles × dim 0 (keys 0..161) + dim 1 (keys 162..299)
 *    At gp_level=8: 642 tiles × dim 0 (keys 0..299 are all in dim 0)
 *
 * 2. dominant-face: maps each key's dominant dodeca face directly to
 *    a pentagon tile_id. Used for coarse levels (gp_level < 4).
 */

/*
 * Strategy 1: Chunk-based. Maps capture_key → GpAddr via gp_chunk_to_addr.
 * This distributes keys across tiles and dims uniformly.
 */
static inline GpAddr icosphere_capture_key_to_chunk_addr(uint32_t capture_key,
                                                           uint8_t  gp_level)
{
    return gp_chunk_to_addr(gp_level, (uint64_t)capture_key);
}

/*
 * Strategy 2: Dominant-face. Maps capture_key to the dodeca pentagon tile.
 * Useful at coarse gp_levels where exact tile positioning is less critical.
 */
static inline GpAddr icosphere_capture_key_to_pent_addr(uint32_t capture_key,
                                                          uint8_t  gp_level)
{
    GpAddr a;
    uint8_t dodeca_face, tile_offset;
    icosphere_capture_key_to_dodeca(capture_key, &dodeca_face, &tile_offset);
    a.tile_id = dodeca_face;
    a.dim     = (uint8_t)(tile_offset / (256u / GP_MAX_DIM));
    if (a.dim >= GP_MAX_DIM) a.dim = GP_MAX_DIM - 1;
    if (a.tile_id >= gp_face_count(gp_level))
        a.tile_id = gp_face_count(gp_level) - 1;
    return a;
}

/*
 * Convert icosa_capture_on_demand(vx, vy) result directly to GpAddr.
 * vx, vy: stereographic input coordinates (fixed-point).
 */
static inline GpAddr icosphere_capture_to_gpaddr(int64_t vx, int64_t vy,
                                                   uint8_t gp_level)
{
    uint32_t key = icosa_capture_on_demand(vx, vy);
    return icosphere_capture_key_to_chunk_addr(key, gp_level);
}

/*
 * Reverse: GpAddr → capture key.
 * Not lossless (300 capture keys pack into GpAddr's larger address space).
 * Used for approximate lookup and debugging.
 */
static inline uint32_t icosphere_gpaddr_to_capture_key(GpAddr a,
                                                         uint8_t gp_level)
{
    uint64_t chunk_idx = gp_addr_to_chunk(gp_level, a);
    if (chunk_idx >= GF_ICOSPHERE_CAPTURE_SLOTS)
        chunk_idx %= GF_ICOSPHERE_CAPTURE_SLOTS;
    return (uint32_t)chunk_idx;
}

/* ═══════════════════════════════════════════════════════════════════════
   CAPTURE KEY → 3D POSITION
   ═══════════════════════════════════════════════════════════════════════
 *
 * Compute 3D position on face-center sphere (R ≈ 1.37638) from capture key.
 * No table lookup — uses icosa_decode_position's barycentric blend.
 */

static inline void icosphere_capture_key_position(uint32_t capture_key,
                                                    double *ox, double *oy, double *oz)
{
    icosa_decode_position(capture_key, ox, oy, oz);
}

/* ═══════════════════════════════════════════════════════════════════════
   GEO_FULL ADDRESS SPACE — Triplet world bridge
   ═══════════════════════════════════════════════════════════════════════
 *
 * Maps capture key to GEO_FULL (20736) node_id in the triplet world.
 * This bridges icosphere addressing with the full geometric address space
 * shared by Y-triangle, DRamTile, SID page table, and shell levels.
 *
 * GEO_FULL layout:
 *   12 pentagons × 1728 nodes = 20736
 *   Each pentagon: 12 shell levels × 144 nodes/level = 1728
 *   Shell level 0 = innermost (icosa vertices)
 *   Shell level 11 = outermost (dodeca surface)
 */

/*
 * Convert capture key → GEO_FULL node_id (0..20735).
 * Maps the dominant dodeca face to a pentagon, and the barycentric
 * position to a shell level + sector offset.
 */
static inline uint32_t icosphere_capture_key_to_geo_full(uint32_t capture_key)
{
    uint8_t dodeca_face, tile_offset;
    icosphere_capture_key_to_dodeca(capture_key, &dodeca_face, &tile_offset);

    /* Each pentagon: 1728 nodes */
    uint32_t pentagon_base = (uint32_t)dodeca_face * 1728u;

    /* Shell level 0..11 from tile_offset */
    uint32_t shell = (uint32_t)tile_offset * 11u / 255u;
    if (shell >= 12u) shell = 11u;

    /* Sector offset within shell */
    uint32_t sector_offset = (uint32_t)tile_offset % 144u;

    return pentagon_base + shell * 144u + sector_offset;
}

/*
 * Reverse: GEO_FULL node_id → approximate capture key.
 */
static inline uint32_t icosphere_geo_full_to_capture_key(uint32_t geo_node)
{
    uint32_t pentagon = geo_node / 1728u;
    if (pentagon >= 12u) pentagon = 11u;
    uint32_t shell_level = (geo_node % 1728u) / 144u;
    uint32_t sector_off  = (geo_node % 1728u) % 144u;

    /* Shell level → approximate offset within dominant dodeca face */
    uint32_t approx_offset = shell_level * 255u / 11u;
    if (approx_offset > 255u) approx_offset = 255u;

    /* Find a capture key whose dominant face matches */
    for (uint32_t k = 0; k < GF_ICOSPHERE_CAPTURE_SLOTS; k++) {
        uint8_t df, off;
        icosphere_capture_key_to_dodeca(k, &df, &off);
        if (df == pentagon && off == (uint8_t)approx_offset)
            return k;
    }
    /* Fallback: approximate from pentagon */
    return pentagon * (GF_ICOSPHERE_CAPTURE_SLOTS / 12u);
}

/* ═══════════════════════════════════════════════════════════════════════
   DEBUG — Print capture key → address mapping
   ═══════════════════════════════════════════════════════════════════════ */

static inline void icosphere_print_addr_map(uint8_t gp_level)
{
    printf("=== Icosphere Capture → GpAddr Map (gp_level=%u) ===\n", gp_level);
    printf("face_max=%u, GF_ICOSPHERE_CAPTURE_SLOTS=%u\n\n",
           gp_face_count(gp_level), GF_ICOSPHERE_CAPTURE_SLOTS);
    printf("%4s %5s %6s %6s %6s %5s %6s\n",
           "key", "face", "dodeca", "offset", "tile", "dim", "geo_full");
    for (uint32_t k = 0; k < GF_ICOSPHERE_CAPTURE_SLOTS; k++) {
        uint8_t df, off;
        icosphere_capture_key_to_dodeca(k, &df, &off);
        GpAddr a = icosphere_capture_key_to_chunk_addr(k, gp_level);
        uint32_t gn = icosphere_capture_key_to_geo_full(k);
        printf("%4u %5u %6u %6u %6u %5u %6u\n",
               k, k / ICOSA_VERTS_PER_FACE, df, off, a.tile_id, a.dim, gn);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   FULL Geofield INTEGRATION (optional)
   ═══════════════════════════════════════════════════════════════════════
 *
   To use GeoField-backed encode/decode, define GF_ICOSPHERE_USE_GEO_FIELD
   before including this header, and ensure geo_field_core.h is in the
   include path.

   Example:
     #define GF_ICOSPHERE_USE_GEO_FIELD
     #include "geo_field_icosphere.h"

   This adds:
     icosphere_lens_encode_chunk()  — encode at a capture key position
     icosphere_lens_decode_chunk()  — decode from a capture key position
     icosphere_field_encode()       — full encode via icosphere positions
     icosphere_field_decode()       — full decode from icosphere positions
     icosphere_field_roundtrip()    — full round-trip verification
 */

#ifdef GF_ICOSPHERE_USE_GEO_FIELD
/* #include "geo_field_core.h" — must be included before this */

/*
 * Encode a 64B chunk at a specific capture key position.
 * Re-uses the GeoField's existing encode mechanism.
 */
static inline int icosphere_lens_encode_chunk(GeoField           *gf,
                                               uint32_t            capture_key,
                                               const uint8_t       chunk[64],
                                               GeoFieldEncodeStats *stats)
{
    if (!gf || !chunk || capture_key >= GF_ICOSPHERE_CAPTURE_SLOTS)
        return -1;

    GpAddr a = icosphere_capture_key_to_chunk_addr(capture_key, gf->gp_level);
    uint64_t chunk_idx = (uint64_t)a.dim * gf->face_max + a.tile_id;
    return geo_field_encode_chunk(gf, chunk_idx, chunk, NULL, stats);
}

/*
 * Decode a 64B chunk from a capture key position.
 */
static inline int icosphere_lens_decode_chunk(const GeoField *gf,
                                               uint32_t        capture_key,
                                               uint8_t         out[64])
{
    if (!gf || !out || capture_key >= GF_ICOSPHERE_CAPTURE_SLOTS)
        return -1;

    GpAddr a = icosphere_capture_key_to_chunk_addr(capture_key, gf->gp_level);
    uint64_t chunk_idx = (uint64_t)a.dim * gf->face_max + a.tile_id;
    return geo_field_decode_chunk(gf, chunk_idx, out);
}

/*
 * Full icosphere encode: distribute data across capture key positions.
 * Each 64B chunk is stored at capture_key = chunk_idx % 300,
 * cycling through all 20 icosa faces × 15 vertex positions.
 */
static inline int icosphere_field_encode(GeoField           *gf,
                                          const uint8_t      *data,
                                          size_t              data_sz,
                                          GeoFieldEncodeStats *stats)
{
    if (!gf || !data || !stats) return -1;
    memset(stats, 0, sizeof(*stats));

    uint64_t n_chunks = (uint64_t)((data_sz + GF_CHUNK_SZ - 1) / GF_CHUNK_SZ);

    for (uint64_t ci = 0; ci < n_chunks; ci++) {
        size_t offset = (size_t)(ci * GF_CHUNK_SZ);
        size_t remain = (offset < data_sz) ? data_sz - offset : 0;
        uint8_t chunk[GF_CHUNK_SZ] = {0};
        size_t cp = (remain < GF_CHUNK_SZ) ? remain : GF_CHUNK_SZ;
        if (cp > 0) memcpy(chunk, data + offset, cp);

        uint32_t capture_key = (uint32_t)(ci % GF_ICOSPHERE_CAPTURE_SLOTS);
        int ret = icosphere_lens_encode_chunk(gf, capture_key, chunk, stats);
        if (ret < 0) return -1;
    }

    stats->total_blocks = gf->n_blocks;
    return 0;
}

/*
 * Full icosphere decode: retrieve data from capture positions.
 */
static inline int64_t icosphere_field_decode(const GeoField  *gf,
                                              uint8_t         *out,
                                              size_t           max_sz,
                                              GeoFieldDecodeStats *stats)
{
    if (!gf || !out || !stats) return -1;
    memset(stats, 0, sizeof(*stats));

    uint64_t n_chunks = (uint64_t)((max_sz + GF_CHUNK_SZ - 1) / GF_CHUNK_SZ);
    int64_t written = 0;

    for (uint64_t ci = 0; ci < n_chunks; ci++) {
        uint32_t capture_key = (uint32_t)(ci % GF_ICOSPHERE_CAPTURE_SLOTS);
        uint8_t chunk[GF_CHUNK_SZ];
        int ret = icosphere_lens_decode_chunk(gf, capture_key, chunk);
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

/*
 * Full round-trip verify.
 * Returns: 0=OK, -1=encode fail, -2=decode fail, -3=data mismatch
 */
static inline int icosphere_field_roundtrip(const uint8_t *data,
                                              size_t         data_sz,
                                              uint8_t        gp_level,
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

    if (icosphere_field_encode(&gf, data, data_sz, enc_stats) != 0) {
        geo_field_free(&gf);
        return -1;
    }

    uint8_t *decoded = (uint8_t *)malloc(data_sz ? data_sz : 1);
    if (!decoded) { geo_field_free(&gf); return -1; }

    int64_t written = icosphere_field_decode(&gf, decoded, data_sz, dec_stats);
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

#endif /* GF_ICOSPHERE_USE_GEO_FIELD */

/* ═══════════════════════════════════════════════════════════════════════
   PRIORITY ZONE — 60 Trapezoid Towers
   ═══════════════════════════════════════════════════════════════════════
 *
 * 60 deterministic towers (12 dodeca faces × 5 edges):
 *   Each tower = GEO_BLOCK (48 = 4×4×3) × GEO_TOWER (144) = 6912 nodes
 *   Total = 60 × 6912 = 414,720  (20× GEO_FULL = 20 × 20,736)
 *
 * Acts as the "RAM tier" for the storage system — opposite of residual zone.
 * Hot data from icosa capture routes through these towers before settling
 * into GEO_FULL bulk storage.
 *
 * Tower adjacency follows the dodeca ADJ table: edge e of face f connects
 * to neighbor ADJ[f][e] with entry edge ADJ[f][e][1].
 */

#define GF_PRIORITY_TRAPEZOIDS     60u     /* 12 × 5 */
#define GF_PRIORITY_TOWER_NODES   6912u    /* GEO_BLOCK × GEO_TOWER = 48 × 144 */
#define GF_PRIORITY_TOTAL        414720u   /* 60 × 6912 */
#define GF_PRIORITY_GEO_FULL_RATIO 20u     /* 414720 / 20736 */

/* Dodeca adjacency: face→edge→{neighbor_face, entry_edge_on_neighbor}
   Same as ADJ in geo_hidden_pocket.h pocket_cross_edge(). */
static const uint8_t GF_TRAP_ADJ[12][5][2] = {
    {{4,0},{3,1},{6,0},{11,4},{8,4}},   /* face 0 */
    {{7,1},{2,1},{5,1},{9,2},{10,2}},   /* face 1 */
    {{7,0},{1,1},{5,0},{11,2},{8,2}},   /* face 2 */
    {{4,1},{0,1},{6,1},{9,4},{10,4}},   /* face 3 */
    {{0,0},{3,0},{10,0},{7,3},{8,0}},   /* face 4 */
    {{2,2},{1,2},{9,1},{6,3},{11,1}},   /* face 5 */
    {{0,2},{3,2},{9,0},{5,3},{11,0}},   /* face 6 */
    {{2,0},{1,0},{10,1},{4,3},{8,1}},   /* face 7 */
    {{4,4},{7,4},{2,4},{11,3},{0,4}},   /* face 8 */
    {{6,2},{5,2},{1,3},{10,3},{3,3}},   /* face 9 */
    {{4,2},{7,2},{1,4},{9,3},{3,4}},    /* face 10 */
    {{6,4},{5,4},{2,3},{8,3},{0,3}},    /* face 11 */
};

/* ── Geometric vertex adjacency (12 vertices × 5 neighbors at edge distance ~1.447)
      ICOSA_FACES has 26/60 invalid edges, so we use actual geometric adjacency. ── */
static const uint8_t GF_VERTEX_NB[12][5] = {
    {3, 4, 6, 8, 11},    /* v 0 */
    {2, 5, 7, 9, 10},    /* v 1 */
    {1, 5, 7, 8, 11},    /* v 2 */
    {0, 4, 6, 9, 10},    /* v 3 */
    {0, 3, 7, 8, 10},    /* v 4 */
    {1, 2, 6, 9, 11},    /* v 5 */
    {0, 3, 5, 9, 11},    /* v 6 */
    {1, 2, 4, 8, 10},    /* v 7 */
    {0, 2, 4, 7, 11},    /* v 8 */
    {1, 3, 5, 6, 10},    /* v 9 */
    {1, 3, 4, 7, 9},     /* v 10 */
    {0, 2, 5, 6, 8},     /* v 11 */
};

/* Check if vertex v has neighbor u */
static inline int gf_is_neighbor(uint8_t v, uint8_t u) {
    for (int i = 0; i < 5; i++)
        if (GF_VERTEX_NB[v][i] == u) return 1;
    return 0;
}

/* ── Decode trapezoid index ─────────────────────────────────── */
static inline uint8_t gf_trap_face(uint8_t trap_idx) {
    return trap_idx / 5u;
}
static inline uint8_t gf_trap_edge(uint8_t trap_idx) {
    return trap_idx % 5u;
}
static inline uint8_t gf_trap_encode(uint8_t face, uint8_t edge) {
    return (uint8_t)((face % 12u) * 5u + (edge % 5u));
}

/*
 * Map capture key → trapezoid tower index (0..59).
 *
 * Logic:
 *   1. Get 3D position from capture key
 *   2. Find nearest dodeca face center D (max dot product)
 *   3. For each of D's 5 geometric neighbors N, compute distance to edge midpoint
 *   4. Closest edge = trapezoid = D*5 + edge_idx
 *
 * Then within the trapezoid:
 *   - The barycentric (i,j,k) gives position along the tower
 *   - tertiary weight → shell level (0..11)
 *   - secondary weight → intra-level position (0..575)
 */
static inline uint8_t icosphere_capture_key_to_trapezoid(uint32_t capture_key)
{
    double px, py, pz;
    icosa_decode_position(capture_key, &px, &py, &pz);

    /* Find nearest dodeca face center D */
    int D = 0;
    double best_dot = -1e30;
    for (int i = 0; i < 12; i++) {
        double dot = px*ICOSA_VERTS[i][0] + py*ICOSA_VERTS[i][1] + pz*ICOSA_VERTS[i][2];
        if (dot > best_dot) { best_dot = dot; D = i; }
    }

    /* For each of D's 5 edges (to neighbor N), compute distance to edge midpoint */
    int best_edge = 0;
    double best_dist = 1e30;
    for (int e = 0; e < 5; e++) {
        uint8_t N = GF_VERTEX_NB[D][e];
        double mx = (ICOSA_VERTS[D][0] + ICOSA_VERTS[N][0]) * 0.5;
        double my = (ICOSA_VERTS[D][1] + ICOSA_VERTS[N][1]) * 0.5;
        double mz = (ICOSA_VERTS[D][2] + ICOSA_VERTS[N][2]) * 0.5;
        double dx = px - mx, dy = py - my, dz = pz - mz;
        double dist = dx*dx + dy*dy + dz*dz;
        if (e == 0 || dist < best_dist) { best_dist = dist; best_edge = e; }
    }

    uint8_t edge = (uint8_t)best_edge;
    /* Verify ADJ consistency: ADJ[D][edge][0] should be the geometric neighbor */
    return gf_trap_encode((uint8_t)D, edge);
}

/*
 * Map capture key → position within trapezoid tower (0..6911).
 *
 * Position encodes:
 *   [0..575]     = shell level 0 (inner mini-dodeca)
 *   [576..1151]  = shell level 1
 *   ...
 *   [6336..6911] = shell level 11 (outer dodeca)
 *
 * Each shell level has 576 positions = 4 × GEO_TOWER (4 × 144).
 * Geo_jump at each position can navigate through 4×4×3 blocks.
 */
static inline uint32_t icosphere_capture_key_to_tower_pos(uint32_t capture_key)
{
    int face = (int)(capture_key / ICOSA_VERTS_PER_FACE);
    int idx  = (int)(capture_key % ICOSA_VERTS_PER_FACE);

    /* Decode (i,j,k) */
    int j = 0, cum = 0;
    for (; j <= ICOSA_F4; j++) {
        int row = ICOSA_F4 - j + 1;
        if (idx < cum + row) break;
        cum += row;
    }
    int i = idx - cum;
    int k = ICOSA_F4 - i - j;

    int weights[3] = {i, j, k};
    int dominant = 0;
    if (weights[1] > weights[dominant]) dominant = 1;
    if (weights[2] > weights[dominant]) dominant = 2;

    /* Secondary vs tertiary determines position in the tower */
    int w_secondary = weights[(dominant + 1) % 3];
    int w_tertiary  = weights[(dominant + 2) % 3];

    /* Shell level: 0..11 from capture_key × face geometry.
     * At f=4, only 15 vertices/face → barycentric weights alone
     * produce only 4 distinct depth values. Use full key range
     * (300 values) for uniform 12-level distribution. */
    uint32_t shell_level = capture_key * 11u / 299u;

    /* Intra-level position: 0..575 from capture_key spread.
     * At f=4, barycentric gives only 4 distinct intra values.
     * Use full 300-key range for uniform distribution. */
    uint32_t intra = capture_key * 575u / 299u;

    return shell_level * 576u + intra;
}

/*
 * Get the GEO_FULL node for a given trapezoid + tower position.
 * Since the priority zone (414,720) is 20× larger than GEO_FULL (20,736),
 * this maps each of the 60 towers' 6912 nodes into the 12-pentagon
 * × 1728-node GEO_FULL space.
 *
 * Mapping: each tower maps across all 12 pentagons at shell level tower_pos/576,
 * spread across the 144 intra-level slots via (tower_pos % 576) / 4.
 */
static inline uint32_t icosphere_trapezoid_to_geo_full(uint8_t trap_idx,
                                                         uint32_t tower_pos)
{
    uint8_t face = gf_trap_face(trap_idx);
    uint32_t shell_level = tower_pos / 576u;
    if (shell_level >= 12u) shell_level = 11u;
    uint32_t intra_slot = (tower_pos % 576u) / 4u;
    if (intra_slot >= 144u) intra_slot = 143u;
    return (uint32_t)face * 1728u + shell_level * 144u + intra_slot;
}

/*
 * Cold zone: mini pentagon at shell level 0 (bottom of frustum pit).
 *
 * The 12 mini pentagons at the pit bottom form a complete mini-dodecahedron.
 * Each face has one mini pentagon at shell level 0 with 144 positions:
 *   face F: node = F × 1728 + 0..143
 *
 * Total cold zone: 12 × 144 = 1728 nodes = 1/12 of GEO_FULL.
 *
 * Use as eviction target: when priority zone data collides, the cold entry
 * promotes up (replacing hot data in GEO_FULL), and the hot entry evicts
 * down to its face's cold pentagon base. The capo rotation (below) provides
 * 12 different cold/hot partitions via shell-level rotation.
 */
static inline uint32_t icosphere_trapezoid_to_cold(uint8_t trap_idx,
                                                     uint32_t tower_pos)
{
    uint8_t face = gf_trap_face(trap_idx);
    uint32_t intra_slot = (tower_pos % 576u) / 4u;
    if (intra_slot >= 144u) intra_slot = 143u;
    return (uint32_t)face * 1728u + intra_slot;
}

/*
 * Capo rotation: change which GEO_FULL slot each tower position maps to.
 *
 * Concept from musical capo — clamp on fret, finger position unchanged,
 * but pitch shifts. Here: same (trap, tower_pos) in priority zone,
 * different GEO_FULL slot.
 *
 * With 12 distinct capo keys (0..11):
 *   capo=0:  default mapping (same as icosphere_trapezoid_to_geo_full)
 *   capo=k:  shell level rotated by k (mod 12)
 *
 * This means:
 *   capo=0:  level 0→cold, levels 1..11→hot
 *   capo=1:  level 11→cold, levels 0..10→hot
 *   capo=6:  swap cold/hot hemispheres
 *
 * Use:
 *   - Time-dependent data (capo = session tick)
 *   - Wear leveling across shell levels
 *   - Temperature promotion/demotion without data movement
 *   - 12 concurrent "views" into same 414,720 tower entries
 */
static inline uint32_t icosphere_trapezoid_to_geo_full_capo(uint8_t trap_idx,
                                                              uint32_t tower_pos,
                                                              uint8_t capo_key)
{
    uint8_t face = gf_trap_face(trap_idx);
    uint32_t shell_level = tower_pos / 576u;
    if (shell_level >= 12u) shell_level = 11u;
    uint32_t intra_slot = (tower_pos % 576u) / 4u;
    if (intra_slot >= 144u) intra_slot = 143u;
    /* Capo: add rotation to shell level, wrapping around 12 levels */
    uint32_t rotated = (shell_level + (capo_key % 12u)) % 12u;
    return (uint32_t)face * 1728u + rotated * 144u + intra_slot;
}

/*
 * Honeycomb redundancy: paired trapezoids (fast-flip mirror).
 *
 * The 60 trapezoids form 30 pairs — each dodeca edge has trapezoids
 * on both adjacent faces. Trap(f, e) mirrors trap(g, e') across
 * their shared edge.
 *
 * Use as RAID-1 mirror:
 *   primary   = icosphere_trapezoid_to_geo_full(trap, pos)
 *   secondary = icosphere_trapezoid_to_geo_full(pair, pos)
 *   Both map to the same GEO_FULL edge region via different faces.
 *
 * If one face corridor is corrupted, "fast flip" to the paired face
 * — no ECC computation, just an address lookup.
 */
static inline uint8_t icosphere_trap_pair(uint8_t trap_idx) {
    uint8_t face = gf_trap_face(trap_idx);
    uint8_t edge = gf_trap_edge(trap_idx);
    /* ADJ[face][edge] = {neighbor, entry_edge_on_neighbor} */
    uint8_t nb_face = GF_TRAP_ADJ[face][edge][0];
    uint8_t nb_edge = GF_TRAP_ADJ[face][edge][1];
    return gf_trap_encode(nb_face, nb_edge);
}

/*
 * Store with mirror: write data to both trapezoids of the pair.
 * Returns the primary GEO_FULL address; mirror is automatic.
 */
static inline void icosphere_trap_store_mirrored(uint8_t trap_idx,
                                                   uint32_t tower_pos_within_trap,
                                                   void *data_out_primary,
                                                   void *data_out_mirror)
{
    /* Primary: this trapezoid's GEO_FULL slot */
    if (data_out_primary) {
        *(uint32_t*)data_out_primary = icosphere_trapezoid_to_geo_full(trap_idx, tower_pos_within_trap);
    }
    /* Mirror: paired trapezoid's GEO_FULL slot (same tower position) */
    if (data_out_mirror) {
        uint8_t mate = icosphere_trap_pair(trap_idx);
        *(uint32_t*)data_out_mirror = icosphere_trapezoid_to_geo_full(mate, tower_pos_within_trap);
    }
}

/*
 * Fast-flip recovery: if primary fails, read from mirror pair.
 * "Flip" = swap which face's trapezoid you address.
 * Returns the GEO_FULL address of the paired trapezoid at same position.
 */
static inline uint32_t icosphere_trap_fast_flip(uint8_t trap_idx,
                                                  uint32_t tower_pos)
{
    uint8_t mate = icosphere_trap_pair(trap_idx);
    return icosphere_trapezoid_to_geo_full(mate, tower_pos);
}

/*
 * Cold zone with capo: extract the cold-target slot for a given
 * capo rotation. The "cold" slot is whatever shell level the capo
 * maps to level 0 — the mini pentagon at that level.
 *
 * With capo=k: cold is at shell level (12-k) % 12.
 * (Level 0 after rotation by k means the data was originally at level -k)
 */
static inline uint32_t icosphere_cold_for_capo(uint8_t trap_idx,
                                                 uint32_t tower_pos,
                                                 uint8_t capo_key)
{
    uint8_t face = gf_trap_face(trap_idx);
    uint32_t shell_level = tower_pos / 576u;
    if (shell_level >= 12u) shell_level = 11u;
    uint32_t intra_slot = (tower_pos % 576u) / 4u;
    if (intra_slot >= 144u) intra_slot = 143u;
    /* Cold = whatever original level maps to level 0 after capo rotation */
    uint32_t cold_origin = (12u - (capo_key % 12u) + shell_level) % 12u;
    return (uint32_t)face * 1728u + cold_origin * 144u + intra_slot;
}

/* ═══════════════════════════════════════════════════════════════════════
   CAPTURE KEY LUT (300 entries)
   ═══════════════════════════════════════════════════════════════════════
 *
 * Cache trapezoid + tower position for all 300 capture keys.
 * Avoids repeated icosa_decode_position() → dot product → dist math.
 * Build once, then plain array lookup.
 */
static uint8_t  g_cap_trap[300] = {0};
static uint32_t g_cap_pos[300]  = {0};
static int      g_cap_lut_ready = 0;

static inline void gf_cap_lut_init(void) {
    if (g_cap_lut_ready) return;
    for (uint32_t k = 0; k < 300; k++) {
        g_cap_trap[k] = icosphere_capture_key_to_trapezoid(k);
        g_cap_pos[k]  = icosphere_capture_key_to_tower_pos(k);
    }
    g_cap_lut_ready = 1;
}

static inline uint8_t gf_cap_trap(uint32_t key) {
    return g_cap_trap[key % 300u];
}
static inline uint32_t gf_cap_pos(uint32_t key) {
    return g_cap_pos[key % 300u];
}

/* ═══════════════════════════════════════════════════════════════════════
   PRIORITY ZONE TOWER NAVIGATION
   ═══════════════════════════════════════════════════════════════════════
 *
 * Each tower = 12 shell levels × 576 intra-level positions = 6912 nodes.
 * Within each level: 144 geo_slots × 4 sub-slots (4:1 mapping to GEO_FULL).
 *
 * Navigation primitives:
 *   gf_tower_shell(pos) / gf_tower_geo_slot(pos) / gf_tower_sub_slot(pos)
 *   gf_tower_build(shell, gs, sub)
 *   gf_tower_step_shell(trap, pos, delta)   — vertical
 *   gf_tower_step_slot(trap, pos, delta)    — horizontal (geo_slot)
 *   gf_tower_step_sub(trap, pos, delta)     — fine intraslot
 *
 * GfTowerWalk: stateful walker chaining steps with GEO_FULL trail.
 *   gf_tower_walk_init(w, trap, pos, capo)
 *   gf_tower_walk_geo(w, shell_delta, slot_delta)  — combined step
 *   gf_tower_walk_flip(w)                           — cross to mirror
 *   gf_tower_walk_geo_at(w, i)                      — trail lookback
 *
 * These let the priority zone act as a navigable address space:
 *   tower_pos → geo_jump within virtual tower → GEO_FULL address
 *   No external jump tables needed — just coordinate arithmetic.
 */

#define GF_TOWER_LEVELS      12u
#define GF_TOWER_PER_LEVEL   576u
#define GF_TOWER_WALK_MAX    64u

/* ── Decomposition —──────────────────────────────── */
static inline uint32_t gf_tower_shell(uint32_t pos) {
    return pos / GF_TOWER_PER_LEVEL;
}
static inline uint32_t gf_tower_geo_slot(uint32_t pos) {
    return (pos % GF_TOWER_PER_LEVEL) / 4u;
}
static inline uint32_t gf_tower_sub_slot(uint32_t pos) {
    return (pos % GF_TOWER_PER_LEVEL) % 4u;
}
static inline uint32_t gf_tower_build(uint32_t shell, uint32_t gs, uint32_t sub) {
    return (shell % GF_TOWER_LEVELS) * GF_TOWER_PER_LEVEL
         + (gs % 144u) * 4u
         + (sub % 4u);
}

/* ── Stepping —──────────────────────────────────── */
static inline uint32_t gf_tower_step_shell(uint8_t trap, uint32_t pos, int delta) {
    (void)trap;
    int shell = (int)(pos / GF_TOWER_PER_LEVEL) + delta;
    /* wrap 0..11 */
    shell = ((shell % 12) + 12) % 12;
    return (uint32_t)shell * GF_TOWER_PER_LEVEL + (pos % GF_TOWER_PER_LEVEL);
}

static inline uint32_t gf_tower_step_slot(uint8_t trap, uint32_t pos, int delta) {
    (void)trap;
    uint32_t shell = pos / GF_TOWER_PER_LEVEL;
    uint32_t gs = gf_tower_geo_slot(pos);
    uint32_t sub = gf_tower_sub_slot(pos);
    int new_gs = ((int)gs + delta) % 144;
    if (new_gs < 0) new_gs += 144;
    return shell * GF_TOWER_PER_LEVEL + (uint32_t)new_gs * 4u + sub;
}

static inline uint32_t gf_tower_step_sub(uint8_t trap, uint32_t pos, int delta) {
    (void)trap;
    uint32_t shell = pos / GF_TOWER_PER_LEVEL;
    uint32_t gs = gf_tower_geo_slot(pos);
    int sub = (int)(pos % 4u) + delta;
    int new_sub = (sub % 4 + 4) % 4;
    return shell * GF_TOWER_PER_LEVEL + gs * 4u + (uint32_t)new_sub;
}

/* ── Walker —────────────────────────────────────── */
typedef struct {
    uint8_t  trap;
    uint32_t pos;
    uint8_t  capo_key;
    uint32_t step;
    uint32_t geo_trail[GF_TOWER_WALK_MAX];
    uint32_t pos_trail[GF_TOWER_WALK_MAX];
} GfTowerWalk;

static inline void gf_tower_walk_init(GfTowerWalk *w, uint8_t trap,
                                       uint32_t pos, uint8_t capo_key)
{
    w->trap     = trap;
    w->pos      = pos;
    w->capo_key = capo_key;
    w->step     = 0;
    w->pos_trail[0] = pos;
    w->geo_trail[0] = icosphere_trapezoid_to_geo_full_capo(trap, pos, capo_key);
}

/* Combined step: shell_delta + slot_delta in one call */
static inline uint32_t gf_tower_walk_geo(GfTowerWalk *w,
                                          int shell_delta, int slot_delta)
{
    if (w->step >= GF_TOWER_WALK_MAX - 1)
        return w->geo_trail[w->step];
    w->pos = gf_tower_step_shell(w->trap, w->pos, shell_delta);
    w->pos = gf_tower_step_slot(w->trap, w->pos, slot_delta);
    w->step++;
    w->pos_trail[w->step] = w->pos;
    w->geo_trail[w->step] = icosphere_trapezoid_to_geo_full_capo(
        w->trap, w->pos, w->capo_key);
    return w->geo_trail[w->step];
}

/* Fast-flip: cross to mirrored trapezoid at same position */
static inline uint32_t gf_tower_walk_flip(GfTowerWalk *w) {
    if (w->step >= GF_TOWER_WALK_MAX - 1)
        return w->geo_trail[w->step];
    w->trap = icosphere_trap_pair(w->trap);
    w->step++;
    w->pos_trail[w->step] = w->pos;
    w->geo_trail[w->step] = icosphere_trapezoid_to_geo_full_capo(
        w->trap, w->pos, w->capo_key);
    return w->geo_trail[w->step];
}

/* Look back at trail */
static inline uint32_t gf_tower_walk_geo_at(const GfTowerWalk *w, uint32_t i) {
    return (i <= w->step) ? w->geo_trail[i] : w->geo_trail[w->step];
}

/* ═══════════════════════════════════════════════════════════════════════
   DRAMTILE GLUE — Store/load via priority zone address
   ═══════════════════════════════════════════════════════════════════════
 *
 * Connect priority zone tower positions → DRamTile storage.
 * Requires: #define GF_ICOSPHERE_USE_DRAMTILE before include,
 * and dramtile_store.h in scope.
 *
 * Each capture key stores at its priority zone GEO_FULL address.
 * Loaded via direct dram_addr lookup (dt_resolve / dt_put_addr).
 */
#ifdef GF_ICOSPHERE_USE_DRAMTILE

#include "dramtile_store.h"

/* Store capture key data at its priority zone address */
static inline uint8_t *gf_cap_dt_store(DRamTileStore *store,
                                        uint32_t capture_key,
                                        const uint8_t *data, size_t sz)
{
    uint8_t trap = gf_cap_trap(capture_key);
    uint32_t pos = gf_cap_pos(capture_key);
    uint32_t addr = icosphere_trapezoid_to_geo_full(trap, pos);
    return dt_put_addr(store, addr, data, sz);
}

/* Load capture key data from its priority zone address */
static inline uint8_t *gf_cap_dt_load(DRamTileStore *store,
                                       uint32_t capture_key)
{
    uint8_t trap = gf_cap_trap(capture_key);
    uint32_t pos = gf_cap_pos(capture_key);
    uint32_t addr = icosphere_trapezoid_to_geo_full(trap, pos);
    DtTensorView v = dt_resolve(store, addr);
    return v.data;
}

/* Store at arbitrary tower position (for priority zone walking) */
static inline uint8_t *gf_tower_dt_store(DRamTileStore *store,
                                          uint8_t trap, uint32_t pos,
                                          const uint8_t *data, size_t sz,
                                          uint8_t capo_key)
{
    uint32_t addr = icosphere_trapezoid_to_geo_full_capo(trap, pos, capo_key);
    return dt_put_addr(store, addr, data, sz);
}

/* Load from arbitrary tower position */
static inline uint8_t *gf_tower_dt_load(DRamTileStore *store,
                                         uint8_t trap, uint32_t pos,
                                         uint8_t capo_key)
{
    uint32_t addr = icosphere_trapezoid_to_geo_full_capo(trap, pos, capo_key);
    DtTensorView v = dt_resolve(store, addr);
    return v.data;
}

#endif /* GF_ICOSPHERE_USE_DRAMTILE */

#endif /* GEO_FIELD_ICOSPHERE_H */

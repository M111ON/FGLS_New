/*
 * goldberg_adj.h — Goldberg Sphere Adjacency LUT + fc_neighbor wire
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Wires goldberg_shutter.h geometry into frustum_coord.h's
 * fc_goldberg_neighbor_stub, replacing NONE with real adjacency.
 *
 * ── Tile ID scheme ──────────────────────────────────────────────────
 *
 *   tile  0..11  = 12 pentagons  (GS_PENTAGON_COUNT)
 *   tile 12..41  = 30 edge hexes (icosahedron edges = shared penta boundaries)
 *   tile 42..61  = 20 face hexes (icosahedron face centers)
 *   tile 62..65  =  4 extra tiles (POGLS 54-fabric extension, NONE neighbors)
 *   Total = 66 = 12 + 54 hexes (FGLS_DIAMOND_COUNT)
 *
 * ── Topology source ──────────────────────────────────────────────────
 *
 *   GP(1,1) Goldberg sphere (truncated icosahedron):
 *     12 pentagons at icosahedron vertices
 *     30 hex tiles  at icosahedron edges (one hex per shared edge)
 *     20 hex tiles  at icosahedron face centers
 *     + 4 POGLS-specific extension tiles (tile 62..65)
 *
 *   Icosahedron edge adjacency drives pentagon neighbor assignment:
 *     Pentagon p has 5 icosa-edge-adjacent pentagons q0..q4
 *     One hex tile sits on each edge (p,qi) → pentagon neighbors are hex tiles
 *     FACE_NZ (face 5) = NONE for pentagons (5-sided, not 6)
 *
 * ── Face assignment ──────────────────────────────────────────────────
 *
 *   FaceId 0..5 = FACE_PX, NX, PY, NY, PZ, NZ
 *   GS_ADJ_LUT[tile_id][face] = neighbor tile_id (or 0xFF = NONE)
 *   Pentagon tiles: face 5 (FACE_NZ) always 0xFF
 *   Hex tiles: all 6 faces used where topology permits
 *
 * ── Usage ────────────────────────────────────────────────────────────
 *
 *   #include "goldberg_adj.h"
 *
 *   // replace stub in fc_adapter_init:
 *   a->neighbor = fc_goldberg_neighbor;   // this file's implementation
 *
 *   // or use directly:
 *   uint64_t nbr = fc_goldberg_neighbor(tile_id, FACE_PX, 0);
 *
 * ── Constants ────────────────────────────────────────────────────────
 *
 *   GS_TILE_COUNT   = 66   (12 penta + 54 hex)
 *   GS_PENTA_TILES  = 12
 *   GS_HEX_TILES    = 54   (= FGLS_DIAMOND_COUNT)
 *   GS_TILE_NONE    = 0xFF (no neighbor at this face)
 *   N parameter ignored (Goldberg topology is fixed, not parametric)
 *
 * Sacred: GS_TILE_COUNT=66, GS_HEX_TILES=54=2×3³ — FROZEN
 * No malloc. No float. No heap. O(1) LUT lookup.
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef GOLDBERG_ADJ_H
#define GOLDBERG_ADJ_H

#include <stdint.h>
#include <stdbool.h>
#include "frustum_coord.h"   /* FaceId, FC_NEIGHBOR_NONE, fc_neighbor_fn */

/* ══════════════════════════════════════════════════════════════
 * FROZEN CONSTANTS
 * ══════════════════════════════════════════════════════════════ */

#define GS_TILE_COUNT    66u   /* 12 penta + 54 hex                    */
#define GS_PENTA_TILES   12u   /* pentagon tiles 0..11                  */
#define GS_HEX_TILES     54u   /* hex tiles 12..65 (= FGLS_DIAMOND_COUNT)*/
#define GS_TILE_NONE    0xFFu  /* no neighbor at this face              */
#define GS_FACE_COUNT     6u   /* uniform 6-face interface              */

/* ── Tile type helpers ─────────────────────────────────────── */

static inline bool gs_tile_is_pentagon(uint8_t tile_id)
{
    return tile_id < GS_PENTA_TILES;
}

static inline bool gs_tile_is_hex(uint8_t tile_id)
{
    return tile_id >= GS_PENTA_TILES && tile_id < GS_TILE_COUNT;
}

/* hex_id: 0..53 (diamond_id in frustum fabric) */
static inline uint8_t gs_tile_to_hex_id(uint8_t tile_id)
{
    return (uint8_t)(tile_id - GS_PENTA_TILES);   /* 12..65 → 0..53 */
}

static inline uint8_t gs_hex_id_to_tile(uint8_t hex_id)
{
    return (uint8_t)(hex_id + GS_PENTA_TILES);    /* 0..53 → 12..65 */
}

/* ══════════════════════════════════════════════════════════════
 * ADJACENCY LUT — GS_ADJ_LUT[tile_id][face] = neighbor_tile_id
 *
 * Source: GP(1,1) icosahedral Goldberg sphere
 *   Pentagons: 12 icosahedron vertices, 5 hex neighbors, face5=NONE
 *   Edge hexes: 30 tiles at icosa edges, 6 neighbors (2 penta + 4 hex)
 *   Face hexes: 20 tiles at icosa face centers, 6 hex neighbors
 *   Extra tiles: 4 POGLS extension tiles, all NONE
 * ══════════════════════════════════════════════════════════════ */

static const uint8_t GS_ADJ_LUT[GS_TILE_COUNT][GS_FACE_COUNT] = {
    /* ── Pentagons 0..11 (face 5 = NONE, 5-sided) ── */
    { 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0xFF },  /* tile  0 pentagon */
    { 0x0C, 0x11, 0x12, 0x13, 0x14, 0xFF },  /* tile  1 pentagon */
    { 0x0D, 0x11, 0x15, 0x16, 0x17, 0xFF },  /* tile  2 pentagon */
    { 0x12, 0x18, 0x19, 0x1A, 0x1B, 0xFF },  /* tile  3 pentagon */
    { 0x15, 0x1C, 0x1D, 0x1E, 0x1F, 0xFF },  /* tile  4 pentagon */
    { 0x0E, 0x20, 0x21, 0x22, 0x23, 0xFF },  /* tile  5 pentagon */
    { 0x0F, 0x16, 0x1C, 0x20, 0x24, 0xFF },  /* tile  6 pentagon */
    { 0x10, 0x13, 0x18, 0x21, 0x25, 0xFF },  /* tile  7 pentagon */
    { 0x14, 0x17, 0x19, 0x1D, 0x26, 0xFF },  /* tile  8 pentagon */
    { 0x1A, 0x1E, 0x26, 0x27, 0x28, 0xFF },  /* tile  9 pentagon */
    { 0x1F, 0x22, 0x24, 0x27, 0x29, 0xFF },  /* tile 10 pentagon */
    { 0x1B, 0x23, 0x25, 0x28, 0x29, 0xFF },  /* tile 11 pentagon */
    /* ── Edge hexes 12..41 (30 tiles, up to 6 neighbors) ── */
    { 0x00, 0x01, 0x2A, 0x2B, 0x0D, 0x0E },  /* tile 12 edge_hex */
    { 0x00, 0x02, 0x2A, 0x2C, 0x0C, 0x0E },  /* tile 13 edge_hex */
    { 0x00, 0x05, 0x2D, 0x2E, 0x0C, 0x0D },  /* tile 14 edge_hex */
    { 0x00, 0x06, 0x2C, 0x2D, 0x0C, 0x0D },  /* tile 15 edge_hex */
    { 0x00, 0x07, 0x2B, 0x2E, 0x0C, 0x0D },  /* tile 16 edge_hex */
    { 0x01, 0x02, 0x2A, 0x2F, 0x0C, 0x0D },  /* tile 17 edge_hex */
    { 0x01, 0x03, 0x30, 0x31, 0x0C, 0x11 },  /* tile 18 edge_hex */
    { 0x01, 0x07, 0x2B, 0x30, 0x0C, 0x10 },  /* tile 19 edge_hex */
    { 0x01, 0x08, 0x2F, 0x31, 0x0C, 0x11 },  /* tile 20 edge_hex */
    { 0x02, 0x04, 0x32, 0x33, 0x0D, 0x11 },  /* tile 21 edge_hex */
    { 0x02, 0x06, 0x2C, 0x32, 0x0D, 0x0F },  /* tile 22 edge_hex */
    { 0x02, 0x08, 0x2F, 0x33, 0x0D, 0x11 },  /* tile 23 edge_hex */
    { 0x03, 0x07, 0x30, 0x34, 0x10, 0x12 },  /* tile 24 edge_hex */
    { 0x03, 0x08, 0x31, 0x35, 0x12, 0x14 },  /* tile 25 edge_hex */
    { 0x03, 0x09, 0x35, 0x36, 0x12, 0x18 },  /* tile 26 edge_hex */
    { 0x03, 0x0B, 0x34, 0x36, 0x12, 0x18 },  /* tile 27 edge_hex */
    { 0x04, 0x06, 0x32, 0x37, 0x0F, 0x15 },  /* tile 28 edge_hex */
    { 0x04, 0x08, 0x33, 0x38, 0x14, 0x15 },  /* tile 29 edge_hex */
    { 0x04, 0x09, 0x38, 0x39, 0x15, 0x1A },  /* tile 30 edge_hex */
    { 0x04, 0x0A, 0x37, 0x39, 0x15, 0x1C },  /* tile 31 edge_hex */
    { 0x05, 0x06, 0x2D, 0x3A, 0x0E, 0x0F },  /* tile 32 edge_hex */
    { 0x05, 0x07, 0x2E, 0x3B, 0x0E, 0x10 },  /* tile 33 edge_hex */
    { 0x05, 0x0A, 0x3A, 0x3C, 0x0E, 0x1F },  /* tile 34 edge_hex */
    { 0x05, 0x0B, 0x3B, 0x3C, 0x0E, 0x1B },  /* tile 35 edge_hex */
    { 0x06, 0x0A, 0x37, 0x3A, 0x0F, 0x16 },  /* tile 36 edge_hex */
    { 0x07, 0x0B, 0x34, 0x3B, 0x10, 0x13 },  /* tile 37 edge_hex */
    { 0x08, 0x09, 0x35, 0x38, 0x14, 0x17 },  /* tile 38 edge_hex */
    { 0x09, 0x0A, 0x39, 0x3D, 0x1A, 0x1E },  /* tile 39 edge_hex */
    { 0x09, 0x0B, 0x36, 0x3D, 0x1A, 0x1B },  /* tile 40 edge_hex */
    { 0x0A, 0x0B, 0x3C, 0x3D, 0x1B, 0x1F },  /* tile 41 edge_hex */
    /* ── Face hexes 42..61 (20 tiles, 6 neighbors each) ── */
    { 0x0C, 0x11, 0x0D, 0x2B, 0x2C, 0x2F },  /* tile 42 face_hex */
    { 0x0C, 0x13, 0x10, 0x2A, 0x2E, 0x30 },  /* tile 43 face_hex */
    { 0x0D, 0x16, 0x0F, 0x2A, 0x2D, 0x32 },  /* tile 44 face_hex */
    { 0x0E, 0x20, 0x0F, 0x2C, 0x2E, 0x3A },  /* tile 45 face_hex */
    { 0x0E, 0x21, 0x10, 0x2B, 0x2D, 0x3B },  /* tile 46 face_hex */
    { 0x11, 0x17, 0x14, 0x2A, 0x31, 0x33 },  /* tile 47 face_hex */
    { 0x12, 0x18, 0x13, 0x2B, 0x31, 0x34 },  /* tile 48 face_hex */
    { 0x12, 0x19, 0x14, 0x2F, 0x30, 0x35 },  /* tile 49 face_hex */
    { 0x15, 0x1C, 0x16, 0x2C, 0x33, 0x37 },  /* tile 50 face_hex */
    { 0x15, 0x1D, 0x17, 0x2F, 0x32, 0x38 },  /* tile 51 face_hex */
    { 0x18, 0x25, 0x1B, 0x30, 0x36, 0x3B },  /* tile 52 face_hex */
    { 0x19, 0x26, 0x1A, 0x31, 0x36, 0x38 },  /* tile 53 face_hex */
    { 0x1A, 0x28, 0x1B, 0x34, 0x35, 0x3D },  /* tile 54 face_hex */
    { 0x1C, 0x24, 0x1F, 0x32, 0x39, 0x3A },  /* tile 55 face_hex */
    { 0x1D, 0x26, 0x1E, 0x33, 0x35, 0x39 },  /* tile 56 face_hex */
    { 0x1E, 0x27, 0x1F, 0x37, 0x38, 0x3D },  /* tile 57 face_hex */
    { 0x20, 0x24, 0x22, 0x2D, 0x37, 0x3C },  /* tile 58 face_hex */
    { 0x21, 0x25, 0x23, 0x2E, 0x34, 0x3C },  /* tile 59 face_hex */
    { 0x22, 0x29, 0x23, 0x3A, 0x3B, 0x3D },  /* tile 60 face_hex */
    { 0x27, 0x29, 0x28, 0x36, 0x39, 0x3C },  /* tile 61 face_hex */
    /* ── Extra tiles 62..65 (POGLS 54-fabric extension) ── */
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },  /* tile 62 extra */
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },  /* tile 63 extra */
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },  /* tile 64 extra */
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },  /* tile 65 extra */
};

/* ══════════════════════════════════════════════════════════════
 * fc_goldberg_neighbor — drop-in replacement for stub
 *
 * Signature matches fc_neighbor_fn typedef exactly.
 * N parameter ignored (topology is fixed, not parametric).
 * block_id = tile_id (0..65)
 * Returns FC_NEIGHBOR_NONE (UINT64_MAX) when LUT says 0xFF.
 * ══════════════════════════════════════════════════════════════ */

static inline uint64_t fc_goldberg_neighbor(uint64_t block_id,
                                             FaceId   face,
                                             uint8_t  N)
{
    (void)N;   /* Goldberg topology fixed — N unused */

    const uint8_t tile = (uint8_t)(block_id & 0xFFu);
    if (tile >= GS_TILE_COUNT) return FC_NEIGHBOR_NONE;

    const uint8_t f = (uint8_t)face;
    if (f >= GS_FACE_COUNT)   return FC_NEIGHBOR_NONE;

    const uint8_t nbr = GS_ADJ_LUT[tile][f];
    return (nbr == GS_TILE_NONE) ? FC_NEIGHBOR_NONE : (uint64_t)nbr;
}

/* ══════════════════════════════════════════════════════════════
 * fc_adapter_init_goldberg — wire LUT into FcAdapter
 *
 * Replaces stub registration in fc_adapter_init().
 * Call this instead of fc_adapter_init(&a, FC_TOPO_GOLDBERG, N)
 * to get real adjacency instead of all-NONE.
 * ══════════════════════════════════════════════════════════════ */

static inline void fc_adapter_init_goldberg(FcAdapter *a)
{
    a->topo     = FC_TOPO_GOLDBERG;
    a->N        = 0u;   /* unused for Goldberg */
    a->neighbor = fc_goldberg_neighbor;
}

/* ══════════════════════════════════════════════════════════════
 * INSPECT HELPERS
 * ══════════════════════════════════════════════════════════════ */

/*
 * gs_adj_count: number of real (non-NONE) neighbors for tile_id
 *   Pentagon → always 5
 *   Hex      → 6 (or fewer for extra tiles)
 */
static inline uint8_t gs_adj_count(uint8_t tile_id)
{
    if (tile_id >= GS_TILE_COUNT) return 0u;
    uint8_t n = 0u;
    for (uint8_t f = 0u; f < GS_FACE_COUNT; f++)
        if (GS_ADJ_LUT[tile_id][f] != GS_TILE_NONE) n++;
    return n;
}

/*
 * gs_adj_is_symmetric: true if A→B implies B→A (sanity check)
 *   Not guaranteed by LUT construction — call in verify only.
 */
static inline bool gs_adj_is_symmetric(uint8_t a, uint8_t b)
{
    /* check a→b */
    bool ab = false;
    for (uint8_t f = 0u; f < GS_FACE_COUNT; f++)
        if (GS_ADJ_LUT[a][f] == b) { ab = true; break; }
    if (!ab) return false;

    /* check b→a */
    for (uint8_t f = 0u; f < GS_FACE_COUNT; f++)
        if (GS_ADJ_LUT[b][f] == a) return true;
    return false;
}

/* ══════════════════════════════════════════════════════════════
 * VERIFY
 * ══════════════════════════════════════════════════════════════ */

static inline int gs_adj_verify(void)
{
    /* tile counts */
    if (GS_TILE_COUNT != GS_PENTA_TILES + GS_HEX_TILES) return -1;

    /* pentagons: exactly 5 neighbors, face 5 = NONE */
    for (uint8_t p = 0u; p < GS_PENTA_TILES; p++) {
        if (gs_adj_count(p) != 5u)          return -2;
        if (GS_ADJ_LUT[p][5] != GS_TILE_NONE) return -3;   /* FACE_NZ = NONE */
    }

    /* all pentagon neighbors are hex tiles (not other pentagons) */
    for (uint8_t p = 0u; p < GS_PENTA_TILES; p++) {
        for (uint8_t f = 0u; f < 5u; f++) {
            uint8_t nbr = GS_ADJ_LUT[p][f];
            if (nbr == GS_TILE_NONE)    return -4;
            if (nbr < GS_PENTA_TILES)   return -5;  /* must be hex, not penta */
        }
    }

    /* edge hexes (12..41): at least 4 neighbors */
    for (uint8_t h = 12u; h < 42u; h++) {
        if (gs_adj_count(h) < 4u) return -6;
    }

    /* face hexes (42..61): exactly 6 neighbors */
    for (uint8_t h = 42u; h < 62u; h++) {
        if (gs_adj_count(h) != 6u) return -7;
    }

    /* extra tiles (62..65): 0 neighbors */
    for (uint8_t h = 62u; h < GS_TILE_COUNT; h++) {
        if (gs_adj_count(h) != 0u) return -8;
    }

    /* all neighbor values in range [0..65] or 0xFF */
    for (uint8_t t = 0u; t < GS_TILE_COUNT; t++) {
        for (uint8_t f = 0u; f < GS_FACE_COUNT; f++) {
            uint8_t nbr = GS_ADJ_LUT[t][f];
            if (nbr != GS_TILE_NONE && nbr >= GS_TILE_COUNT) return -9;
        }
    }

    /* fc_goldberg_neighbor: pentagon face 5 = FC_NEIGHBOR_NONE */
    if (fc_goldberg_neighbor(0u, FACE_NZ, 0u) != FC_NEIGHBOR_NONE) return -10;

    /* fc_goldberg_neighbor: penta 0 face 0 = tile 0x0C = 12 */
    if (fc_goldberg_neighbor(0u, FACE_PX, 0u) != 12u)              return -11;

    /* out-of-range tile: FC_NEIGHBOR_NONE */
    if (fc_goldberg_neighbor(99u, FACE_PX, 0u) != FC_NEIGHBOR_NONE) return -12;

    /* FcAdapter wiring */
    FcAdapter a;
    fc_adapter_init_goldberg(&a);
    if (a.topo != FC_TOPO_GOLDBERG)             return -13;
    if (fc_neighbor(&a, 0u, FACE_PX) != 12u)    return -14;
    if (fc_neighbor(&a, 0u, FACE_NZ) != FC_NEIGHBOR_NONE) return -15;

    /* hex_id roundtrip */
    if (gs_tile_to_hex_id(12u) != 0u)  return -16;
    if (gs_hex_id_to_tile(0u)  != 12u) return -17;
    if (gs_tile_to_hex_id(65u) != 53u) return -18;

    return 0;
}

#endif /* GOLDBERG_ADJ_H */

/*
 * frustum_coord.h — FrustumBlock Universal Coordinate Interface
 * ══════════════════════════════════════════════════════════════
 *
 * Core insight:
 *   FrustumBlock has 6 faces → normalizes to unit cube 1×1×1
 *   face_id 0..5 = { +X, -X, +Y, -Y, +Z, -Z }
 *   ANY topology with 6 addressable faces plugs in here.
 *   Middleware inside the block never sees outer topology.
 *
 * Topology adapters (this file):
 *   CUBE     — full impl, onion shell model, spatial index
 *   CYLINDER — full impl, streaming pipe, fibo_clock = angle
 *   GOLDBERG — stub, requires goldberg_shutter.h adjacency LUT
 *
 * Why CUBE is densest:
 *   slope ชนกันทุก edge → 100% packing, zero gap
 *   12 edges = 12 pentagon drains (Goldberg match)
 *   8 corners = 8 (genesis before-state)
 *   onion shell (2N+1)³ → sacred shells at N mod 3 = 1
 *
 * Sacred shell sequence (N mod 3 = 1):
 *   N= 1 → side= 3³ =     27 blocks   0.13 MB  (Rubik unit)
 *   N= 4 → side= 9³ =    729 blocks   3.40 MB
 *   N= 7 → side=15³ =   3375 blocks  15.76 MB
 *   N= 8 → side=17³ =   4913 blocks  22.94 MB  ← FRUSTUM BOUNDARY
 *   N=10 → side=21³ =   9261 blocks  43.24 MB
 *   N=13 → side=27³ =  19683 blocks  91.90 MB
 *   Shell 8 = 17³: inner(15³=3375) + wall(1538) = drain boundary
 *   wall thickness 17 = drain gap = same 17 as FrustumBlock header
 *
 * Block addressing (CUBE — NO manifest needed):
 *   position = (x,y,z), x,y,z ∈ -N..N
 *   shell_n  = max(|x|,|y|,|z|)   ← Chebyshev distance, derive only
 *   block_id = pack(x+N, y+N, z+N) into uint32 (6 bits per axis)
 *   offset   = block_id × FRUSTUM_BLOCK_BYTES  ← direct seek, O(1)
 *
 * Encoding limits (uint32, 6b+6b+6b = 18 bits):
 *   max axis value: 0..62 → max N=31, side=63
 *   max blocks: 63³ = 250,047 × 4896B = 1.19 GB
 *
 * Sacred numbers: FROZEN
 *   FRUSTUM_BLOCK_BYTES = 4896   (17×2×144)
 *   FRUSTUM_DRAIN_N     = 8      (shell 8 = 17³ boundary)
 *   FC_AXIS_BITS        = 6      (bits per axis in block_id)
 *   FC_AXIS_MASK        = 0x3F   (6 bits)
 *   FC_FACE_COUNT       = 6      (universal interface)
 *
 * No malloc. No float. No heap. Stateless O(1).
 * ══════════════════════════════════════════════════════════════
 */

#ifndef FRUSTUM_COORD_H
#define FRUSTUM_COORD_H

#include <stdint.h>
#include <stdbool.h>
#include "geo_dodeca_adj.h"

/* ══════════════════════════════════════════════════════════════
 * FROZEN CONSTANTS
 * ══════════════════════════════════════════════════════════════ */

#define FRUSTUM_BLOCK_BYTES  4896u   /* 17×2×144 — from frustum_layout_v2.h */
#define FRUSTUM_DRAIN_N         8u   /* shell 8 = 17³ = frustum boundary    */
#define FC_AXIS_BITS            6u   /* bits per axis in packed block_id     */
#define FC_AXIS_MASK         0x3Fu   /* 0b00111111                           */
#define FC_AXIS_MAX          62u     /* max value per axis (0..62, N≤31)     */
#define FC_FACE_COUNT           6u   /* universal 6-face interface           */
#define FC_NEIGHBOR_NONE UINT64_MAX  /* sentinel: no neighbor (boundary)     */

/* ══════════════════════════════════════════════════════════════
 * FACE ENUM — universal 6-face interface
 *
 * Topology mapping:
 *   Cube:     direct {±X,±Y,±Z}
 *   Cylinder: PZ=top cap, NZ=bottom cap, PX/NX/PY/NY=4 sectors
 *   Goldberg: PZ=inward, NZ=outward, PX..NY=4 of 5/6 neighbors
 *
 * FACE_PZ = +Z = World A = STORE  (genesis: 8 = before)
 * FACE_NZ = -Z = World B = DRAIN  (genesis: 9 = after)
 * ══════════════════════════════════════════════════════════════ */

typedef enum {
    FACE_PX = 0,   /* +X neighbor                          */
    FACE_NX = 1,   /* -X neighbor                          */
    FACE_PY = 2,   /* +Y neighbor                          */
    FACE_NY = 3,   /* -Y neighbor                          */
    FACE_PZ = 4,   /* +Z = top    = World A = STORE        */
    FACE_NZ = 5,   /* -Z = bottom = World B = DRAIN        */
} FaceId;

/* face_opposite: FACE_PX↔FACE_NX, FACE_PZ↔FACE_NZ etc.
 * Property: face_opposite(face_opposite(f)) == f  (self-inverse)
 * bit flip on bit0: PX(0)↔NX(1), PY(2)↔NY(3), PZ(4)↔NZ(5)
 */
static inline FaceId face_opposite(FaceId f)
{
    return (FaceId)((uint8_t)f ^ 1u);
}

/* ══════════════════════════════════════════════════════════════
 * NEIGHBOR FUNCTION TYPE — topology adapter interface
 *
 * Implement once per topology.
 * Returns FC_NEIGHBOR_NONE if no neighbor exists (boundary).
 * ══════════════════════════════════════════════════════════════ */

typedef uint64_t (*fc_neighbor_fn)(uint64_t block_id, FaceId face, uint8_t N);

/* ══════════════════════════════════════════════════════════════
 * CUBE — full implementation
 * ══════════════════════════════════════════════════════════════
 *
 * block_id = pack(xi, yi, zi)  where xi=x+N, yi=y+N, zi=z+N (0..2N)
 * packing:  bits [17:12]=xi, [11:6]=yi, [5:0]=zi
 * shell_n  = max(|xi-N|, |yi-N|, |zi-N|)  — derive, never store
 * offset   = block_id × 4896
 * ══════════════════════════════════════════════════════════════ */

/* pack/unpack ─────────────────────────────────────────────── */

static inline uint32_t fc_cube_pack(uint8_t xi, uint8_t yi, uint8_t zi)
{
    return ((uint32_t)(xi & FC_AXIS_MASK) << (FC_AXIS_BITS * 2u))
         | ((uint32_t)(yi & FC_AXIS_MASK) <<  FC_AXIS_BITS)
         |  (uint32_t)(zi & FC_AXIS_MASK);
}

static inline uint8_t fc_cube_xi(uint32_t id) { return (uint8_t)((id >> (FC_AXIS_BITS*2u)) & FC_AXIS_MASK); }
static inline uint8_t fc_cube_yi(uint32_t id) { return (uint8_t)((id >>  FC_AXIS_BITS)     & FC_AXIS_MASK); }
static inline uint8_t fc_cube_zi(uint32_t id) { return (uint8_t)( id                       & FC_AXIS_MASK); }

/* signed coords (x,y,z) from block_id + N ─────────────────── */

static inline int8_t fc_cube_x(uint32_t id, uint8_t N) { return (int8_t)((int)fc_cube_xi(id) - (int)N); }
static inline int8_t fc_cube_y(uint32_t id, uint8_t N) { return (int8_t)((int)fc_cube_yi(id) - (int)N); }
static inline int8_t fc_cube_z(uint32_t id, uint8_t N) { return (int8_t)((int)fc_cube_zi(id) - (int)N); }

/* shell_n = Chebyshev distance from center — O(1), no storage ─ */

static inline uint8_t fc_cube_shell(uint32_t id, uint8_t N)
{
    int ax = fc_cube_x(id, N); if (ax < 0) ax = -ax;
    int ay = fc_cube_y(id, N); if (ay < 0) ay = -ay;
    int az = fc_cube_z(id, N); if (az < 0) az = -az;
    int m  = ax > ay ? ax : ay;
    return (uint8_t)(m > az ? m : az);
}

/* is_sacred_shell: N mod 3 == 1 → (2N+1)³ has digit_sum=9 ─── */

static inline bool fc_cube_is_sacred_shell(uint8_t shell_n)
{
    return shell_n > 0u && (shell_n % 3u) == 1u;
}

/* is_drain_boundary: shell_n == FRUSTUM_DRAIN_N (shell 8 = 17³) */

static inline bool fc_cube_is_drain_boundary(uint8_t shell_n)
{
    return shell_n == FRUSTUM_DRAIN_N;
}

/* neighbor — returns FC_NEIGHBOR_NONE at boundary ──────────── */

static inline uint64_t fc_cube_neighbor(uint64_t block_id, FaceId face, uint8_t N)
{
    const uint32_t id = (uint32_t)block_id;
    const uint8_t  side = (uint8_t)(2u * N + 1u);

    uint8_t xi = fc_cube_xi(id);
    uint8_t yi = fc_cube_yi(id);
    uint8_t zi = fc_cube_zi(id);

    switch (face) {
        case FACE_PX: if (xi >= side - 1u) return FC_NEIGHBOR_NONE; xi++; break;
        case FACE_NX: if (xi == 0u)        return FC_NEIGHBOR_NONE; xi--; break;
        case FACE_PY: if (yi >= side - 1u) return FC_NEIGHBOR_NONE; yi++; break;
        case FACE_NY: if (yi == 0u)        return FC_NEIGHBOR_NONE; yi--; break;
        case FACE_PZ: if (zi >= side - 1u) return FC_NEIGHBOR_NONE; zi++; break;  /* World A store  */
        case FACE_NZ: if (zi == 0u)        return FC_NEIGHBOR_NONE; zi--; break;  /* World B drain  */
        default:      return FC_NEIGHBOR_NONE;
    }
    return (uint64_t)fc_cube_pack(xi, yi, zi);
}

/* file offset — direct seek, O(1), no index ────────────────── */

static inline uint64_t fc_cube_offset(uint32_t block_id)
{
    return (uint64_t)block_id * FRUSTUM_BLOCK_BYTES;
}

/* build block_id from signed coords ───────────────────────── */

static inline uint32_t fc_cube_id(int8_t x, int8_t y, int8_t z, uint8_t N)
{
    return fc_cube_pack((uint8_t)((int)x + N),
                        (uint8_t)((int)y + N),
                        (uint8_t)((int)z + N));
}

/* core block (0,0,0) — seed/SEED ──────────────────────────── */

static inline uint32_t fc_cube_seed(uint8_t N)
{
    return fc_cube_pack(N, N, N);   /* xi=yi=zi=N → x=y=z=0 */
}

/* total blocks in cube of shell N: (2N+1)³ ────────────────── */

static inline uint32_t fc_cube_total(uint8_t N)
{
    uint32_t side = 2u * (uint32_t)N + 1u;
    return side * side * side;
}

/* blocks in shell N wall only: (2N+1)³ - (2N-1)³ ─────────── */

static inline uint32_t fc_cube_shell_count(uint8_t N)
{
    if (N == 0u) return 1u;
    uint32_t outer = fc_cube_total(N);
    uint32_t inner = fc_cube_total((uint8_t)(N - 1u));
    return outer - inner;
}

/* ══════════════════════════════════════════════════════════════
 * CYLINDER — full implementation
 *
 * block_id = pack(r, sector, z_layer)
 *   r        : ring radius 0..N      (6 bits)
 *   sector   : angular sector 0..3   (2 bits, 4 sectors = ±X,±Y)
 *   z_layer  : depth 0..2N           (6 bits)
 *   packing  : bits [13:8]=r, [7:6]=sector, [5:0]=z
 *
 * fibo_clock maps to sector angle: tick % 4 = sector
 * top cap    (z=2N) = FACE_PZ = World A = STORE
 * bottom cap (z=0)  = FACE_NZ = World B = DRAIN
 * ══════════════════════════════════════════════════════════════ */

#define FC_CYL_SECTOR_BITS  2u
#define FC_CYL_SECTOR_MASK  0x3u   /* 0..3 */
#define FC_CYL_SECTOR_COUNT 4u

static inline uint32_t fc_cyl_pack(uint8_t r, uint8_t sector, uint8_t z)
{
    return ((uint32_t)(r      & FC_AXIS_MASK)      << (FC_AXIS_BITS + FC_CYL_SECTOR_BITS))
         | ((uint32_t)(sector & FC_CYL_SECTOR_MASK) <<  FC_AXIS_BITS)
         |  (uint32_t)(z      & FC_AXIS_MASK);
}

static inline uint8_t fc_cyl_r     (uint32_t id) { return (uint8_t)((id >> (FC_AXIS_BITS + FC_CYL_SECTOR_BITS)) & FC_AXIS_MASK); }
static inline uint8_t fc_cyl_sector(uint32_t id) { return (uint8_t)((id >>  FC_AXIS_BITS) & FC_CYL_SECTOR_MASK); }
static inline uint8_t fc_cyl_z     (uint32_t id) { return (uint8_t)( id & FC_AXIS_MASK); }

static inline uint64_t fc_cyl_neighbor(uint64_t block_id, FaceId face, uint8_t N)
{
    const uint32_t id      = (uint32_t)block_id;
    const uint8_t  z_top   = (uint8_t)(2u * N);
    const uint8_t  r       = fc_cyl_r(id);
    const uint8_t  sector  = fc_cyl_sector(id);
    const uint8_t  z       = fc_cyl_z(id);

    switch (face) {
        case FACE_PZ:  /* top cap = World A */
            if (z >= z_top) return FC_NEIGHBOR_NONE;
            return (uint64_t)fc_cyl_pack(r, sector, (uint8_t)(z + 1u));
        case FACE_NZ:  /* bottom cap = World B */
            if (z == 0u)   return FC_NEIGHBOR_NONE;
            return (uint64_t)fc_cyl_pack(r, sector, (uint8_t)(z - 1u));
        case FACE_PX:  /* outward radial */
            if (r >= N)    return FC_NEIGHBOR_NONE;
            return (uint64_t)fc_cyl_pack((uint8_t)(r + 1u), sector, z);
        case FACE_NX:  /* inward radial */
            if (r == 0u)   return FC_NEIGHBOR_NONE;
            return (uint64_t)fc_cyl_pack((uint8_t)(r - 1u), sector, z);
        case FACE_PY:  /* next sector (wrap) */
            return (uint64_t)fc_cyl_pack(r, (uint8_t)((sector + 1u) % FC_CYL_SECTOR_COUNT), z);
        case FACE_NY:  /* prev sector (wrap) */
            return (uint64_t)fc_cyl_pack(r, (uint8_t)((sector + FC_CYL_SECTOR_COUNT - 1u) % FC_CYL_SECTOR_COUNT), z);
        default: return FC_NEIGHBOR_NONE;
    }
}

static inline uint64_t fc_cyl_offset(uint32_t block_id)
{
    return (uint64_t)block_id * FRUSTUM_BLOCK_BYTES;
}

/* ══════════════════════════════════════════════════════════════
 * GOLDBERG — neighbor function
 *
 * block_id = tile_id (0..n_tiles-1) on GP(N,0) Goldberg sphere
 *   n_tiles = 10N² + 2  (12 pentagons + 10(N²-1) hexagons)
 *
 * Pentagon tiles (tile_id 0..11):
 *   Uses dodeca_adj() from geo_dodeca_adj.h (icosa dual).
 *   5 dodeca edges → 5 lateral face neighbors.
 *   NZ (outward) always returns NONE (only 5 edges).
 *   The returned neighbor is the adjacent pentagon's tile_id.
 *   At GP(N,0) with N>1, real neighbors between pentagons are
 *   hexagons; this returns the pentagon across each edge as the
 *   "sector anchor" (sufficient for routing/approximation).
 *
 * Hexagon tiles (tile_id ≥ 12):
 *   Returns FC_NEIGHBOR_NONE. Full hexagon adjacency requires a
 *   per-level LUT from icosahedral subdivision coordinates.
 *   gp_face_count(N) = 10N² + 2  →  at N=4: 162 tiles.
 *
 * FACE mapping:
 *   PX/NX/PY/NY/PZ → dodeca edges 0..4 (5 lateral directions)
 *   NZ             → no edge (pentagon only has 5)
 * ══════════════════════════════════════════════════════════════ */

/* FaceId → dodeca edge index. NZ=5 means "past 5 edges" → NONE for pentagons */
static const uint8_t FC_GOLD_FACE_TO_EDGE[6] = { 0,1,2,3,4,5 };

static inline uint64_t fc_goldberg_neighbor(uint64_t block_id, FaceId face, uint8_t N)
{
    (void)N;
    uint32_t tid = (uint32_t)(block_id & 0xFFFFFFFFu);

    if (tid >= 12u) {
        /* Hexagon: needs per-level LUT — not yet implemented */
        return FC_NEIGHBOR_NONE;
    }

    /* Pentagon: use dodeca_adj (edge 0..4 → neighbor pentagon face) */
    uint8_t edge = FC_GOLD_FACE_TO_EDGE[(uint8_t)face % 6u];
    if (edge >= 5u) return FC_NEIGHBOR_NONE;  /* NZ = outward, NONE */

    DodecaAdj nb = dodeca_adj(0, (uint8_t)tid, edge);
    return (uint64_t)nb.face;
}

/* ══════════════════════════════════════════════════════════════
 * TOPOLOGY DESCRIPTOR — runtime-selectable adapter
 *
 * Wire once at init, use everywhere.
 * Swap topology without touching FrustumBlock internals.
 * ══════════════════════════════════════════════════════════════ */

typedef enum {
    FC_TOPO_CUBE     = 0,
    FC_TOPO_CYLINDER = 1,
    FC_TOPO_GOLDBERG = 2,   /* pentagon wired via dodeca_adj; hexagon: WIP */
} FcTopology;

typedef struct {
    FcTopology   topo;
    uint8_t      N;           /* radius / half-side / shell depth        */
    fc_neighbor_fn neighbor;  /* topology-specific neighbor function     */
} FcAdapter;

static inline void fc_adapter_init(FcAdapter *a, FcTopology topo, uint8_t N)
{
    a->topo = topo;
    a->N    = N;
    switch (topo) {
        case FC_TOPO_CUBE:     a->neighbor = fc_cube_neighbor;          break;
        case FC_TOPO_CYLINDER: a->neighbor = fc_cyl_neighbor;           break;
        case FC_TOPO_GOLDBERG: a->neighbor = fc_goldberg_neighbor;      break;
        default:               a->neighbor = fc_goldberg_neighbor;      break;
    }
}

/* uniform neighbor call — topology-agnostic */
static inline uint64_t fc_neighbor(const FcAdapter *a, uint64_t block_id, FaceId face)
{
    return a->neighbor(block_id, face, a->N);
}

/* ══════════════════════════════════════════════════════════════
 * VERIFY — call once at init
 * ══════════════════════════════════════════════════════════════ */

static inline int fc_verify(void)
{
    /* face_opposite self-inverse */
    for (int f = 0; f < (int)FC_FACE_COUNT; f++) {
        if (face_opposite(face_opposite((FaceId)f)) != (FaceId)f) return -1;
    }
    /* World A/B: PZ↔NZ are opposites */
    if (face_opposite(FACE_PZ) != FACE_NZ) return -2;

    /* cube: seed at (0,0,0) for any N */
    const uint8_t N = 4u;
    uint32_t seed = fc_cube_seed(N);
    if (fc_cube_x(seed, N) != 0) return -3;
    if (fc_cube_y(seed, N) != 0) return -4;
    if (fc_cube_z(seed, N) != 0) return -5;
    if (fc_cube_shell(seed, N)  != 0u) return -6;

    /* cube: shell 8 = drain boundary */
    uint32_t s8 = fc_cube_id(8, 0, 0, 31);   /* N=31, x=8 → shell=8 */
    if (fc_cube_shell(s8, 31u) != 8u) return -7;
    if (!fc_cube_is_drain_boundary(8u))  return -8;

    /* cube: sacred shells at N mod 3 == 1 */
    if (!fc_cube_is_sacred_shell(1u))  return -9;
    if (!fc_cube_is_sacred_shell(4u))  return -10;
    if ( fc_cube_is_sacred_shell(2u))  return -11;  /* non-sacred */
    if ( fc_cube_is_sacred_shell(8u))  return -12;  /* 8 mod 3 = 2 */

    /* cube: PZ neighbor of seed exists, NZ of bottom does not */
    FcAdapter a; fc_adapter_init(&a, FC_TOPO_CUBE, N);
    if (fc_neighbor(&a, (uint64_t)seed, FACE_PZ) == FC_NEIGHBOR_NONE) return -13;
    uint32_t bottom = fc_cube_id(0, 0, -(int8_t)N, N);
    if (fc_neighbor(&a, (uint64_t)bottom, FACE_NZ) != FC_NEIGHBOR_NONE) return -14;

    /* cylinder: FACE_PY/NY wrap (no boundary) */
    FcAdapter ca; fc_adapter_init(&ca, FC_TOPO_CYLINDER, N);
    uint32_t cyl0 = fc_cyl_pack(2u, 3u, 4u);
    uint64_t wrap = fc_neighbor(&ca, (uint32_t)cyl0, FACE_PY);
    if (fc_cyl_sector((uint32_t)wrap) != 0u) return -15;  /* 3+1 mod 4 = 0 */

    /* goldberg: pentagon 0 → FACE_NZ = NONE, FACE_PX = neighbor 1 */
    FcAdapter gs; fc_adapter_init(&gs, FC_TOPO_GOLDBERG, 4u);
    if (fc_neighbor(&gs, 0, FACE_NZ) != FC_NEIGHBOR_NONE) return -16;
    if (fc_neighbor(&gs, 0, FACE_PX) != 1)                return -17;
    /* goldberg: hexagon tile_id 12 → NONE (not yet implemented) */
    if (fc_neighbor(&gs, 12, FACE_PX) != FC_NEIGHBOR_NONE) return -18;

    return 0;
}

#endif /* FRUSTUM_COORD_H */

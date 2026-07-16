/*
 * geo_jump_config.h — GeoJump Multi-Configuration + Gear Converter
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Three configurations sharing GEO_FULL = 20736:
 *
 *   Config A: 4×4×3 [3 towers]  → 48B unit → 144×144 layout
 *   Config B: 8×8   [2 towers]  → 64B unit → 128×162 layout
 *   Config C: 4×4×4 [2 towers]  → 64B unit → 128×162 layout
 *   Config D: 9×9   [2 towers]  → 64B unit → 162×128 layout (inverse of A)
 *   Config E: 16×16 [1 tower]   → 64B unit → 256×81  layout
 *
 *   128×162 = 144×144 = 162×128 = 256×81 = 20736
 *   Scaling ratio: 9/8 = 1.125 (128 × 9/8 = 144 × 9/8 = 162)
 *   256 = 128 × 2 (binary double), 81 = 162 / 2
 *
 * Gear converter: translates node_id between configurations.
 * The address space is the same (20736 nodes), but the grouping differs.
 *
 * Compile: gcc -O2 -std=c11 -o geo_jump_config.exe geo_jump_config.c -lm
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef GEO_JUMP_CONFIG_H
#define GEO_JUMP_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════ */

#define GEO_FULL            20736u      /* 144² = 128×162 = universal */

/* Config A: 4×4×3, 3 towers */
#define GJ_A_FLOOR_COLS     4u
#define GJ_A_FLOOR_ROWS     4u
#define GJ_A_FLOOR_CELLS    16u         /* 4×4 */
#define GJ_A_FLOORS         3u          /* per tower */
#define GJ_A_CELLS_PER_TOWER (GJ_A_FLOOR_CELLS * GJ_A_FLOORS) /* 48 */
#define GJ_A_TOWERS_PER_GROUP 3u
#define GJ_A_GROUP_SIZE     (GJ_A_CELLS_PER_TOWER * GJ_A_TOWERS_PER_GROUP) /* 144 */
#define GJ_A_GROUPS         (GEO_FULL / GJ_A_GROUP_SIZE)   /* 144 */
#define GJ_A_UNIT_SZ        48u         /* bytes per unit */

/* Config B: 8×8, 2 towers */
#define GJ_B_FLOOR_COLS     8u
#define GJ_B_FLOOR_ROWS     8u
#define GJ_B_FLOOR_CELLS    64u         /* 8×8 */
#define GJ_B_FLOORS         1u          /* per tower */
#define GJ_B_CELLS_PER_TOWER (GJ_B_FLOOR_CELLS * GJ_B_FLOORS) /* 64 */
#define GJ_B_TOWERS_PER_GROUP 2u
#define GJ_B_GROUP_SIZE     (GJ_B_CELLS_PER_TOWER * GJ_B_TOWERS_PER_GROUP) /* 128 */
#define GJ_B_GROUPS         (GEO_FULL / GJ_B_GROUP_SIZE)   /* 162 */
#define GJ_B_UNIT_SZ        64u         /* bytes per unit */

/* Config C: 4×4×4, 2 towers */
#define GJ_C_FLOOR_COLS     4u
#define GJ_C_FLOOR_ROWS     4u
#define GJ_C_FLOOR_CELLS    16u         /* 4×4 */
#define GJ_C_FLOORS         4u          /* per tower */
#define GJ_C_CELLS_PER_TOWER (GJ_C_FLOOR_CELLS * GJ_C_FLOORS) /* 64 */
#define GJ_C_TOWERS_PER_GROUP 2u
#define GJ_C_GROUP_SIZE     (GJ_C_CELLS_PER_TOWER * GJ_C_TOWERS_PER_GROUP) /* 128 */
#define GJ_C_GROUPS         (GEO_FULL / GJ_C_GROUP_SIZE)   /* 162 */
#define GJ_C_UNIT_SZ        64u         /* bytes per unit */

/* Config D: 162 cells/group (inverse of A), 128 groups, 64B unit */
/* 162 = 144 × 9/8. Internal: 2 towers × 81 cells = 162.
 * 81 = 9×9 flat grid (non-power-of-2 Hilbert uses Peano or custom). */
#define GJ_D_FLOOR_COLS     9u
#define GJ_D_FLOOR_ROWS     9u
#define GJ_D_FLOOR_CELLS    81u         /* 9×9 */
#define GJ_D_FLOORS         1u          /* per tower */
#define GJ_D_CELLS_PER_TOWER (GJ_D_FLOOR_CELLS * GJ_D_FLOORS) /* 81 */
#define GJ_D_TOWERS_PER_GROUP 2u
#define GJ_D_GROUP_SIZE     (GJ_D_CELLS_PER_TOWER * GJ_D_TOWERS_PER_GROUP) /* 162 */
#define GJ_D_GROUPS         (GEO_FULL / GJ_D_GROUP_SIZE)   /* 128 */
#define GJ_D_UNIT_SZ        64u         /* bytes per unit */

/* Config E: 16×16, 1 tower, 64B unit, 256×81 layout */
#define GJ_E_FLOOR_COLS     16u
#define GJ_E_FLOOR_ROWS     16u
#define GJ_E_FLOOR_CELLS    256u        /* 16×16 */
#define GJ_E_FLOORS         1u          /* per tower */
#define GJ_E_CELLS_PER_TOWER (GJ_E_FLOOR_CELLS * GJ_E_FLOORS) /* 256 */
#define GJ_E_TOWERS_PER_GROUP 1u
#define GJ_E_GROUP_SIZE     (GJ_E_CELLS_PER_TOWER * GJ_E_TOWERS_PER_GROUP) /* 256 */
#define GJ_E_GROUPS         (GEO_FULL / GJ_E_GROUP_SIZE)   /* 81 */
#define GJ_E_UNIT_SZ        64u         /* bytes per unit */

/* ═══════════════════════════════════════════════════════════════════════
   CONFIG TYPE
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    GJ_CONFIG_A = 0,    /* 4×4×3, 3 towers, 48B unit, 144×144 */
    GJ_CONFIG_B = 1,    /* 8×8, 2 towers, 64B unit, 128×162 */
    GJ_CONFIG_C = 2,    /* 4×4×4, 2 towers, 64B unit, 128×162 */
    GJ_CONFIG_D = 3,    /* 9×9, 2 towers, 64B unit, 162×128 (inverse of A) */
    GJ_CONFIG_E = 4     /* 16×16, 1 tower, 64B unit, 256×81 */
} GjConfig;

typedef struct {
    GjConfig config;
    uint32_t floor_cols;
    uint32_t floor_rows;
    uint32_t floor_cells;   /* cols × rows */
    uint32_t floors;        /* per tower */
    uint32_t cells_per_tower;
    uint32_t towers_per_group;
    uint32_t group_size;
    uint32_t groups;
    uint32_t unit_sz;       /* bytes per cell */
} GjConfigInfo;

static inline GjConfigInfo gj_config_info(GjConfig cfg)
{
    GjConfigInfo info;
    info.config = cfg;
    switch (cfg) {
    case GJ_CONFIG_A:
        info.floor_cols      = GJ_A_FLOOR_COLS;
        info.floor_rows      = GJ_A_FLOOR_ROWS;
        info.floor_cells     = GJ_A_FLOOR_CELLS;
        info.floors          = GJ_A_FLOORS;
        info.cells_per_tower = GJ_A_CELLS_PER_TOWER;
        info.towers_per_group= GJ_A_TOWERS_PER_GROUP;
        info.group_size      = GJ_A_GROUP_SIZE;
        info.groups          = GJ_A_GROUPS;
        info.unit_sz         = GJ_A_UNIT_SZ;
        break;
    case GJ_CONFIG_B:
        info.floor_cols      = GJ_B_FLOOR_COLS;
        info.floor_rows      = GJ_B_FLOOR_ROWS;
        info.floor_cells     = GJ_B_FLOOR_CELLS;
        info.floors          = GJ_B_FLOORS;
        info.cells_per_tower = GJ_B_CELLS_PER_TOWER;
        info.towers_per_group= GJ_B_TOWERS_PER_GROUP;
        info.group_size      = GJ_B_GROUP_SIZE;
        info.groups          = GJ_B_GROUPS;
        info.unit_sz         = GJ_B_UNIT_SZ;
        break;
    case GJ_CONFIG_C:
        info.floor_cols      = GJ_C_FLOOR_COLS;
        info.floor_rows      = GJ_C_FLOOR_ROWS;
        info.floor_cells     = GJ_C_FLOOR_CELLS;
        info.floors          = GJ_C_FLOORS;
        info.cells_per_tower = GJ_C_CELLS_PER_TOWER;
        info.towers_per_group= GJ_C_TOWERS_PER_GROUP;
        info.group_size      = GJ_C_GROUP_SIZE;
        info.groups          = GJ_C_GROUPS;
        info.unit_sz         = GJ_C_UNIT_SZ;
        break;
    case GJ_CONFIG_D:
        info.floor_cols      = GJ_D_FLOOR_COLS;
        info.floor_rows      = GJ_D_FLOOR_ROWS;
        info.floor_cells     = GJ_D_FLOOR_CELLS;
        info.floors          = GJ_D_FLOORS;
        info.cells_per_tower = GJ_D_CELLS_PER_TOWER;
        info.towers_per_group= GJ_D_TOWERS_PER_GROUP;
        info.group_size      = GJ_D_GROUP_SIZE;
        info.groups          = GJ_D_GROUPS;
        info.unit_sz         = GJ_D_UNIT_SZ;
        break;
    case GJ_CONFIG_E:
        info.floor_cols      = GJ_E_FLOOR_COLS;
        info.floor_rows      = GJ_E_FLOOR_ROWS;
        info.floor_cells     = GJ_E_FLOOR_CELLS;
        info.floors          = GJ_E_FLOORS;
        info.cells_per_tower = GJ_E_CELLS_PER_TOWER;
        info.towers_per_group= GJ_E_TOWERS_PER_GROUP;
        info.group_size      = GJ_E_GROUP_SIZE;
        info.groups          = GJ_E_GROUPS;
        info.unit_sz         = GJ_E_UNIT_SZ;
        break;
    default:
        memset(&info, 0, sizeof(info));
        break;
    }
    return info;
}

/* ═══════════════════════════════════════════════════════════════════════
   NODE ADDRESS DECOMPOSE / COMPOSE
   ═══════════════════════════════════════════════════════════════════════

   For any config, a node_id (0..20735) decomposes to:

   Config A (4×4×3, 3 towers):
     group (0..143) × tower_in_group (0..2) × tower (0..47) × floor (0..2) × cell (0..15)
     = node_id / 144  ×  node_id / 48 % 3  ×  node_id / 48  ×  node_id / 16 % 3  ×  node_id % 16

   Config B (8×8, 2 towers):
     group (0..161) × tower_in_group (0..1) × tower (0..63) × cell (0..63)
     = node_id / 128  ×  node_id / 64 % 2  ×  node_id / 64  ×  node_id % 64

   Config C (4×4×4, 2 towers):
     group (0..161) × tower_in_group (0..1) × tower (0..63) × floor (0..3) × cell (0..15)
     = node_id / 128  ×  node_id / 64 % 2  ×  node_id / 64  ×  node_id / 16 % 4  ×  node_id % 16
 */

typedef struct {
    uint32_t group;
    uint32_t tower_in_group;
    uint32_t tower;
    uint32_t floor;
    uint32_t cell;
} GjNodeAddr;

static inline GjNodeAddr gj_decompose(uint32_t node_id, GjConfig cfg)
{
    GjNodeAddr a;
    GjConfigInfo info = gj_config_info(cfg);
    (void)info;

    switch (cfg) {
    case GJ_CONFIG_A:
        a.group           = node_id / GJ_A_GROUP_SIZE;                    /* 0..143 */
        a.tower_in_group  = (node_id / GJ_A_CELLS_PER_TOWER) % GJ_A_TOWERS_PER_GROUP; /* 0..2 */
        a.tower           = node_id / GJ_A_CELLS_PER_TOWER;              /* 0..143 */
        a.floor           = (node_id / GJ_A_FLOOR_CELLS) % GJ_A_FLOORS;  /* 0..2 */
        a.cell            = node_id % GJ_A_FLOOR_CELLS;                  /* 0..15 */
        break;
    case GJ_CONFIG_B:
        a.group           = node_id / GJ_B_GROUP_SIZE;                    /* 0..161 */
        a.tower_in_group  = (node_id / GJ_B_CELLS_PER_TOWER) % GJ_B_TOWERS_PER_GROUP; /* 0..1 */
        a.tower           = node_id / GJ_B_CELLS_PER_TOWER;              /* 0..323 */
        a.floor           = 0;                                            /* single floor */
        a.cell            = node_id % GJ_B_FLOOR_CELLS;                  /* 0..63 */
        break;
    case GJ_CONFIG_C:
        a.group           = node_id / GJ_C_GROUP_SIZE;
        a.tower_in_group  = (node_id / GJ_C_CELLS_PER_TOWER) % GJ_C_TOWERS_PER_GROUP;
        a.tower           = node_id / GJ_C_CELLS_PER_TOWER;
        a.floor           = (node_id / GJ_C_FLOOR_CELLS) % GJ_C_FLOORS;
        a.cell            = node_id % GJ_C_FLOOR_CELLS;
        break;
    case GJ_CONFIG_D:
        a.group           = node_id / GJ_D_GROUP_SIZE;
        a.tower_in_group  = (node_id / GJ_D_CELLS_PER_TOWER) % GJ_D_TOWERS_PER_GROUP;
        a.tower           = node_id / GJ_D_CELLS_PER_TOWER;
        a.floor           = (node_id / GJ_D_FLOOR_CELLS) % GJ_D_FLOORS;
        a.cell            = node_id % GJ_D_FLOOR_CELLS;
        break;
    case GJ_CONFIG_E:
        a.group           = node_id / GJ_E_GROUP_SIZE;
        a.tower_in_group  = (node_id / GJ_E_CELLS_PER_TOWER) % GJ_E_TOWERS_PER_GROUP;
        a.tower           = node_id / GJ_E_CELLS_PER_TOWER;
        a.floor           = (node_id / GJ_E_FLOOR_CELLS) % GJ_E_FLOORS;
        a.cell            = node_id % GJ_E_FLOOR_CELLS;
        break;
    default:
        memset(&a, 0, sizeof(a));
        break;
    }
    return a;
}

static inline uint32_t gj_compose(const GjNodeAddr *a, GjConfig cfg)
{
    switch (cfg) {
    case GJ_CONFIG_A:
        return a->group * GJ_A_GROUP_SIZE
             + a->tower_in_group * GJ_A_CELLS_PER_TOWER
             + a->floor * GJ_A_FLOOR_CELLS
             + a->cell;
    case GJ_CONFIG_B:
        return a->group * GJ_B_GROUP_SIZE
             + a->tower_in_group * GJ_B_CELLS_PER_TOWER
             + a->cell;
    case GJ_CONFIG_C:
        return a->group * GJ_C_GROUP_SIZE
             + a->tower_in_group * GJ_C_CELLS_PER_TOWER
             + a->floor * GJ_C_FLOOR_CELLS
             + a->cell;
    case GJ_CONFIG_D:
        return a->group * GJ_D_GROUP_SIZE
             + a->tower_in_group * GJ_D_CELLS_PER_TOWER
             + a->floor * GJ_D_FLOOR_CELLS
             + a->cell;
    case GJ_CONFIG_E:
        return a->group * GJ_E_GROUP_SIZE
             + a->tower_in_group * GJ_E_CELLS_PER_TOWER
             + a->floor * GJ_E_FLOOR_CELLS
             + a->cell;
    default:
        return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   HILBERT ORDERING WITHIN FLOOR
   ═══════════════════════════════════════════════════════════════════════ */

/* Standard Hilbert curve: 2D (x,y) → distance d */
static inline uint32_t gj_hilbert_xy2d(uint32_t x, uint32_t y, uint32_t order)
{
    uint32_t d = 0;
    for (uint32_t s = 1u << (order - 1); s; s >>= 1) {
        uint32_t rx = (x & s) ? 1 : 0;
        uint32_t ry = (y & s) ? 1 : 0;
        d = (d << 2) | ((3u * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = (s - 1) - x; y = (s - 1) - y; }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

/* Inverse Hilbert: distance d → (x,y) — from pogls_hilbert_container.h */
static inline void gj_hilbert_d2xy(uint32_t d, uint32_t order,
                                    uint32_t *x, uint32_t *y)
{
    uint32_t hx = 0, hy = 0;
    uint32_t s = 1;
    for (uint32_t i = 0; i < order; i++) {
        uint32_t rot = d & 3;
        uint32_t rx = (rot >> 1) & 1;
        uint32_t ry = (rot & 1) ^ rx;
        if (ry == 0) {
            if (rx == 1) {
                hx = s - 1 - hx;
                hy = s - 1 - hy;
            }
            uint32_t t = hx; hx = hy; hy = t;
        }
        hx += rx * s;
        hy += ry * s;
        d >>= 2;
        s <<= 1;
    }
    *x = hx;
    *y = hy;
}

/* Peano curve: 2D (x,y) → distance d (for non-power-of-2 grids like 9×9) */
static inline uint32_t gj_peano_xy2d(uint32_t x, uint32_t y, uint32_t cols, uint32_t rows)
{
    if (x & 1u)
        return x * rows + (rows - 1u - y);
    return x * rows + y;
}

/* ═══════════════════════════════════════════════════════════════════════
   GEAR CONVERTER
   ═══════════════════════════════════════════════════════════════════════

   Converts data between configurations. The address space is the same
   (20736 nodes), but chunk size and grouping differ.

   Config A: 48B chunks → 20736 × 48 = 995,328 bytes raw
   Config B: 64B chunks → 20736 × 64 = 1,327,104 bytes raw
   Config C: 64B chunks → 20736 × 64 = 1,327,104 bytes raw

   Conversion: for each node_id in target config, find the corresponding
   node_id in source config and copy unit_sz bytes.

   For 48B↔64B: the data is the same conceptually (each node holds one
   "cell" of data), but the cell size differs. The converter treats each
   node as a fixed-size cell and truncates/pads as needed.

   For 64B↔64B (B↔C): same cell size, different grouping. The converter
   re-groups cells from one tower/floor layout to another.
 */

/* Convert data from one config to another.
 *
 * src_data / src_cfg: source data + config
 * dst_data / dst_cfg: destination data + config
 * max_nodes: maximum nodes to convert (0 = all GEO_FULL)
 *
 * For each node_id 0..max_nodes-1:
 *   1. Decompose node_id in dst_cfg → (group, tower_in_group, tower, floor, cell)
 *   2. Find corresponding position in src_cfg
 *   3. Copy min(src_unit_sz, dst_unit_sz) bytes
 *
 * When src_unit_sz != dst_unit_sz (48B↔64B):
 *   - 48B→64B: pads to 64B (zero-fill tail)
 *   - 64B→48B: truncates to 48B
 *
 * Returns number of nodes converted.
 */
static inline uint32_t gj_gear_convert(const void *src_data, GjConfig src_cfg,
                                        void *dst_data, GjConfig dst_cfg,
                                        uint32_t max_nodes)
{
    if (!src_data || !dst_data) return 0;
    if (max_nodes == 0 || max_nodes > GEO_FULL) max_nodes = GEO_FULL;

    GjConfigInfo si = gj_config_info(src_cfg);
    GjConfigInfo di = gj_config_info(dst_cfg);
    const uint8_t *src = (const uint8_t *)src_data;
    uint8_t       *dst = (uint8_t *)dst_data;

    uint32_t converted = 0;
    for (uint32_t nid = 0; nid < max_nodes; nid++) {
        /* Map via global sequential position:
         * dst node → sequential position → same position in src → src node_id
         *
         * For each config, sequential position = group * group_size + pos_in_group.
         * The converter maps dst[i] ↔ src[i] by sequential index. */
        GjNodeAddr da = gj_decompose(nid, dst_cfg);

        /* Global sequential position of dst node */
        uint32_t dst_seq = da.group * di.group_size
                         + da.tower_in_group * di.cells_per_tower
                         + da.floor * di.floor_cells
                         + da.cell;

        /* Map sequential position to src node_id */
        uint32_t src_group   = dst_seq / si.group_size;
        uint32_t src_pos     = dst_seq % si.group_size;
        GjNodeAddr sa;
        memset(&sa, 0, sizeof(sa));
        sa.group          = src_group;
        sa.tower_in_group = src_pos / si.cells_per_tower;
        sa.floor          = (src_pos / si.floor_cells) % si.floors;
        sa.cell           = src_pos % si.floor_cells;
        uint32_t src_nid   = gj_compose(&sa, src_cfg);

        if (src_nid < GEO_FULL && nid < GEO_FULL) {
            uint32_t copy_sz = (si.unit_sz < di.unit_sz) ? si.unit_sz : di.unit_sz;
            memcpy(dst + (size_t)nid * di.unit_sz,
                   src + (size_t)src_nid * si.unit_sz,
                   copy_sz);
            if (di.unit_sz > si.unit_sz) {
                memset(dst + (size_t)nid * di.unit_sz + copy_sz, 0,
                       di.unit_sz - copy_sz);
            }
            converted++;
        }
    }
    return converted;
}

/* ═══════════════════════════════════════════════════════════════════════
   AUTO-SELECT: pick best config for given chunk size
   ═══════════════════════════════════════════════════════════════════════ */

static inline GjConfig gj_config_for_chunk_size(uint32_t chunk_bytes)
{
    if (chunk_bytes <= 48) return GJ_CONFIG_A;   /* 4×4×3, 48B */
    return GJ_CONFIG_B;                           /* 8×8, 64B (default for >=49B) */
}

static inline GjConfig gj_config_auto(uint32_t chunk_bytes, int prefer_3d)
{
    if (chunk_bytes <= 48) return GJ_CONFIG_A;
    if (prefer_3d) return GJ_CONFIG_C;           /* 4×4×4, 64B, 3D */
    return GJ_CONFIG_B;                           /* 8×8, 64B, 2D */
}

#ifdef __cplusplus
}
#endif

#endif /* GEO_JUMP_CONFIG_H */

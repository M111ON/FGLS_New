/* contour_codec_virtual.h — Virtual (Lazy) Contour Codec
 * ═══════════════════════════════════════════════════════════════════════════
 * 
 * "Everything is just a map — parallel along the mental model"
 * 
 * No geo array allocated. No storage of zeros.
 * Address = map(cell) computed O(1) on demand.
 * Only non-zero cells stored in sparse hash map.
 * 
 * Mental Model:
 *   - 6 faces = 6 directions of a cube (±X, ±Y, ±Z)
 *   - 12 faces = 12 faces of dodecahedron
 *   - Face IS the weight class / direction
 *   - (x,y,z) = position within that face
 *   - value = displacement from origin (XOR with 0 recovers weight)
 *   - Geo address = where this displacement projects in 144×144 space
 * 
 * Usage:
 *   #define CONTOUR_CODEC_VIRTUAL_IMPLEMENTATION
 *   #include "contour_codec_virtual.h"
 * 
 *   ccs_config cfg = { .faces=6, .W=10, .H=10, .L=10, .strategy=CCS_STRIDE37 };
 *   ccs_virtual_ctx *ctx = ccs_virtual_create(&cfg);
 *   ccs_virtual_set(ctx, face, x, y, z, value);  // O(1), allocates only this cell
 *   int8_t v = ccs_virtual_get(ctx, face, x, y, z); // O(1)
 *   ccs_virtual_encode_all(ctx, cells, n);       // batch, only non-zero stored
 *   ccs_virtual_free(ctx);
 */

#ifndef CONTOUR_CODEC_VIRTUAL_H
#define CONTOUR_CODEC_VIRTUAL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
   Configuration (same as scaled codec)
   ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    CCSV_SEQUENTIAL  = 0,  // face*(W*H*L) + z*(H*W) + y*W + x
    CCSV_STRIDE37    = 1,  // global_idx * stride % geo_full
    CCSV_FACE_REGION = 2,  // face*region_size + z*region_z + y*region_y + x
    CCSV_GRID        = 3,  // row = face*z_planes + z, col = y*W + x
    CCSV_COUNT       = 4
} ccsv_strategy;

typedef struct {
    int faces;              // 6 (cube directions) or 12 (dodecahedron faces)
    int W, H, L;            // Dimensions per face
    ccsv_strategy strategy; // Mapping strategy
    int geo_full;           // Geo space size (0 = auto)
    int geo_dim;            // sqrt(geo_full) (0 = auto)
    int stride;             // For stride37 (0 = auto coprime)
    int max_sparse;         // Max non-zero cells (0 = unlimited)
} ccsv_config;

typedef struct {
    int face, x, y, z;
    int8_t value;
    int global_idx;
} ccsv_cell;

typedef struct ccsv_virtual_ctx ccsv_virtual_ctx;

/* ═══════════════════════════════════════════════════════════════════════════
   Public API
   ═══════════════════════════════════════════════════════════════════════════ */

/* Create virtual context. Returns NULL on invalid config. */
ccsv_virtual_ctx *ccsv_create(const ccsv_config *cfg);

/* Free context and sparse storage */
void ccsv_free(ccsv_virtual_ctx *ctx);

/* O(1) single cell operations — the core virtual interface */
int8_t ccsv_get(const ccsv_virtual_ctx *ctx, int face, int x, int y, int z);
void   ccsv_set(ccsv_virtual_ctx *ctx, int face, int x, int y, int z, int8_t val);

/* Batch operations (iterate only non-zero or all cells) */
int ccsv_encode_all(ccsv_virtual_ctx *ctx, const ccsv_cell *cells, int n);
int ccsv_decode_all(ccsv_virtual_ctx *ctx, ccsv_cell *out, int max_out);

/* Sparse iteration — only visits cells that were actually set */
typedef void (*ccsv_iter_fn)(int face, int x, int y, int z, int8_t value, void *user);
void ccsv_iterate(const ccsv_virtual_ctx *ctx, ccsv_iter_fn fn, void *user);

/* Get count of non-zero cells stored */
int ccsv_count(const ccsv_virtual_ctx *ctx);

/* Verify roundtrip for given cells */
int ccsv_verify(ccsv_virtual_ctx *ctx, const ccsv_cell *cells, int n);

/* Strategy name */
const char *ccsv_strategy_name(ccsv_strategy s);

/* Default config helper */
static inline ccsv_config ccsv_default_config(void) {
    ccsv_config cfg = {
        .faces = 6,
        .W = 10, .H = 10, .L = 10,
        .strategy = CCSV_STRIDE37,
        .geo_full = 0,
        .geo_dim = 0,
        .stride = 0,
        .max_sparse = 0,
    };
    return cfg;
}

/* Total theoretical cells (not stored) */
static inline int ccsv_total_cells(const ccsv_config *cfg) {
    return cfg->faces * cfg->W * cfg->H * cfg->L;
}

/* Get computed config from context */
const ccsv_config *ccsv_get_config(const ccsv_virtual_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CONTOUR_CODEC_VIRTUAL_H */


/* ═══════════════════════════════════════════════════════════════════════════
   IMPLEMENTATION — Virtual Sparse Map
   ═══════════════════════════════════════════════════════════════════════════ */

#ifdef CONTOUR_CODEC_VIRTUAL_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════════════════
   Internal: Sparse Hash Map (open addressing, power-of-2 size)
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t key;      // geo address (0 = empty, use key+1 for storage)
    int8_t value;      // cell value
    int face, x, y, z; // original coordinates for iteration
    int global_idx;
    uint8_t occupied;  // 1 = occupied, 0 = empty, 255 = deleted
} ccsv_entry;

struct ccsv_virtual_ctx {
    ccsv_config config;
    ccsv_entry *entries;     // hash table
    int table_size;          // power of 2
    int count;               // number of occupied entries
    int max_load;            // resize threshold
    int initialized;
};

/* Hash function for geo address */
static inline int ccsv_hash(uint32_t key, int table_size) {
    // Multiplicative hash (Knuth)
    return (int)((key * 2654435769u) >> (32 - (int)log2(table_size)));
}

/* Find slot for key (for get/set) */
static int ccsv_find_slot(const ccsv_virtual_ctx *ctx, uint32_t key) {
    int start = ccsv_hash(key, ctx->table_size);
    int idx = start;
    do {
        if (ctx->entries[idx].occupied == 0) return -idx - 1;  // empty slot
        if (ctx->entries[idx].occupied == 1 && ctx->entries[idx].key == key) return idx;  // found
        idx = (idx + 1) & (ctx->table_size - 1);
    } while (idx != start);
    return -1;  // table full
}

/* Resize hash table */
static int ccsv_resize(ccsv_virtual_ctx *ctx) {
    int new_size = ctx->table_size * 2;
    ccsv_entry *new_entries = (ccsv_entry*)calloc(new_size, sizeof(ccsv_entry));
    if (!new_entries) return -1;
    
    // Rehash all entries
    for (int i = 0; i < ctx->table_size; i++) {
        if (ctx->entries[i].occupied == 1) {
            uint32_t key = ctx->entries[i].key;
            int idx = ccsv_hash(key, new_size);
            while (new_entries[idx].occupied == 1) {
                idx = (idx + 1) & (new_size - 1);
            }
            new_entries[idx] = ctx->entries[i];
        }
    }
    
    free(ctx->entries);
    ctx->entries = new_entries;
    ctx->table_size = new_size;
    ctx->max_load = new_size * 3 / 4;  // 75% load factor
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Helpers (same as scaled codec)
   ═══════════════════════════════════════════════════════════════════════════ */

static int gcd_int(int a, int b) {
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

static int next_coprime(int n, int start) {
    for (int i = start; i < n; i++) {
        if (gcd_int(i, n) == 1) return i;
    }
    return 37;
}

static int next_square_ge(int n) {
    int r = (int)ceil(sqrt((double)n));
    return r * r;
}

static void ccsv_compute_derived(ccsv_config *cfg) {
    int cells = cfg->faces * cfg->W * cfg->H * cfg->L;
    
    if (cfg->geo_full <= 0) {
        cfg->geo_full = next_square_ge(cells);
        if (cfg->geo_full > 20736) cfg->geo_full = 20736;
    }
    if (cfg->geo_dim <= 0) {
        cfg->geo_dim = (int)round(sqrt((double)cfg->geo_full));
    }
    if (cfg->stride <= 0) {
        cfg->stride = next_coprime(cfg->geo_full, 37);
    }
}

static inline int ccsv_global_idx(const ccsv_config *cfg, int face, int x, int y, int z) {
    return face * cfg->W * cfg->H * cfg->L + z * cfg->H * cfg->W + y * cfg->W + x;
}

static inline void ccsv_from_idx(const ccsv_config *cfg, int idx, int *face, int *x, int *y, int *z) {
    int face_cells = cfg->W * cfg->H * cfg->L;
    *face = idx / face_cells;
    int rem = idx % face_cells;
    *z = rem / (cfg->H * cfg->W);
    rem %= (cfg->H * cfg->W);
    *y = rem / cfg->W;
    *x = rem % cfg->W;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Mapping Functions
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t map_sequential(const ccsv_config *cfg, const ccsv_cell *c) {
    return (uint32_t)(c->face * cfg->W * cfg->H * cfg->L + c->z * cfg->H * cfg->W + c->y * cfg->W + c->x);
}

static inline uint32_t map_stride37(const ccsv_config *cfg, const ccsv_cell *c) {
    int global = ccsv_global_idx(cfg, c->face, c->x, c->y, c->z);
    return (uint32_t)((uint32_t)global * (uint32_t)cfg->stride) % (uint32_t)cfg->geo_full;
}

static inline uint32_t map_face_region(const ccsv_config *cfg, const ccsv_cell *c) {
    int region_size = cfg->geo_full / cfg->faces;
    int region_z = cfg->H;
    int region_y = cfg->W;
    return (uint32_t)(c->face * region_size + c->z * region_z * region_y + c->y * region_y + c->x);
}

static inline uint32_t map_grid(const ccsv_config *cfg, const ccsv_cell *c) {
    int z_planes = (cfg->geo_dim + cfg->faces - 1) / cfg->faces;
    uint32_t row = (uint32_t)(c->face * z_planes + c->z);
    uint32_t col = (uint32_t)(c->y * cfg->W + c->x);
    return row * (uint32_t)cfg->geo_dim + col;
}

typedef uint32_t (*ccsv_mapfn)(const ccsv_config *, const ccsv_cell *);

static ccsv_mapfn map_func_for(ccsv_strategy s) {
    switch (s) {
        case CCSV_SEQUENTIAL:  return map_sequential;
        case CCSV_STRIDE37:    return map_stride37;
        case CCSV_FACE_REGION: return map_face_region;
        case CCSV_GRID:        return map_grid;
        default:               return map_sequential;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Public API Implementation
   ═══════════════════════════════════════════════════════════════════════════ */

ccsv_virtual_ctx *ccsv_create(const ccsv_config *cfg) {
    if (!cfg) return NULL;
    
    ccsv_config computed = *cfg;
    ccsv_compute_derived(&computed);
    
    int cells = computed.faces * computed.W * computed.H * computed.L;
    if (cells > computed.geo_full) return NULL;
    
    ccsv_virtual_ctx *ctx = (ccsv_virtual_ctx*)calloc(1, sizeof(ccsv_virtual_ctx));
    if (!ctx) return NULL;
    
    ctx->config = computed;
    
    // Hash table: power of 2, at least 2x expected non-zero cells
    int expected = (cfg->max_sparse > 0) ? cfg->max_sparse : (cells / 4);  // assume 25% sparse
    ctx->table_size = 1;
    while (ctx->table_size < expected * 2) ctx->table_size <<= 1;
    if (ctx->table_size < 16) ctx->table_size = 16;
    if (ctx->table_size > 65536) ctx->table_size = 65536;
    
    ctx->entries = (ccsv_entry*)calloc(ctx->table_size, sizeof(ccsv_entry));
    if (!ctx->entries) {
        free(ctx);
        return NULL;
    }
    
    ctx->max_load = ctx->table_size * 3 / 4;
    ctx->initialized = 1;
    return ctx;
}

void ccsv_free(ccsv_virtual_ctx *ctx) {
    if (!ctx) return;
    free(ctx->entries);
    free(ctx);
}

int8_t ccsv_get(const ccsv_virtual_ctx *ctx, int face, int x, int y, int z) {
    if (!ctx || !ctx->initialized) return 0;
    
    // Bounds check
    if (face < 0 || face >= ctx->config.faces) return 0;
    if (x < 0 || x >= ctx->config.W) return 0;
    if (y < 0 || y >= ctx->config.H) return 0;
    if (z < 0 || z >= ctx->config.L) return 0;
    
    ccsv_cell tmp = { .face = face, .x = x, .y = y, .z = z };
    tmp.global_idx = ccsv_global_idx(&ctx->config, face, x, y, z);
    
    ccsv_mapfn map = map_func_for(ctx->config.strategy);
    uint32_t addr = map(&ctx->config, &tmp);
    if (addr >= (uint32_t)ctx->config.geo_full) return 0;
    
    int slot = ccsv_find_slot(ctx, addr);
    if (slot >= 0) return ctx->entries[slot].value;
    return 0;  // not found = zero (virtual)
}

void ccsv_set(ccsv_virtual_ctx *ctx, int face, int x, int y, int z, int8_t val) {
    if (!ctx || !ctx->initialized) return;
    
    if (face < 0 || face >= ctx->config.faces) return;
    if (x < 0 || x >= ctx->config.W) return;
    if (y < 0 || y >= ctx->config.H) return;
    if (z < 0 || z >= ctx->config.L) return;
    
    ccsv_cell tmp = { .face = face, .x = x, .y = y, .z = z };
    tmp.global_idx = ccsv_global_idx(&ctx->config, face, x, y, z);
    
    ccsv_mapfn map = map_func_for(ctx->config.strategy);
    uint32_t addr = map(&ctx->config, &tmp);
    if (addr >= (uint32_t)ctx->config.geo_full) return;
    
    // Special case: setting to zero = delete
    if (val == 0) {
        int slot = ccsv_find_slot(ctx, addr);
        if (slot >= 0) {
            ctx->entries[slot].occupied = 255;  // tombstone
            ctx->count--;
        }
        return;
    }
    
    // Insert or update
    int slot = ccsv_find_slot(ctx, addr);
    if (slot >= 0) {
        ctx->entries[slot].value = val;
        return;
    }
    
    // New entry
    slot = -slot - 1;
    ctx->entries[slot].key = addr;
    ctx->entries[slot].value = val;
    ctx->entries[slot].face = face;
    ctx->entries[slot].x = x;
    ctx->entries[slot].y = y;
    ctx->entries[slot].z = z;
    ctx->entries[slot].global_idx = tmp.global_idx;
    ctx->entries[slot].occupied = 1;
    ctx->count++;
    
    // Resize if needed
    if (ctx->count >= ctx->max_load) {
        ccsv_resize(ctx);
    }
}

int ccsv_encode_all(ccsv_virtual_ctx *ctx, const ccsv_cell *cells, int n) {
    if (!ctx || !ctx->initialized || !cells || n <= 0) return -1;
    
    int collisions = 0;
    ccsv_mapfn map = map_func_for(ctx->config.strategy);
    
    for (int i = 0; i < n; i++) {
        if (cells[i].value == 0) continue;  // skip zeros (virtual)
        
        uint32_t addr = map(&ctx->config, &cells[i]);
        if (addr >= (uint32_t)ctx->config.geo_full) continue;
        
        int slot = ccsv_find_slot(ctx, addr);
        if (slot >= 0 && ctx->entries[slot].value != cells[i].value) {
            collisions++;
        }
        
        // Insert/update
        if (slot >= 0) {
            ctx->entries[slot].value = cells[i].value;
        } else {
            slot = -slot - 1;
            ctx->entries[slot].key = addr;
            ctx->entries[slot].value = cells[i].value;
            ctx->entries[slot].face = cells[i].face;
            ctx->entries[slot].x = cells[i].x;
            ctx->entries[slot].y = cells[i].y;
            ctx->entries[slot].z = cells[i].z;
            ctx->entries[slot].global_idx = cells[i].global_idx;
            ctx->entries[slot].occupied = 1;
            ctx->count++;
            
            if (ctx->count >= ctx->max_load) ccsv_resize(ctx);
        }
    }
    return collisions;
}

int ccsv_decode_all(ccsv_virtual_ctx *ctx, ccsv_cell *out, int max_out) {
    if (!ctx || !ctx->initialized || !out || max_out <= 0) return -1;
    
    ccsv_mapfn map = map_func_for(ctx->config.strategy);
    int written = 0;
    int total_cells = ccsv_total_cells(&ctx->config);
    
    // Decode by iterating all theoretical positions
    for (int i = 0; i < total_cells && written < max_out; i++) {
        ccsv_cell tmp;
        ccsv_from_idx(&ctx->config, i, &tmp.face, &tmp.x, &tmp.y, &tmp.z);
        tmp.global_idx = i;
        uint32_t addr = map(&ctx->config, &tmp);
        if (addr >= (uint32_t)ctx->config.geo_full) continue;
        
        int slot = ccsv_find_slot(ctx, addr);
        out[written] = tmp;
        out[written].value = (slot >= 0) ? ctx->entries[slot].value : 0;
        written++;
    }
    return written;
}

void ccsv_iterate(const ccsv_virtual_ctx *ctx, ccsv_iter_fn fn, void *user) {
    if (!ctx || !ctx->initialized || !fn) return;
    
    for (int i = 0; i < ctx->table_size; i++) {
        if (ctx->entries[i].occupied == 1) {
            fn(ctx->entries[i].face,
               ctx->entries[i].x,
               ctx->entries[i].y,
               ctx->entries[i].z,
               ctx->entries[i].value,
               user);
        }
    }
}

int ccsv_count(const ccsv_virtual_ctx *ctx) {
    return ctx ? ctx->count : 0;
}

int ccsv_verify(ccsv_virtual_ctx *ctx, const ccsv_cell *cells, int n) {
    if (!ctx || !cells || n <= 0) return -1;
    
    ccsv_cell *decoded = (ccsv_cell*)malloc(n * sizeof(ccsv_cell));
    if (!decoded) return -1;
    
    int decoded_n = ccsv_decode_all(ctx, decoded, n);
    int mismatches = 0;
    
    for (int i = 0; i < n && i < decoded_n; i++) {
        if (cells[i].value != decoded[i].value) mismatches++;
    }
    
    free(decoded);
    return mismatches;
}

const char *ccsv_strategy_name(ccsv_strategy s) {
    static const char *names[] = { "sequential", "stride37", "face_region", "grid" };
    if (s >= 0 && s < CCSV_COUNT) return names[s];
    return "unknown";
}

const ccsv_config *ccsv_get_config(const ccsv_virtual_ctx *ctx) {
    return ctx ? &ctx->config : NULL;
}

#endif /* CONTOUR_CODEC_VIRTUAL_IMPLEMENTATION */
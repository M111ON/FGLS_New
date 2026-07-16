/*
 * geo_pipeline.h — GeoPixel Full Pipeline: CPU + CUDA + DRamTile + GearShift
 *
 * Complete pipeline:
 *   1. Raw file → 64-byte chunks
 *   2. Chunks → GpAddr (tile_id, dim) via geo_field mapping
 *   3. Wallet seed + checksum per chunk
 *   4. Wang tile validation (Fibonacci 2&7 chords)
 *   5. Tantrix 256-state routing
 *   6. Cube build from coord records
 *   7. 6-face unfold
 *
 * CPU version: single-header C11, no dependencies beyond stdint/stdlib.
 * GPU version: launched via CUDA kernels (see geo_pipeline_gpu.cu).
 * DRamTile: intermediate data stored via dt_put/dt_get.
 * GearShift: priority streaming between pipeline stages.
 *
 * Constants match the Python reference (tools/geopixel_pipeline.py).
 */
#ifndef GEO_PIPELINE_H
#define GEO_PIPELINE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════
   CONSTANTS (from geopixel_pipeline.py)
   ═══════════════════════════════════════════════════════════════════ */

#define GP_CHUNK_SZ          64u
#define GP_FRAME_CYCLE      1440u
#define GP_FRAME_STRIDE       37u
#define GP_FRAME_FACE_SZ     120u
#define GP_FRAME_EDGES        12u
#define GP_FRAME_H_ACTIVE      9u
#define GP_FRAME_ICO_NODES   162u

#define GP_FULL             20736u   /* 144^2 */
#define GP_GOLDEN_PHI_FIXED 1618033u /* φ × 1e6 */

#define GP_LEVEL_2_FACES      42u    /* 10*2^2+2 */
#define GP_PENT_COUNT         12u

#define GP_WANG_WIN_SZ        12u
#define GP_WANG_WIN_COUNT    120u    /* 1440/12 */

#define GP_TANTRIX_NULL     0x00u
#define GP_TANTRIX_CROSS    0xAAu
#define GP_TANTRIX_MERGE    0x55u
#define GP_TANTRIX_SPLIT    0xFFu

#define GP_WALLET_SEED_MUL1 0xff51afd7ed558ccdULL
#define GP_WALLET_SEED_MUL2 0xc4ceb9fe1a85ec53ULL

/* ═══════════════════════════════════════════════════════════════════
   SECTION 1: Goldberg Sphere → GpAddr mapping
   ═══════════════════════════════════════════════════════════════════ */

static inline uint32_t gp_face_count(int level) {
    return (uint32_t)(10 * level * level + 2);
}

static inline void gp_chunk_to_addr(int level, uint64_t chunk_idx,
                                     uint32_t *tile_id, uint32_t *dim)
{
    uint32_t face_max = gp_face_count(level);
    *tile_id = (uint32_t)(chunk_idx % face_max);
    *dim     = (uint32_t)((chunk_idx / face_max) & 0x7Fu);
}

static inline int gp_is_pentagon(uint32_t tile_id) {
    return tile_id < GP_PENT_COUNT;
}

static inline int gp_is_zone_boundary(uint32_t tile_id) {
    return gp_is_pentagon(tile_id);
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 2: TRing walk (stride-37)
   ═══════════════════════════════════════════════════════════════════ */

static inline uint16_t tring_walk_enc(uint32_t tile_id) {
    return (uint16_t)((tile_id * GP_FRAME_STRIDE) % 720u);
}

static inline uint8_t tring_walk_spoke(uint32_t tile_id) {
    return (uint8_t)(tile_id % 6u);
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 3: Skeleton classification
   ═══════════════════════════════════════════════════════════════════ */

#define GP_SKEL_ID     0
#define GP_SKEL_FLAT   1
#define GP_SKEL_DIFF   2
#define GP_SKEL_BREF   3
#define GP_SKEL_GEOM   4
#define GP_SKEL_RAW    5

typedef struct {
    const uint8_t *last_chunk;
    uint32_t       zone_resets;
    uint32_t       hits[6];
    uint8_t        _buf[GP_CHUNK_SZ];
} GpSkelCtx;

static inline void gp_skel_init(GpSkelCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

static inline int gp_skel_encode(GpSkelCtx *ctx,
                                  const uint8_t chunk[GP_CHUNK_SZ],
                                  uint8_t *out_data, uint32_t *out_sz)
{
    if (!ctx->last_chunk) {
        ctx->hits[GP_SKEL_ID]++;
        ctx->last_chunk = chunk;
        memcpy(out_data, chunk, GP_CHUNK_SZ);
        *out_sz = GP_CHUNK_SZ;
        return GP_SKEL_ID;
    }

    /* FLAT: all bytes same */
    uint8_t first = chunk[0];
    int all_same = 1;
    for (uint32_t i = 1; i < GP_CHUNK_SZ; i++) {
        if (chunk[i] != first) { all_same = 0; break; }
    }
    if (all_same) {
        ctx->hits[GP_SKEL_FLAT]++;
        out_data[0] = first;
        *out_sz = 1;
        return GP_SKEL_FLAT;
    }

    /* DIFF: XOR with previous */
    uint8_t diff[GP_CHUNK_SZ];
    uint32_t nonzero = 0;
    for (uint32_t i = 0; i < GP_CHUNK_SZ; i++) {
        diff[i] = (uint8_t)(chunk[i] ^ ctx->last_chunk[i]);
        if (diff[i]) nonzero++;
    }
    if (nonzero < 16) {
        ctx->hits[GP_SKEL_DIFF]++;
        memcpy(out_data, diff, GP_CHUNK_SZ);
        *out_sz = GP_CHUNK_SZ;
        ctx->last_chunk = chunk;
        return GP_SKEL_DIFF;
    }

    /* RAW fallback */
    ctx->hits[GP_SKEL_RAW]++;
    memcpy(out_data, chunk, GP_CHUNK_SZ);
    *out_sz = GP_CHUNK_SZ;
    ctx->last_chunk = chunk;
    return GP_SKEL_RAW;
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 4: Wallet (CoordRecord + seed)
   ═══════════════════════════════════════════════════════════════════ */

static inline uint64_t wallet_chunk_seed(const uint8_t chunk[GP_CHUNK_SZ]) {
    uint64_t w[8];
    for (int i = 0; i < 8; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) {
            w[i] |= (uint64_t)chunk[i * 8 + j] << (j * 8);
        }
    }
    uint64_t s = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4] ^ w[5] ^ w[6] ^ w[7];
    s ^= s >> 33;
    s *= GP_WALLET_SEED_MUL1;
    s ^= s >> 33;
    s *= GP_WALLET_SEED_MUL2;
    s ^= s >> 33;
    return s;
}

static inline uint32_t wallet_xorfold64(const uint8_t chunk[GP_CHUNK_SZ]) {
    uint32_t acc = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t w = (uint32_t)chunk[i * 4 + 0]
                   | ((uint32_t)chunk[i * 4 + 1] << 8)
                   | ((uint32_t)chunk[i * 4 + 2] << 16)
                   | ((uint32_t)chunk[i * 4 + 3] << 24);
        acc ^= w;
    }
    return acc;
}

static inline uint32_t wallet_coord_pack(uint8_t face, uint8_t edge, uint8_t z) {
    return ((uint32_t)face << 24) | ((uint32_t)edge << 16) | ((uint32_t)z << 8);
}

static inline void wallet_coord_unpack(uint32_t packed,
                                        uint8_t *face, uint8_t *edge, uint8_t *z)
{
    if (face) *face = (uint8_t)(packed >> 24);
    if (edge) *edge = (uint8_t)(packed >> 16);
    if (z)    *z    = (uint8_t)(packed >> 8);
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 5: Wang tile validation (Fibonacci 2&7 chords)
   ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t xor_enc;
    uint8_t  edge_top;
    uint8_t  edge_top_b;
    uint8_t  edge_bot;
    uint8_t  edge_bot_b;
    uint16_t tile_id;
    uint16_t skip_mask;
    int      valid;
} GpWangWindow;

static inline int wang_chord_a(int enc) {
    return (enc * 2) % 9;
}

static inline int wang_chord_b(int enc) {
    return (enc * 7) % 9;
}

static inline int wang_is_369(int enc) {
    int d = enc % 9;
    return d == 0 || d == 3 || d == 6;
}

static inline int wang_chord_valid(int enc) {
    int a = wang_chord_a(enc);
    int b = wang_chord_b(enc);
    if (enc % 9 == 0) return a == 0 && b == 0;
    return (a + b) == 9;
}

static inline void wang_frame_at(int enc, uint32_t *face, uint32_t *slot, int *is_skip) {
    *face = (uint32_t)(enc / GP_FRAME_FACE_SZ);
    *slot = (uint32_t)(enc % GP_FRAME_FACE_SZ);
    *is_skip = (enc % GP_FRAME_EDGES) >= GP_FRAME_H_ACTIVE;
}

static inline void wang_compute_window(int win_idx, GpWangWindow *w) {
    uint32_t base_t = (uint32_t)win_idx * GP_WANG_WIN_SZ;
    uint32_t xor_acc = 0;
    uint16_t skip_mask = 0;
    int first_enc = 0, last_enc = 0;
    uint32_t tile_id = 0;

    for (uint32_t i = 0; i < GP_WANG_WIN_SZ; i++) {
        uint32_t t = base_t + i;
        int enc = (int)((t * GP_FRAME_STRIDE) % GP_FRAME_CYCLE);
        xor_acc ^= (uint32_t)enc;
        uint32_t face;
        uint32_t slot;
        int is_skip;
        wang_frame_at(enc, &face, &slot, &is_skip);
        if (is_skip) skip_mask |= (uint16_t)(1u << i);
        if (i == 0) {
            first_enc = enc;
            tile_id = face;
        }
        if (i == GP_WANG_WIN_SZ - 1) {
            last_enc = enc;
        }
    }

    w->xor_enc   = xor_acc;
    w->edge_top   = (uint8_t)wang_chord_a(first_enc);
    w->edge_top_b = (uint8_t)wang_chord_b(first_enc);
    w->edge_bot   = (uint8_t)wang_chord_a(last_enc);
    w->edge_bot_b = (uint8_t)wang_chord_b(last_enc);
    w->tile_id    = (uint16_t)tile_id;
    w->skip_mask  = skip_mask;
    w->valid      = 1;
}

static inline int wang_edge_valid(const GpWangWindow *prev, const GpWangWindow *curr) {
    return prev->edge_bot == curr->edge_top;
}

static inline int wang_tamper_check(const GpWangWindow *w) {
    int top_ok = (w->edge_top == 0 && w->edge_top_b == 0) ||
                 (w->edge_top + w->edge_top_b == 9);
    int bot_ok = (w->edge_bot == 0 && w->edge_bot_b == 0) ||
                 (w->edge_bot + w->edge_bot_b == 9);
    return top_ok && bot_ok;
}

/* Compute all Wang windows (CPU) */
static inline void wang_compute_all(GpWangWindow *windows, int n_windows) {
    for (int i = 0; i < n_windows; i++) {
        wang_compute_window(i, &windows[i]);
    }
}

/* Verify entire Wang layer */
static inline int wang_verify_all(const GpWangWindow *windows, int n_windows) {
    for (int i = 0; i < n_windows; i++) {
        if (!windows[i].valid) return 0;
    }
    for (int i = 1; i < n_windows; i++) {
        if (!wang_edge_valid(&windows[i-1], &windows[i])) return 0;
    }
    for (int i = 0; i < n_windows; i++) {
        if (!wang_tamper_check(&windows[i])) return 0;
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 6: Tantrix 256-state routing
   ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    GP_GATE_WARP      = 0,
    GP_GATE_COLLISION = 1,
    GP_GATE_ROUTE     = 2,
    GP_GATE_GROUND    = 3,
} GpTantrixGate;

static inline uint8_t tantrix_make(uint8_t entry, uint8_t exit, uint8_t spoke, uint8_t cls) {
    return (entry & 0x3) | ((exit & 0x3) << 2)
         | ((spoke & 0x3) << 4) | ((cls & 0x3) << 6);
}

static inline uint8_t tantrix_entry(uint8_t t)  { return t & 0x3; }
static inline uint8_t tantrix_exit(uint8_t t)   { return (t >> 2) & 0x3; }
static inline uint8_t tantrix_spoke(uint8_t t)  { return (t >> 4) & 0x3; }
static inline uint8_t tantrix_class(uint8_t t)  { return (t >> 6) & 0x3; }

static inline int tantrix_connects(uint8_t left, uint8_t right) {
    if (left == GP_TANTRIX_NULL || right == GP_TANTRIX_NULL) return 0;
    if (left == GP_TANTRIX_SPLIT || right == GP_TANTRIX_SPLIT) return 1;
    return tantrix_exit(left) == tantrix_entry(right);
}

static inline int tantrix_route(uint8_t tile, uint8_t incoming_gate,
                                 uint8_t *out_gate, const char **out_type)
{
    if (tile == GP_TANTRIX_NULL) {
        *out_gate = GP_GATE_GROUND;
        *out_type = "DROP";
        return 0;
    }
    if (tile == GP_TANTRIX_SPLIT) {
        *out_gate = incoming_gate;
        *out_type = "BROADCAST";
        return 1;
    }
    if (tile == GP_TANTRIX_MERGE) {
        *out_gate = GP_GATE_GROUND;
        *out_type = "MERGE";
        return 1;
    }
    if (tile == GP_TANTRIX_CROSS) {
        static const uint8_t cross_map[4] = {
            GP_GATE_COLLISION, GP_GATE_GROUND,
            GP_GATE_WARP, GP_GATE_ROUTE
        };
        *out_gate = cross_map[incoming_gate & 3];
        *out_type = "FORWARD";
        return 1;
    }

    if (tantrix_entry(tile) != incoming_gate) {
        *out_gate = GP_GATE_GROUND;
        *out_type = "DROP";
        return 0;
    }

    uint8_t eg = tantrix_exit(tile);
    uint8_t cls = tantrix_class(tile);
    if (cls == 1) eg ^= 0x3;       /* SKIP */
    else if (cls == 2) {           /* MIRROR */
        eg = ((eg & 1) << 1) | ((eg >> 1) & 1);
    }
    *out_gate = eg;
    *out_type = "FORWARD";
    return 1;
}

static inline int tantrix_route_simple(uint8_t tile, uint8_t incoming_gate) {
    const char *type;
    uint8_t gate;
    return tantrix_route(tile, incoming_gate, &gate, &type);
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 7: GeoFrame timeline
   ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    int      enc;
    uint32_t face;
    uint32_t slot;
    uint32_t group;
    uint32_t edge;
    int      is_skip;
    uint32_t step;
    uint32_t sub;
    uint32_t hilbert_group;
    uint32_t ico_idx;
    uint32_t phase;
} GpFrameInfo;

static inline int gp_frame_enc(int t) {
    return (int)(((uint32_t)t * GP_FRAME_STRIDE) % GP_FRAME_CYCLE);
}

static inline void gp_frame_at(int enc, GpFrameInfo *f) {
    f->enc   = enc;
    f->face  = (uint32_t)(enc / GP_FRAME_FACE_SZ);
    f->slot  = (uint32_t)(enc % GP_FRAME_FACE_SZ);
    f->group = f->face % 3;
    f->edge  = (uint32_t)(enc % 3);
    f->is_skip = (enc % GP_FRAME_EDGES) >= GP_FRAME_H_ACTIVE;
    f->step  = (uint32_t)((enc / 3) % 4);
    f->sub   = (uint32_t)(enc % 3);
    f->hilbert_group = (uint32_t)((enc / GP_FRAME_EDGES) % 3);
    f->ico_idx = (uint32_t)(enc % GP_FRAME_ICO_NODES);
    f->phase = (uint32_t)((enc / GP_FRAME_EDGES) % 12);
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 8: CoordRecord
   ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t file_idx;
    uint32_t chunk_idx;
    uint32_t tile_id;
    uint32_t dim;
    uint32_t coord_packed;
    uint8_t  face;
    uint8_t  edge;
    uint8_t  z;
    uint64_t seed;
    uint32_t checksum;
    uint32_t fast_sig;
    int      skel_strategy;
    uint16_t tring_enc;
    uint8_t  spoke;
} GpCoordRecord;

static inline void gp_coord_fill(GpCoordRecord *cr,
                                  uint32_t chunk_idx, uint32_t tile_id, uint32_t dim,
                                  const uint8_t chunk[GP_CHUNK_SZ],
                                  int skel_strategy)
{
    uint8_t face = (uint8_t)(tile_id % 12);
    uint8_t edge = (uint8_t)(dim % 5);
    uint8_t z    = (uint8_t)((tile_id / 12) & 0xFF);

    cr->file_idx      = 0;
    cr->chunk_idx     = chunk_idx;
    cr->tile_id       = tile_id;
    cr->dim           = dim;
    cr->coord_packed  = wallet_coord_pack(face, edge, z);
    cr->face          = face;
    cr->edge          = edge;
    cr->z             = z;
    cr->seed          = wallet_chunk_seed(chunk);
    cr->checksum      = wallet_xorfold64(chunk);
    cr->fast_sig      = (uint32_t)(cr->seed & 0xFFFFFFFFu);
    cr->skel_strategy = skel_strategy;
    cr->tring_enc     = tring_walk_enc(tile_id);
    cr->spoke         = tring_walk_spoke(tile_id);
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 9: 6-face cube operations
   ═══════════════════════════════════════════════════════════════════ */

#define GP_FACE_FRONT  0
#define GP_FACE_BACK   1
#define GP_FACE_LEFT   2
#define GP_FACE_RIGHT  3
#define GP_FACE_TOP    4
#define GP_FACE_BOTTOM 5

typedef struct {
    float *faces[6];       /* each face = side × side floats */
    uint32_t side;
} GpCubeFaces;

static inline void gp_unfold_cube(const float *cube, uint32_t side,
                                   GpCubeFaces *out)
{
    uint32_t s2 = side * side;
    for (int f = 0; f < 6; f++) {
        out->faces[f] = (float *)malloc(s2 * sizeof(float));
    }
    out->side = side;

    /* front: z = side-1 */
    for (uint32_t y = 0; y < side; y++)
        for (uint32_t x = 0; x < side; x++)
            out->faces[GP_FACE_FRONT][y * side + x] = cube[(y * side + x) * side + (side - 1)];

    /* back: z = 0 */
    for (uint32_t y = 0; y < side; y++)
        for (uint32_t x = 0; x < side; x++)
            out->faces[GP_FACE_BACK][y * side + x] = cube[(y * side + x) * side + 0];

    /* left: x = 0 */
    for (uint32_t z = 0; z < side; z++)
        for (uint32_t y = 0; y < side; y++)
            out->faces[GP_FACE_LEFT][z * side + y] = cube[(y * side + 0) * side + z];

    /* right: x = side-1 */
    for (uint32_t z = 0; z < side; z++)
        for (uint32_t y = 0; y < side; y++)
            out->faces[GP_FACE_RIGHT][z * side + y] = cube[(y * side + (side - 1)) * side + z];

    /* top: y = side-1 */
    for (uint32_t z = 0; z < side; z++)
        for (uint32_t x = 0; x < side; x++)
            out->faces[GP_FACE_TOP][z * side + x] = cube[((side - 1) * side + x) * side + z];

    /* bottom: y = 0 */
    for (uint32_t z = 0; z < side; z++)
        for (uint32_t x = 0; x < side; x++)
            out->faces[GP_FACE_BOTTOM][z * side + x] = cube[(0 * side + x) * side + z];
}

static inline void gp_free_faces(GpCubeFaces *f) {
    for (int i = 0; i < 6; i++) {
        if (f->faces[i]) { free(f->faces[i]); f->faces[i] = NULL; }
    }
}

/* ═══════════════════════════════════════════════════════════════════
   SECTION 10: Full CPU Pipeline
   ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t   data_size;
    uint32_t   n_chunks;
    uint32_t   face_max;
    uint32_t   cube_side;
    size_t     face_bytes;
    size_t     cube_bytes;
    double     ratio;
} GpPipelineStats;

typedef struct {
    int           wang_valid;
    int           tantrix_valid;
    uint32_t      skel_hits[6];
    uint32_t      coord_count;
    double        total_time_ms;
    GpPipelineStats stats;
    float        *cube;              /* side × side × side */
    GpCubeFaces   faces;
    GpCoordRecord *records;
} GpPipelineResult;

/*
 * Run full CPU pipeline: data → chunks → GpAddr → wallet → Wang → Tantrix → cube → 6 faces
 * Returns 0 on success, -1 on error.
 */
static inline int gp_pipeline_encode(const uint8_t *data, size_t data_sz,
                                      int gp_level, GpPipelineResult *result)
{
    if (!data || !result || data_sz == 0) return -1;
    memset(result, 0, sizeof(*result));

    uint32_t face_max = gp_face_count(gp_level);
    uint32_t n_chunks = (uint32_t)((data_sz + GP_CHUNK_SZ - 1) / GP_CHUNK_SZ);
    uint32_t side = 100;

    /* Allocate cube */
    size_t cube_sz = (size_t)side * side * side * sizeof(float);
    float *cube = (float *)calloc(side * side * side, sizeof(float));
    if (!cube) return -1;

    GpCoordRecord *records = (GpCoordRecord *)calloc(n_chunks, sizeof(GpCoordRecord));
    if (!records) { free(cube); return -1; }

    GpSkelCtx skel;
    gp_skel_init(&skel);

    /* Step 1-4: Chunk → GeoField → Wallet → Skeleton */
    uint8_t chunk_buf[GP_CHUNK_SZ];
    for (uint32_t ci = 0; ci < n_chunks; ci++) {
        size_t offset = (size_t)ci * GP_CHUNK_SZ;
        size_t remain = (offset < data_sz) ? data_sz - offset : 0;
        memset(chunk_buf, 0, GP_CHUNK_SZ);
        if (remain > 0) {
            size_t cp = (remain < GP_CHUNK_SZ) ? remain : GP_CHUNK_SZ;
            memcpy(chunk_buf, data + offset, cp);
        }

        uint32_t tile_id, dim;
        gp_chunk_to_addr(gp_level, ci, &tile_id, &dim);

        uint8_t skel_data[GP_CHUNK_SZ];
        uint32_t skel_sz;
        int skel_strat = gp_skel_encode(&skel, chunk_buf, skel_data, &skel_sz);

        gp_coord_fill(&records[ci], ci, tile_id, dim, chunk_buf, skel_strat);
    }

    result->coord_count = n_chunks;
    memcpy(result->skel_hits, skel.hits, sizeof(skel.hits));

    /* Step 5: Wang tile validation */
    GpWangWindow windows[GP_WANG_WIN_COUNT];
    wang_compute_all(windows, GP_WANG_WIN_COUNT);
    result->wang_valid = wang_verify_all(windows, GP_WANG_WIN_COUNT);

    /* Step 6: Tantrix routing */
    result->tantrix_valid = 1;
    for (uint32_t ci = 0; ci < n_chunks && result->tantrix_valid; ci++) {
        uint8_t entry_gate = records[ci].edge % 4;
        uint8_t tile = tantrix_make(entry_gate, (entry_gate + 1) % 4,
                                     records[ci].spoke % 4, 0);
        uint8_t out_gate;
        const char *out_type;
        int ok = tantrix_route(tile, entry_gate, &out_gate, &out_type);
        if (!ok || strcmp(out_type, "DROP") == 0) {
            result->tantrix_valid = 0;
        }
    }

    /* Step 7: Build cube from coord records */
    for (uint32_t ci = 0; ci < n_chunks; ci++) {
        uint32_t x = (ci / (side * side)) % side;
        uint32_t y = (ci / side) % side;
        uint32_t z_idx = ci % side;

        int enc = gp_frame_enc((int)(ci % GP_FRAME_CYCLE));
        GpFrameInfo frame;
        gp_frame_at(enc, &frame);

        cube[(y * side + x) * side + z_idx] = (float)(
            records[ci].face * 100.0f +
            records[ci].edge * 50.0f +
            frame.slot * 10.0f +
            records[ci].dim +
            frame.ico_idx +
            frame.phase * 5.0f
        );
    }

    /* Step 8: Unfold */
    GpCubeFaces faces;
    gp_unfold_cube(cube, side, &faces);

    size_t face_bytes = (size_t)side * side * sizeof(float) * 6;

    /* Fill result */
    result->cube = cube;
    result->faces = faces;
    result->records = records;
    result->stats.data_size = data_sz;
    result->stats.n_chunks = n_chunks;
    result->stats.face_max = face_max;
    result->stats.cube_side = side;
    result->stats.face_bytes = face_bytes;
    result->stats.cube_bytes = cube_sz;
    result->stats.ratio = cube_sz > 0 ? (double)cube_sz / (double)face_bytes : 0.0;

    return 0;
}

/* Free pipeline result */
static inline void gp_pipeline_free(GpPipelineResult *result) {
    if (result->cube) { free(result->cube); result->cube = NULL; }
    if (result->records) { free(result->records); result->records = NULL; }
    gp_free_faces(&result->faces);
    memset(result, 0, sizeof(*result));
}

#endif /* GEO_PIPELINE_H */

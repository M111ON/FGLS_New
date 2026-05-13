/*
 * geo_goldberg_tile.h — Goldberg GP(1,1) Tile Integration for GeoPixel
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Two integration points:
 *
 *   POINT 3 — Circuit-based residual routing (per tile)
 *     ggt_tile_scan()  → GGTileResult { circuit_fired, max_tension }
 *     OR aggressive: if circuit says LOOSE → override to LOOSE regardless of gerr
 *
 *   POINT 2 — Blueprint dedup index (per 144-tile window)
 *     ggt_window_feed()  → accumulate stamp per tile
 *     ggt_window_flush() → GGBlueprint { stamp_hash, circuit_map, window_id }
 *     Blueprint stored in index; matching stamp_hash → ref pointer dedup
 *
 * Self-contained — baked LUT inline, no external POGLS headers needed.
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef GEO_GOLDBERG_TILE_H
#define GEO_GOLDBERG_TILE_H

#include <stdint.h>
#include <string.h>

/* ── GP(1,1) constants ─────────────────────────────────────────────── */
#define GGT_N_FACES     32   /* 12 pentagon + 20 hexagon              */
#define GGT_N_TRIGAP    60   /* triangle gaps = I-symmetry order      */
#define GGT_N_PAIRS      6   /* bipolar pentagon pairs                */
#define GGT_FLUSH_PERIOD 144 /* Fibonacci flush boundary              */

/* ── Baked LUT: pentagon opposite pairs ────────────────────────────── */
static const uint8_t GGT_PEN_PAIRS[6][2] = {
    {0,3},{1,2},{4,7},{5,10},{6,8},{9,11}
};

/* ── Baked LUT: triangle gap adjacency [gap][face_a,face_b,face_c] ── */
static const uint8_t GGT_TRIGAP_ADJ[60][3] = {
    { 0,12,13},{ 1,14,15},{ 2,16,17},{ 3,16,18},{ 4,12,19},
    { 4,19,20},{ 5,21,22},{ 4,17,20},{ 6,15,23},{ 5,18,21},
    { 3,21,23},{ 0,12,19},{ 5,22,24},{ 7,21,22},{ 7,21,23},
    { 8,12,25},{ 9,22,24},{ 1,26,27},{ 7,22,27},{10,19,28},
    {11,20,29},{ 0,26,28},{ 4,12,25},{10,20,29},{ 2,25,30},
    { 7,15,27},{ 3,18,21},{ 3,23,31},{ 6,23,31},{10,14,28},
    { 0,13,26},{ 8,24,30},{ 8,13,24},{ 6,14,15},{ 9,13,24},
    {11,16,17},{ 1,26,28},{10,14,29},{ 2,18,30},{ 1,14,28},
    {10,19,20},{ 9,13,26},{ 2,17,25},{ 2,16,18},{11,16,31},
    {11,17,20},{ 8,25,30},{ 9,22,27},{ 1,15,27},{ 3,16,31},
    { 6,14,29},{ 6,29,31},{ 5,18,30},{ 8,12,13},{ 7,15,23},
    { 0,19,28},{ 9,26,27},{ 5,24,30},{ 4,17,25},{11,29,31},
};

/* ── Result from per-tile scan ─────────────────────────────────────── */
typedef struct {
    uint8_t  circuit_fired;  /* bitmask: bit[p]=1 if bipolar pair p fired */
    uint8_t  n_circuits;     /* popcount(circuit_fired)                   */
    uint8_t  max_tension_gap;/* gap_id with highest XOR tension           */
    uint32_t max_tension;    /* tension value at max gap                  */
    uint32_t spatial_xor;    /* XOR fold of all 32 face diffs             */
    uint32_t stamp;          /* tile fingerprint: spatial_xor ^ n_circuits*/
} GGTileResult;

/* ── Blueprint: window-level fingerprint (every 144 tiles) ─────────── */
typedef struct {
    uint32_t stamp_hash;     /* XOR chain of all tile stamps in window    */
    uint8_t  circuit_map;    /* OR of all circuit_fired in window         */
    uint32_t event_count;    /* tiles in this window                      */
    uint64_t window_id;      /* monotonic flush counter                   */
} GGBlueprint;

/* ── Window accumulator ─────────────────────────────────────────────── */
typedef struct {
    uint32_t stamp_hash;
    uint8_t  circuit_map;
    uint32_t event_count;
    uint64_t window_id;
    uint8_t  ready;          /* 1 = blueprint ready after flush           */
    GGBlueprint blueprint;
} GGWindow;

/* ── Internal scanner state (stack-allocated per tile call) ─────────── */
typedef struct {
    uint32_t face_state[GGT_N_FACES];
    uint32_t face_diff [GGT_N_FACES];
} GGScan;

/* ─────────────────────────────────────────────────────────────────────
 * POINT 3: Per-tile scan
 *
 * Samples the tile by mapping 32 pixel positions → 32 Goldberg faces.
 * Pixel sampling: stride across tile to cover spatial distribution.
 * Each face gets one pixel value XOR'd into its state.
 *
 * Usage:
 *   GGTileResult r = ggt_tile_scan(iY, x0,y0,x1,y1, W);
 *   if (r.n_circuits >= 3) → LOOSE or DELTA
 * ───────────────────────────────────────────────────────────────────── */
static inline GGTileResult ggt_tile_scan(const int *iY,
                                          int x0, int y0, int x1, int y1,
                                          int W)
{
    int tw = x1 - x0, th = y1 - y0;
    GGScan s; memset(&s, 0, sizeof(s));

    /* sample 32 positions across tile → map to faces 0..31
     * use pseudo-uniform stride: face_id = sample_index % 32
     * sample positions spread across tile using 2D stride */
    int n_samples = tw * th;
    if(n_samples > 0){
        /* stride to pick ~32 representative pixels */
        int stride = (n_samples + 31) / 32;
        if(stride < 1) stride = 1;
        int si = 0;
        for(int y = y0; y < y1 && si < GGT_N_FACES; y++){
            for(int x = x0; x < x1 && si < GGT_N_FACES; x++){
                /* select every stride-th pixel */
                int linear = (y - y0) * tw + (x - x0);
                if(linear % stride == 0){
                    uint32_t pix = (uint32_t)(iY[y * W + x] & 0xFF);
                    uint8_t  fid = (uint8_t)(si % GGT_N_FACES);
                    s.face_diff[fid]  = s.face_state[fid] ^ pix;
                    s.face_state[fid] = pix;
                    si++;
                }
            }
        }
    }

    /* compute bipolar circuit diffs */
    uint8_t circuit_fired = 0;
    for(int p = 0; p < GGT_N_PAIRS; p++){
        uint8_t pos = GGT_PEN_PAIRS[p][0];
        uint8_t neg = GGT_PEN_PAIRS[p][1];
        uint32_t cd = s.face_state[pos] ^ s.face_state[neg];
        if(cd > 0) circuit_fired |= (uint8_t)(1u << p);
    }

    /* find max tension gap */
    uint8_t  max_gap = 0;
    uint32_t max_ten = 0;
    for(int i = 0; i < GGT_N_TRIGAP; i++){
        uint32_t t = s.face_diff[GGT_TRIGAP_ADJ[i][0]]
                   ^ s.face_diff[GGT_TRIGAP_ADJ[i][1]]
                   ^ s.face_diff[GGT_TRIGAP_ADJ[i][2]];
        if(t > max_ten){ max_ten = t; max_gap = (uint8_t)i; }
    }

    /* spatial XOR fold */
    uint32_t sxor = 0;
    for(int i = 0; i < GGT_N_FACES; i++) sxor ^= s.face_diff[i];

    /* popcount */
    uint8_t nc = 0;
    uint8_t tmp = circuit_fired;
    while(tmp){ nc += tmp & 1; tmp >>= 1; }

    GGTileResult r;
    r.circuit_fired   = circuit_fired;
    r.n_circuits      = nc;
    r.max_tension_gap = max_gap;
    r.max_tension     = max_ten;
    r.spatial_xor     = sxor;
    r.stamp           = sxor ^ (uint32_t)nc;
    return r;
}

/* ── grad mode constants (mirror from encoder) ─────────────────────── */
#define GGT_MODE_NORMAL  0
#define GGT_MODE_LOOSE   1
#define GGT_MODE_DELTA   2

/* ─────────────────────────────────────────────────────────────────────
 * POINT 3: OR-aggressive mode decision
 *
 * Original gerr-based decision comes in as base_mode.
 * Goldberg can UPGRADE to LOOSE or DELTA — never downgrade.
 *
 *   n_circuits == 0      → trust base_mode (no signal)
 *   n_circuits 1-2       → LOOSE if base was NORMAL (gentle push)
 *   n_circuits 3-4       → LOOSE always
 *   n_circuits 5-6       → DELTA (high spatial complexity)
 * ───────────────────────────────────────────────────────────────────── */
static inline int ggt_mode_override(int base_mode, const GGTileResult *r)
{
    if(r->n_circuits == 0) return base_mode;  /* no signal → keep original */

    int gb_mode;
    if     (r->n_circuits >= 5) gb_mode = GGT_MODE_DELTA;
    else if(r->n_circuits >= 3) gb_mode = GGT_MODE_LOOSE;
    else                        gb_mode = GGT_MODE_LOOSE; /* 1-2 → gentle push */

    /* OR aggressive: take the more permissive of the two */
    if(gb_mode > base_mode) return gb_mode;
    return base_mode;
}

/* ─────────────────────────────────────────────────────────────────────
 * POINT 2: Window accumulator
 * ───────────────────────────────────────────────────────────────────── */
static inline void ggt_window_init(GGWindow *w){
    memset(w, 0, sizeof(GGWindow));
}

static inline void ggt_window_feed(GGWindow *w, const GGTileResult *r){
    w->stamp_hash  ^= r->stamp;
    w->circuit_map |= r->circuit_fired;
    w->event_count++;
    w->ready = 0;

    if(w->event_count % GGT_FLUSH_PERIOD == 0){
        w->blueprint.stamp_hash  = w->stamp_hash;
        w->blueprint.circuit_map = w->circuit_map;
        w->blueprint.event_count = w->event_count;
        w->blueprint.window_id   = w->window_id++;
        w->ready = 1;
        /* reset window accumulators but keep window_id */
        w->stamp_hash  = 0;
        w->circuit_map = 0;
    }
}

static inline GGBlueprint ggt_window_flush(GGWindow *w){
    /* force flush remaining tiles (partial window) */
    w->blueprint.stamp_hash  = w->stamp_hash;
    w->blueprint.circuit_map = w->circuit_map;
    w->blueprint.event_count = w->event_count;
    w->blueprint.window_id   = w->window_id++;
    w->ready = 0;
    return w->blueprint;
}

#endif /* GEO_GOLDBERG_TILE_H */

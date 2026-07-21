/*
 * gls_enclosure.h — Entropy Enclosure
 * ═══════════════════════════════════════════════════════════════
 *
 * "จับโจร" — หั่น RDH hexagon เป็น fixed-width chunk, auto-align/auto-fit
 *
 * Architecture:
 *   RDH (point cast) → TW_capture → Enclosure → Container → Frame Seek
 *
 * Enclosure = fixed-width chunker ที่:
 *   1. ลากกลับบ้าน — trace path กลับ home จาก data's own signature
 *   2. ถึงบ้าน → ระเบิด → hexagon spread (6-fold symmetry)
 *   3. จัด hexagon ลง chunk ((48×3)×144)×S
 *   4. auto-align (data = address = position on field)
 *   5. auto-fit (container รับ chunk แบบ deterministic)
 *
 * Scale levels: S ∈ {4, 12, 16, ...} = #², #³, #⁴
 *   Base chunk:   144 × 144 = (48×3)×144 = 20,736 bytes
 *   Scaled chunk: 20,736 × S bytes
 *
 * Constants (DGLS core):
 *   GEO_BLOCK = 48    — Metatron atomic unit (Hilbert block)
 *   GEO_TOWER = 144   — 48×3 (stride-3 tower)
 *   GEO_FULL  = 20736 — 144² (field)
 *
 * ═══════════════════════════════════════════════════════════════
 * No malloc in hot path.  No float.  O(1) operations.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef GLS_ENCLOSURE_H
#define GLS_ENCLOSURE_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>

/* RDH — pure integer address derivation (no hash, no byte scan) */
#include "rdh_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
   CONSTANTS — fixed geometry
   ═══════════════════════════════════════════════════════════════════════ */

#define ENC_BLOCK          48u     /* Metatron atom (4×4×3 Hilbert unit)   */
#define ENC_TOWER          144u    /* 48 × 3 (stride-3 tower)              */
#define ENC_FIELD          144u    /* field dimension (width = height)     */
#define ENC_FULL           20736u  /* 144² = total positions in field      */
#define ENC_HEX_RADIUS     3u      /* hexagon spread radius (7-cell hex)   */
#define ENC_HEX_CELLS      7u      /* center + 6 surround = 7 cells        */
#define ENC_STRIDE_6       6u      /* 6-fold symmetry on 12-gon            */
#define ENC_DODECA_FACES   12u     /* 12 pentagon faces / shell ticks      */
#define ENC_SLOTS_60       60u     /* TW capture slots (10 sectors ×6)     */
#define ENC_SLOTS_72       72u     /* Capture Twin: 60+10shared+2pentagon  */

/* ═══════════════════════════════════════════════════════════════════════
   SCALE PRESETS — chunk size = ENC_FULL × scale
   ═══════════════════════════════════════════════════════════════════════
   Scale 4  → 82,944  B  (general data)
   Scale 12 → 248,832 B  (stride-3, medium tensor)
   Scale 16 → 331,776 B  (bull/boss level, #⁴)
   ═══════════════════════════════════════════════════════════════════════ */

#define ENC_SCALE_4        4u
#define ENC_SCALE_12       12u
#define ENC_SCALE_16       16u

/* ═══════════════════════════════════════════════════════════════════════
   CHUNK SIZE — compute from scale
   ═══════════════════════════════════════════════════════════════════════ */

#define ENC_CHUNK_SIZE(s)  (ENC_FULL * (s))  /* bytes */
#define ENC_CHUNK_BLOCKS(s) (ENC_CHUNK_SIZE(s) / ENC_BLOCK)  /* number of 48B blocks */

/* ═══════════════════════════════════════════════════════════════════════
   ENCLOSURE CONFIG
   ═══════════════════════════════════════════════════════════════════════
   One config → one fixed-width chunk shape.
   Different scales = different sizes.  Config is immutable after init.
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t  scale;           /* scale factor: 4, 12, 16, ...          */
    uint32_t  chunk_size;      /* bytes: ENC_FULL × scale               */
    uint32_t  n_blocks;        /* 48-byte blocks per chunk              */
    uint32_t  field_dim;       /* field dimension after scaling          */
    uint32_t  field_total;     /* field positions after scaling          */
} EncConfig;

/* ── Init config from scale ─────────────────────────────────────────-- */

static inline EncConfig enc_config(uint32_t scale) {
    EncConfig cfg;
    cfg.scale        = scale;
    cfg.chunk_size   = ENC_CHUNK_SIZE(scale);
    cfg.n_blocks     = ENC_CHUNK_BLOCKS(scale);
    cfg.field_dim    = ENC_FIELD;        /* field grid is always 144×144 */
    cfg.field_total  = ENC_FULL;         /* positions on field           */
    return cfg;
}

/* ═══════════════════════════════════════════════════════════════════════
   HOME FINDING — "ลากกลับบ้าน"
   ═══════════════════════════════════════════════════════════════════════
   Data's own byte values define stride direction on 12-gon topology.
   Walking N steps → reaches "home" = position on 144×144 field.
   
   Algorithm:
     data → byte[i % 48] → stride step on 12-gon → accumulate position
     After N steps → (x, y) on field → HOME
   
   The path walked IS the address IS the DNA IS the seed.
   ═══════════════════════════════════════════════════════════════════════ */

/* Walk data bytes through 12-gon topology, return home field position.
 * data   : pointer to data bytes (size should be >= 48)
 * len    : number of bytes to read for path
 * field_w: field width (144 for unscaled, scaled for larger)
 * x, y   : output home coordinates on field */
static inline void enc_find_home(const uint8_t *data, uint32_t len,
                                 uint32_t field_w,
                                 uint32_t *x, uint32_t *y) 
{
    int32_t acc_x = 0, acc_y = 0;
    uint32_t m = field_w - 1;  /* mod mask (power of 2 or safe mod) */
    
    for (uint32_t i = 0; i < len; i++) {
        uint32_t b = data[i % 48];
        /* 12-gon stride: byte's low 4 bits → direction (0..11) */
        uint32_t dir = b & 0x0F;
        /* Each direction maps to a (dx, dy) step on the 12-gon */
        switch (dir) {
            case 0:  acc_x++;                break;  /* E */
            case 1:  acc_x++; acc_y++;        break;  /* NE */
            case 2:           acc_y++;        break;  /* N */
            case 3:  acc_x--; acc_y++;        break;  /* NW */
            case 4:  acc_x--;                break;  /* W */
            case 5:  acc_x--; acc_y--;        break;  /* SW */
            case 6:           acc_y--;        break;  /* S */
            case 7:  acc_x++; acc_y--;        break;  /* SE */
            case 8:  acc_x += 2;             break;  /* E2 */
            case 9:  acc_x++; acc_y += 2;     break;  /* N2E */
            case 10: acc_x--; acc_y += 2;     break;  /* N2W */
            case 11: acc_x -= 2;             break;  /* W2 */
            default: break;
        }
    }
    
    /* Fold into field — handle negative safely */
    if (field_w > 0 && (field_w & (field_w - 1)) == 0) {
        *x = (uint32_t)(acc_x) & (field_w - 1);
        *y = (uint32_t)(acc_y) & (field_w - 1);
    } else {
        *x = (uint32_t)((acc_x % (int32_t)field_w + (int32_t)field_w) % (int32_t)field_w);
        *y = (uint32_t)((acc_y % (int32_t)field_w + (int32_t)field_w) % (int32_t)field_w);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   HEXAGON SPREAD — 6-fold symmetry around home
   ═══════════════════════════════════════════════════════════════════════
   When data reaches home → RDH "time bomb" explodes → hexagon mesh
   spreads in 6 directions (stride-6 on 12-gon = opposite pairs).
   
   Hexagon layout (radius=3):
          (0)
      (5) │ (1)
        \ │ /
    (4)──┤H├──(1?)
        / │ \
      (3) │ (2)
          (?)
   
   Actually (for axial coords):
         (-1,1) (0,1)
     (-1,0)  H  (1,0)
         (0,-1) (1,-1)
   ═══════════════════════════════════════════════════════════════════════ */

/* Pre-computed hex neighbors in axial coordinates */
#define ENC_HEX_NB 6

/* 6 directions in axial coords (pointy-top hex, 12-gon alignment) */
static const int8_t ENC_HEX_DIR[6][2] = {
    { 0, 1},  /* N   */
    { 1, 0},  /* NE  */
    { 1,-1},  /* SE  */
    { 0,-1},  /* S   */
    {-1, 0},  /* SW  */
    {-1, 1},  /* NW  */
};

/* Spread hexagon around home, return cell positions.
 * Returns number of hex cells filled (center + 6 = 7 at radius 1).
 * Cells[0] = home position. Cells[1..6] = 6 directions. */
static inline int enc_hexagon_spread(uint32_t home_x, uint32_t home_y,
                                      uint32_t field_w,
                                      uint32_t cells[][2], uint32_t max_cells) 
{
    if (max_cells < 7) return 0;
    
    /* Cell 0 = home */
    cells[0][0] = home_x;
    cells[0][1] = home_y;
    
    uint32_t m = field_w - 1;
    int is_pow2 = (field_w & (field_w - 1)) == 0;
    
    for (int d = 0; d < 6; d++) {
        int32_t nx = (int32_t)home_x + ENC_HEX_DIR[d][0];
        int32_t ny = (int32_t)home_y + ENC_HEX_DIR[d][1];
        
        /* Wrap around field edges */
        if (is_pow2) {
            cells[d + 1][0] = (uint32_t)(nx) & (field_w - 1);
            cells[d + 1][1] = (uint32_t)(ny) & (field_w - 1);
        } else {
            cells[d + 1][0] = (uint32_t)((nx % (int32_t)field_w + (int32_t)field_w) % (int32_t)field_w);
            cells[d + 1][1] = (uint32_t)((ny % (int32_t)field_w + (int32_t)field_w) % (int32_t)field_w);
        }
    }
    
    return 7;  /* center + 6 neighbors */
}

/* ═══════════════════════════════════════════════════════════════════════
   AUTO-ALIGN — data places itself on field
   ═══════════════════════════════════════════════════════════════════════
   Data = address = DNA = seed.
   
   The chunk address (position in container) is derived from the data's
   home position on the field.  No external mapping table needed.
   
   chunk_idx = home_y * (field_w / chunk_grid) + home_x / chunk_grid
   where chunk_grid tells how many chunks fit across the field
   ═══════════════════════════════════════════════════════════════════════ */

/* Get chunk grid — how many chunks fit across the field.
 * At scale=S, chunk size = ENC_FULL×S bytes, so:
 *   chunks_wide = field_w / 144 (always 1 for base scale)
 * Actually: each chunk covers ENC_FULL positions.
 * At scale=1, 1 chunk = entire field.
 * At scale=4, 4 chunks = entire field (2×2 grid).  And so on.
 */
static inline uint32_t enc_chunks_across(uint32_t scale) {
    /* chunk grid = √scale (must be integer for grid to work) */
    /* Only works for perfect squares: 4→2, 16→4 */
    /* For non-squares like 12: use strip layout (1×N) */
    uint32_t grid;
    switch (scale) {
        case 4:  grid = 2; break;   /* 2×2 tiles */
        case 12: grid = 1; break;   /* strip */
        case 16: grid = 4; break;   /* 4×4 tiles */
        default:
            /* General case: try sqrt */
            for (grid = 1; grid * grid < scale; grid++);
            if (grid * grid != scale) grid = 1;  /* fallback strip */
            break;
    }
    return grid;
}

/* Compute chunk index from home position.
 * Grid layout: chunks_wide × chunks_tall covers the field.
 * chunk_idx = (home_y / chunk_h) * chunks_wide + (home_x / chunk_w) */
static inline uint32_t enc_chunk_idx(uint32_t home_x, uint32_t home_y,
                                      uint32_t field_w, uint32_t scale) 
{
    uint32_t grid = enc_chunks_across(scale);
    uint32_t chunk_w = field_w / grid;
    uint32_t chunk_h = field_w / grid;
    uint32_t cx = home_x / chunk_w;
    uint32_t cy = home_y / chunk_h;
    return cy * grid + cx;
}

/* ═══════════════════════════════════════════════════════════════════════
   AUTO-FIT — container placement
   ═══════════════════════════════════════════════════════════════════════
   Container is a sequential store of fixed-width chunks.
   Auto-fit means:
     1. Chunk size is known (from EncConfig)
     2. Chunk address on field is known (from auto-align)
     3. Container places chunk at: base + chunk_idx × chunk_size
   
   This is O(1), deterministic, no resize, no rehash.
   ═══════════════════════════════════════════════════════════════════════ */

/* Compute container byte offset for chunk at given index */
static inline size_t enc_chunk_offset(uint32_t chunk_idx, uint32_t chunk_size) {
    return (size_t)chunk_idx * chunk_size;
}

/* ═══════════════════════════════════════════════════════════════════════
   PATH WHISTLE — "ผิวปากเรียกไปเก็บ" (RDH-based, ไม่มี hash)
   ═══════════════════════════════════════════════════════════════════════
   After enclosure, each chunk's "whistle" signature comes from RDH's
   flat key — address derivation, not hashing.
   
   Whistle = RDH flat key for (home_y, home_x) on 144×144 field
           = ring × 144 + wedge  (where ring=home_y, wedge=home_x)
   
   RDH ~1.5 ns  (vs FNV-1a ~17.2 ns)
   RDH = pure integer O(1) — not byte-scanning
   RDH = reversible — whistle → home_y, home_x
   
   RDH config for enclosure field: {144, 144, 1, 1, 1}
     ring   = home_y  (0..143)
     wedge  = home_x  (0..143)
     mirror = 0
     u      = 0
     v      = 0
     total  = 144×144 = 20,736 = ENC_FULL
   ═══════════════════════════════════════════════════════════════════════ */

/* RDH config for 144×144 field — global singleton (read-only) */
#define RDH_FIELD_144  { 144, 144, 1, 1, 1 }

/* Compute whistle from home position via RDH.
 * O(1), no loop, no byte access, no hash table.
 * Returns flat key = whistle = address = signature in one. */
static inline int64_t enc_whistle_rdh(uint32_t home_x, uint32_t home_y) {
    static const RDHConfig rdh_144 = RDH_FIELD_144;
    return rdh_key(&rdh_144, (int64_t)home_y, (int64_t)home_x, 0, 0, 0);
}

/* Decompose whistle back to home position (reversible) */
static inline void enc_whistle_decompose(int64_t whistle,
                                          uint32_t *home_x, uint32_t *home_y) {
    static const RDHConfig rdh_144 = RDH_FIELD_144;
    int64_t ring, wedge, mirror, u;
    rdh_decompose(&rdh_144, whistle, &ring, &wedge, &mirror, &u);
    *home_y = (uint32_t)ring;
    *home_x = (uint32_t)wedge;
}

/* ═══════════════════════════════════════════════════════════════════════
   HIGH-LEVEL API — full lifecycle
   ═══════════════════════════════════════════════════════════════════════ */

/* Enclosure context — holds config + current state */
typedef struct {
    EncConfig  cfg;            /* config (scale, sizes)                  */
    uint32_t   n_chunks;       /* total chunks written                   */
    uint64_t   total_bytes;    /* total bytes processed                  */
    uint32_t   last_home_x;    /* last home position                     */
    uint32_t   last_home_y;    /* last home position                     */
} EncCtx;

/* ── Init ────────────────────────────────────────────────────────────── */

static inline void enc_init(EncCtx *ctx, uint32_t scale) {
    ctx->cfg            = enc_config(scale);
    ctx->n_chunks       = 0;
    ctx->total_bytes    = 0;
    ctx->last_home_x    = 0;
    ctx->last_home_y    = 0;
}

/* ── Process one data chunk: find home → hexagon spread → return position ── */

static inline int enc_process(EncCtx *ctx, 
                               const uint8_t *data, uint32_t data_len,
                               uint32_t *home_x, uint32_t *home_y) 
{
    if (!ctx || !data || !data_len) return -1;
    
    /* Step 1: ลากกลับบ้าน — trace path from data */
    enc_find_home(data, data_len, ctx->cfg.field_dim, home_x, home_y);
    
    /* Step 2: compute chunk index on field */
    uint32_t cidx = enc_chunk_idx(*home_x, *home_y, 
                                   ctx->cfg.field_dim, ctx->cfg.scale);
    
    ctx->last_home_x = *home_x;
    ctx->last_home_y = *home_y;
    ctx->n_chunks++;
    ctx->total_bytes += data_len;
    
    return (int)cidx;
}

/* ── Pack hexagon spread into fixed-width chunk ──────────────────────────
 * Writes to 'chunk_out' which must be ctx->cfg.chunk_size bytes.
 * Layout:
 *   [0..47]    = hexagon center (48 bytes = 1 Metatron block)
 *   [48..335]  = 6 hex neighbors (6 × 48 = 288 bytes)
 *   [336..]   = residuals / drain info (remaining)
 *   Must fill exactly ctx->cfg.chunk_size bytes.
 * ─────────────────────────────────────────────────────────────────────────── */

static inline int enc_pack_chunk(EncCtx *ctx,
                                  const uint8_t *data, uint32_t data_len,
                                  uint32_t home_x, uint32_t home_y,
                                  uint8_t *chunk_out)
{
    if (!ctx || !chunk_out) return -1;
    
    uint32_t cs = ctx->cfg.chunk_size;
    memset(chunk_out, 0, cs);
    
    /* Hexagon cells on field */
    uint32_t cells[7][2];
    int n_cells = enc_hexagon_spread(home_x, home_y, ctx->cfg.field_dim, cells, 7);
    if (n_cells < 1) return -1;
    
    /* Pack hexagon data into chunk:
     *   cells[0] = home → offset 0 (48 bytes)
     *   cells[1..6] = neighbors → offset 48 + d*48 (6 × 48 bytes) 
     *   Remaining data beyond 7×48 = packing fills chunk */
    
    /* Write home cell data (47 bytes max from input, pad with zeros) */
    uint32_t copy = data_len < 47 ? data_len : 47;
    memcpy(chunk_out, data, copy);
    
    /* Write data into neighbor slots (remaining data from input, 48 each) */
    uint32_t offset = ENC_BLOCK;
    uint32_t remain = data_len > 48 ? data_len - 48 : 0;
    for (int d = 0; d < 6 && remain > 0; d++) {
        uint32_t nb = remain < ENC_BLOCK ? remain : ENC_BLOCK;
        memcpy(chunk_out + offset, data + 48 + d * ENC_BLOCK, nb);
        offset += ENC_BLOCK;
        remain -= (nb < remain ? nb : remain);
    }
    
    /* Fill rest of chunk with hex cell indices as address header */
    for (int c = 0; c < n_cells && offset + 8 <= cs; c++) {
        chunk_out[offset]     = (uint8_t)(cells[c][0] & 0xFF);
        chunk_out[offset + 1] = (uint8_t)((cells[c][0] >> 8) & 0xFF);
        chunk_out[offset + 2] = (uint8_t)(cells[c][1] & 0xFF);
        chunk_out[offset + 3] = (uint8_t)((cells[c][1] >> 8) & 0xFF);
        offset += 4;
    }
    
    return (int)cs;  /* returns packed size (always = chunk_size) */
}

#ifdef __cplusplus
}
#endif

#endif /* GLS_ENCLOSURE_H */

#ifndef GEO_METATRON_RESHAPE_H
#define GEO_METATRON_RESHAPE_H

#include <stdint.h>
#include <string.h>

/*
 * geo_metatron_reshape.h — Hilbert × Peano Dual-Line บน 3⁴ Grid
 * ═══════════════════════════════════════════════════════════════════
 *
 * World A (Hilbert, binary, north pole):
 *   8×8 = 64 cells, stride-37, pole=0
 *   outer 28 cells = shadow border (FGLS_SHADOW_COUNT)
 *
 * World B (Peano, ternary, south pole):
 *   3⁴ = 81 cells on 9×9, crop to 6×6 = 36 active
 *   remaining 45 = padding reserve
 *
 * Bridge = cpair (diameter line):
 *   cpair(enc) = (enc + 720) % 1440
 *   World A enc ↔ World B enc via pole flip
 *
 * Key invariants:
 *   36 + 28 = 64 = DiamondBlock ✓
 *   81 × 2 = 162 = icosphere L2 nodes ✓
 *   162 - 128 = 34 = delta term ✓
 *
 * Sacred constants (from geo_tring_walk.h):
 *   TRING_WALK_CYCLE  = 1440  (pentagon sync)
 *   TRING_WALK_STRIDE = 37    (prime, coprime to 1440)
 *   META_HALF         = 720   (chiral offset = 6×120)
 *   FGLS_SHADOW_COUNT = 28    (outer boundary = 4×7)
 */

#define PEANO_OUTSIDE       0xFF
#define VERTEX              0x01
#define EDGE                0x02
#define CORE                0x04
#define SHADOW              0x08

#define WORLD_A_NORTH       0
#define WORLD_B_SOUTH       1

#define TRING_CYCLE         1440u
#define TRING_STRIDE        37u
#define SHADOW_COUNT        28u
#define PEANO_GRID          81u
#define HILBERT_GRID        64u
#define CROP_ACTIVE         36u

typedef struct {
    uint8_t row;
    uint8_t col;
} PeanoCoord;

/*
 * DualCell — unified World A/B address
 *
 *  ico_idx    : 0..161 icosphere address (pole×81 + peano_idx)
 *  pole       : 0=World A (north / Hilbert), 1=World B (south / Peano)
 *  peano_idx  : 0..80 ternary position in 3⁴ grid
 *  hilbert_idx: 0..63 binary position on 8×8 (stride-37)
 *  cell_role  : VERTEX | EDGE | CORE | SHADOW
 *  cell_id    : 0..35 if in_crop, PEANO_OUTSIDE if not
 *  in_crop    : 1 = active 6×6 zone, 0 = outside
 */
typedef struct {
    uint8_t  ico_idx;
    uint8_t  pole;
    uint8_t  peano_idx;
    uint8_t  hilbert_idx;
    uint8_t  cell_role;
    uint8_t  cell_id;
    uint8_t  in_crop;
} DualCell;

/* ── Peano L1 LUT (9 entries, S-shape) ────────────────────────── */
static const PeanoCoord peano_l1_lut[9] = {
    {0,0}, {0,1}, {0,2},
    {1,2}, {1,1}, {1,0},
    {2,0}, {2,1}, {2,2}
};

/* ── peano_l2(idx) → (row, col) บน 9×9, O(1) ─────────────────── */
static inline PeanoCoord peano_l2(uint8_t idx) {
    PeanoCoord coord;
    uint8_t p_hi = idx / 9;
    uint8_t p_lo = idx % 9;
    PeanoCoord c1 = peano_l1_lut[p_hi];
    PeanoCoord c0 = peano_l1_lut[p_lo];
    coord.row = c1.row * 3 + c0.row;
    if (c1.row & 1u)
        coord.col = c1.col * 3 + (uint8_t)(2u - c0.col);
    else
        coord.col = c1.col * 3 + c0.col;
    return coord;
}

/* ── peano_crop(row, col) → role, *cell_id = 0..35 or PEANO_OUTSIDE ─── */
static inline uint8_t peano_crop(uint8_t row, uint8_t col, uint8_t *cell_id) {
    if (row < 1 || row > 6 || col < 1 || col > 6) {
        *cell_id = PEANO_OUTSIDE;
        return SHADOW;
    }
    uint8_t r = row - 1;
    uint8_t c = col - 1;
    *cell_id = r * 6 + c;

    /* 4 corners */
    if ((r == 0 || r == 5) && (c == 0 || c == 5))
        return VERTEX;

    /* mid-edge boundary */
    if ((r == 0 || r == 5) && (c == 2 || c == 3)) return EDGE;
    if ((c == 0 || c == 5) && (r == 2 || r == 3)) return EDGE;

    /* interior 4×4 = core */
    if (r >= 1 && r <= 4 && c >= 1 && c <= 4)
        return CORE;

    return EDGE;
}

/* ── ico_enc(pole, peano_idx) → 0..161 icosphere address ──────── */
static inline uint8_t ico_enc(uint8_t pole, uint8_t peano_idx) {
    return pole * PEANO_GRID + peano_idx;
}

/* ── ico_decompose(ico_idx) → {pole, peano_idx} ────────────────── */
static inline void ico_decompose(uint8_t ico_idx, uint8_t *pole, uint8_t *peano_idx) {
    *pole      = ico_idx / PEANO_GRID;
    *peano_idx = ico_idx % PEANO_GRID;
}

/* ── ico_meta_cpair(enc) = (enc + 720) % 1440 — diameter line ─── */
/* Note: named ico_* prefix to avoid clash with old geo_metatron_route.h's
   meta_cpair which uses META_HALF=360 on 720-cycle. This version is the
   updated 720-on-1440-cycle (active_updates convention). */
static inline uint16_t ico_meta_cpair(uint16_t enc) {
    return (uint16_t)((enc + 720u) % 1440u);
}

/* ── ico_cpair(ico_idx) — north↔south flip within icosphere ───── */
static inline uint8_t ico_cpair(uint8_t ico_idx) {
    /* ico_idx = pole*81 + peano_idx
     * flip pole: 0→1, 1→0
     * = (ico_idx + 81) % 162 */
    return (uint8_t)((ico_idx + PEANO_GRID) % (PEANO_GRID * 2));
}

/* ── ico_to_hilbert(ico_idx) → 0..63 or 0xFF if south pole ───── */
static inline uint8_t ico_to_hilbert(uint8_t ico_idx) {
    uint8_t pole, peano_idx;
    ico_decompose(ico_idx, &pole, &peano_idx);
    if (pole == WORLD_B_SOUTH)
        return 0xFF;  /* south pole has no direct Hilbert mapping */
    /* World A: Hilbert stride-37 on 8×8 */
    return (uint8_t)((peano_idx * TRING_STRIDE) % HILBERT_GRID);
}

/* ── geo_metatron_reshape(pole, peano_idx, hilbert_raw) → DualCell ── */
static inline DualCell geo_metatron_reshape(uint8_t pole, uint8_t peano_idx, uint8_t hilbert_raw) {
    DualCell cell;
    cell.pole       = pole;
    cell.peano_idx  = peano_idx;
    cell.ico_idx    = ico_enc(pole, peano_idx);

    PeanoCoord coord = peano_l2(peano_idx);
    uint8_t cid = PEANO_OUTSIDE;
    cell.cell_role = peano_crop(coord.row, coord.col, &cid);
    cell.cell_id   = cid;
    cell.in_crop   = (cid != PEANO_OUTSIDE) ? 1u : 0u;
    cell.hilbert_idx = (uint8_t)((hilbert_raw * TRING_STRIDE) % HILBERT_GRID);

    return cell;
}

/* ── geo_metatron_reshape_verify() — self-test, returns 0 on pass ── */
static inline int geo_metatron_reshape_verify(void) {
    /* [1] peano_l2: all 81 positions valid 0..8 */
    for (uint16_t i = 0; i < PEANO_GRID; i++) {
        PeanoCoord c = peano_l2((uint8_t)i);
        if (c.row > 8 || c.col > 8) return -1;
    }

    /* [2] peano_crop: 36 active + 45 outside */
    uint32_t n_active = 0, n_outside = 0, n_vertex = 0, n_edge = 0, n_core = 0;
    for (uint8_t r = 0; r <= 8; r++) {
        for (uint8_t c = 0; c <= 8; c++) {
            uint8_t cid;
            uint8_t role = peano_crop(r, c, &cid);
            if (role == SHADOW) n_outside++;
            else                n_active++;
            if (role == VERTEX) n_vertex++;
            if (role == EDGE)   n_edge++;
            if (role == CORE)   n_core++;
        }
    }
    if (n_active  != CROP_ACTIVE)   return -10;
    if (n_outside != PEANO_GRID - CROP_ACTIVE) return -11;
    if (n_vertex  != 4u)            return -12;
    if (n_core    != 16u)           return -13;
    if (n_core + n_vertex + n_edge != CROP_ACTIVE) return -14;

    /* [3] ico_enc / ico_decompose roundtrip */
    for (uint16_t p = 0; p < PEANO_GRID; p++) {
        for (uint8_t pole = 0; pole <= 1; pole++) {
            uint8_t enc = ico_enc(pole, (uint8_t)p);
            if (enc != pole * PEANO_GRID + p) return -20;
            uint8_t pole2, pidx2;
            ico_decompose(enc, &pole2, &pidx2);
            if (pole2 != pole || pidx2 != p) return -21;
        }
    }

    /* [4] ico_cpair: north↔south flip */
    for (uint16_t i = 0; i < PEANO_GRID * 2; i++) {
        uint8_t cp = ico_cpair((uint8_t)i);
        if (ico_cpair(cp) != (uint8_t)i) return -30;  /* self-inverse */
        if (cp == (uint8_t)i)            return -31;  /* no fixed point */
    }

    /* [5] ico_meta_cpair self-inverse on 1440 cycle */
    for (uint16_t e = 0; e < 1440u; e++) {
        if (ico_meta_cpair(ico_meta_cpair(e)) != e) return -35;
    }

    /* [6] 36 + 28 = 64 DiamondBlock invariant */
    if (CROP_ACTIVE + SHADOW_COUNT != HILBERT_GRID) return -40;

    return 0;
}

#endif /* GEO_METATRON_RESHAPE_H */

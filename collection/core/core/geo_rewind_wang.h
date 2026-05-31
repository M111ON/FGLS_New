#pragma once
#ifndef GEO_REWIND_WANG_H
#define GEO_REWIND_WANG_H

/*
 * geo_rewind_wang.h — Wang Parity Layer for geo_rewind
 *
 * 972 slots → 108 rows × 9 slots
 * Each row has XOR parity + Wang edge colors (top/bottom)
 * Edge match between adjacent rows = valid transition
 *
 * Role in recovery pipeline:
 *   BEFORE L1 XOR → check wang_row_valid()
 *   invalid edge   → skip L1, go L3 RS directly
 *   valid + parity mismatch → flag corrupt slot in row
 */

#include "geo_rewind.h"

#define WANG_ROW_SIZE    9u
#define WANG_ROW_COUNT   108u   /* 972 / 9 */

/* Edge colors: maps to lc_twin_gate polarity */
typedef enum {
    WANG_EDGE_GROUND    = 0,
    WANG_EDGE_ROUTE     = 1,
    WANG_EDGE_WARP      = 2,
    WANG_EDGE_COLLISION = 3,
} WangEdgeColor;

typedef struct {
    uint32_t xor_enc;        /* XOR of all enc in row — parity check */
    uint8_t  edge_top;       /* WangEdgeColor — must match prev row edge_bot */
    uint8_t  edge_bot;       /* WangEdgeColor — must match next row edge_top */
    uint16_t corrupt_mask;   /* bitmask: which slot(s) in row are suspect */
    bool     valid;          /* row has been computed */
} WangParityRow;

typedef struct {
    WangParityRow rows[WANG_ROW_COUNT];
    uint32_t      dirty_mask[4]; /* 108 bits — row needs recompute (4×uint32) */
} RewindWangLayer;

/* ── helpers ── */

static inline uint16_t wang_row_of(uint16_t slot_idx) {
    return slot_idx / WANG_ROW_SIZE;
}

static inline uint16_t wang_row_start(uint16_t row) {
    return row * WANG_ROW_SIZE;
}

static inline void _wang_set_dirty(RewindWangLayer *wl, uint16_t row) {
    wl->dirty_mask[row >> 5] |= (1u << (row & 31u));
}

static inline bool _wang_is_dirty(const RewindWangLayer *wl, uint16_t row) {
    return (wl->dirty_mask[row >> 5] >> (row & 31u)) & 1u;
}

static inline void _wang_clear_dirty(RewindWangLayer *wl, uint16_t row) {
    wl->dirty_mask[row >> 5] &= ~(1u << (row & 31u));
}

/* ── derive edge color from enc (maps to spoke polarity) ── */
static inline uint8_t _wang_enc_to_edge(uint32_t enc) {
    /* use enc mod 6 → spoke, then map to 4 edge colors */
    static const uint8_t spoke_to_edge[6] = {
        WANG_EDGE_GROUND, WANG_EDGE_ROUTE,
        WANG_EDGE_WARP,   WANG_EDGE_COLLISION,
        WANG_EDGE_ROUTE,  WANG_EDGE_GROUND,
    };
    return spoke_to_edge[enc % 6u];
}

/* ── compute parity row from RewindBuffer ── */
static inline void wang_compute_row(RewindWangLayer *wl,
                                    const RewindBuffer *rb,
                                    uint16_t row)
{
    uint16_t base = wang_row_start(row);
    uint32_t xor_acc = 0u;
    uint16_t corrupt = 0u;
    uint32_t first_valid_enc = 0u;
    uint32_t last_valid_enc  = 0u;

    for (uint8_t i = 0u; i < WANG_ROW_SIZE; i++) {
        uint16_t idx = (uint16_t)((base + i) % REWIND_SLOTS);
        if (rb->slots[idx].valid) {
            xor_acc ^= rb->slots[idx].enc;
            if (!first_valid_enc) first_valid_enc = rb->slots[idx].enc;
            last_valid_enc = rb->slots[idx].enc;
        } else {
            corrupt |= (1u << i);
        }
    }

    WangParityRow *r = &wl->rows[row];
    r->xor_enc    = xor_acc;
    r->edge_top   = _wang_enc_to_edge(first_valid_enc);
    r->edge_bot   = _wang_enc_to_edge(last_valid_enc);
    r->corrupt_mask = corrupt;
    r->valid      = true;
    _wang_clear_dirty(wl, row);
}

/* ── init: compute all rows ── */
static inline void wang_init(RewindWangLayer *wl, const RewindBuffer *rb) {
    memset(wl, 0, sizeof(*wl));
    for (uint16_t r = 0u; r < WANG_ROW_COUNT; r++)
        wang_compute_row(wl, rb, r);
}

/* ── call after rewind_store() to keep layer fresh ── */
static inline void wang_notify_store(RewindWangLayer *wl, uint16_t slot_idx) {
    _wang_set_dirty(wl, wang_row_of(slot_idx));
}

static inline void wang_flush_dirty(RewindWangLayer *wl, const RewindBuffer *rb) {
    for (uint16_t r = 0u; r < WANG_ROW_COUNT; r++)
        if (_wang_is_dirty(wl, r))
            wang_compute_row(wl, rb, r);
}

/* ── edge validation ── */

/* core ^ inv == 0xFF analog: adjacent rows must have matching edge */
static inline bool wang_edge_valid(const RewindWangLayer *wl, uint16_t row) {
    if (!wl->rows[row].valid) return false;
    if (row == 0u) return true;
    /* bottom edge of prev must match top edge of current */
    return wl->rows[row - 1u].edge_bot == wl->rows[row].edge_top;
}

/* ── recovery gate: call BEFORE L1 XOR ── */
typedef enum {
    WANG_RECOVER_L1_OK,   /* edge valid, parity ok → proceed L1 XOR */
    WANG_RECOVER_SKIP_L1, /* edge mismatch → go L3 RS directly */
    WANG_RECOVER_PARTIAL, /* edge ok but corrupt_mask set → targeted recover */
} WangRecoverDecision;

static inline WangRecoverDecision wang_recover_gate(RewindWangLayer *wl,
                                                     const RewindBuffer *rb,
                                                     uint16_t row)
{
    if (_wang_is_dirty(wl, row))
        wang_compute_row(wl, rb, row);

    if (!wang_edge_valid(wl, row))
        return WANG_RECOVER_SKIP_L1;   /* edge broken → L3 only */

    if (wl->rows[row].corrupt_mask)
        return WANG_RECOVER_PARTIAL;   /* some slots missing → targeted */

    return WANG_RECOVER_L1_OK;
}

/* ── reconstruct missing slot via XOR (single missing only) ── */
static inline bool wang_reconstruct_slot(const RewindWangLayer *wl,
                                          const RewindBuffer *rb,
                                          uint16_t row,
                                          uint32_t *out_enc)
{
    /* only valid if exactly 1 slot missing */
    if (__builtin_popcount(wl->rows[row].corrupt_mask) != 1) return false;

    uint16_t base = wang_row_start(row);
    uint32_t xor_known = 0u;

    for (uint8_t i = 0u; i < WANG_ROW_SIZE; i++) {
        uint16_t idx = (uint16_t)((base + i) % REWIND_SLOTS);
        if (rb->slots[idx].valid)
            xor_known ^= rb->slots[idx].enc;
    }

    /* missing enc = stored parity ^ known XOR */
    *out_enc = wl->rows[row].xor_enc ^ xor_known;
    return true;
}

#endif /* GEO_REWIND_WANG_H */

#pragma once
#include "geo_rewind.h"
#include <stdbool.h>

#define WANG_ROW_SIZE    9u
#define WANG_ROW_COUNT   108u

typedef enum {
    WANG_EDGE_GROUND    = 0,
    WANG_EDGE_ROUTE     = 1,
    WANG_EDGE_WARP      = 2,
    WANG_EDGE_COLLISION = 3,
} WangEdgeColor;

typedef struct {
    uint32_t xor_enc;
    uint8_t  edge_top;
    uint8_t  edge_bot;
    uint16_t corrupt_mask;
    bool     valid;
} WangParityRow;

typedef struct {
    WangParityRow rows[WANG_ROW_COUNT];
    uint32_t      dirty_mask[4];
} RewindWangLayer;

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

static inline uint8_t _wang_enc_to_edge(uint32_t enc) {
    static const uint8_t spoke_to_edge[6] = {
        WANG_EDGE_GROUND, WANG_EDGE_ROUTE,
        WANG_EDGE_WARP,   WANG_EDGE_COLLISION,
        WANG_EDGE_ROUTE,  WANG_EDGE_GROUND,
    };
    return spoke_to_edge[enc % 6u];
}

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

static inline void wang_init(RewindWangLayer *wl, const RewindBuffer *rb) {
    memset(wl, 0, sizeof(*wl));
    for (uint16_t r = 0u; r < WANG_ROW_COUNT; r++)
        wang_compute_row(wl, rb, r);
}

static inline void wang_notify_store(RewindWangLayer *wl, uint16_t slot_idx) {
    _wang_set_dirty(wl, wang_row_of(slot_idx));
}

static inline void wang_flush_dirty(RewindWangLayer *wl, const RewindBuffer *rb) {
    for (uint16_t r = 0u; r < WANG_ROW_COUNT; r++)
        if (_wang_is_dirty(wl, r))
            wang_compute_row(wl, rb, r);
}

static inline bool wang_edge_valid(const RewindWangLayer *wl, uint16_t row) {
    if (!wl->rows[row].valid) return false;
    if (row == 0u) return true;
    return wl->rows[row - 1u].edge_bot == wl->rows[row].edge_top;
}

typedef enum {
    WANG_RECOVER_L1_OK,
    WANG_RECOVER_SKIP_L1,
    WANG_RECOVER_PARTIAL,
} WangRecoverDecision;

static inline WangRecoverDecision wang_recover_gate(RewindWangLayer *wl,
                                                     const RewindBuffer *rb,
                                                     uint16_t row)
{
    if (_wang_is_dirty(wl, row))
        wang_compute_row(wl, rb, row);
    if (!wang_edge_valid(wl, row))
        return WANG_RECOVER_SKIP_L1;
    if (wl->rows[row].corrupt_mask)
        return WANG_RECOVER_PARTIAL;
    return WANG_RECOVER_L1_OK;
}

static inline bool wang_reconstruct_slot(const RewindWangLayer *wl,
                                          const RewindBuffer *rb,
                                          uint16_t row,
                                          uint32_t *out_enc)
{
    if (__builtin_popcount(wl->rows[row].corrupt_mask) != 1) return false;
    uint16_t base = wang_row_start(row);
    uint32_t xor_known = 0u;
    for (uint8_t i = 0u; i < WANG_ROW_SIZE; i++) {
        uint16_t idx = (uint16_t)((base + i) % REWIND_SLOTS);
        if (rb->slots[idx].valid)
            xor_known ^= rb->slots[idx].enc;
    }
    *out_enc = wl->rows[row].xor_enc ^ xor_known;
    return true;
}

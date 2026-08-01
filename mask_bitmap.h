// mask_bitmap.h
// Sparse weight storage per mask using bitmap + popcount compact array.
// Design goal: O(1)-ish lookup, minimal metadata, cache-friendly.

#ifndef MASK_BITMAP_H
#define MASK_BITMAP_H

#include <stdint.h>
#include <string.h>

// ---------------- Config constants (Lego block, tune here only) ----------------
#define MB_NUM_MASKS        10          // 10 masks (0-9)
#define MB_SLOTS_PER_MASK   1000        // 10x10x10 cube capacity per mask's addressable field
#define MB_BITMAP_WORDS     ((MB_SLOTS_PER_MASK + 63) / 64)   // 64-bit words needed for bitmap
#define MB_MAX_ACTIVE       MB_SLOTS_PER_MASK                 // worst case all active

typedef float mb_value_t;   // change to double / int32_t / etc. as needed

// ---------------- Per-mask structure ----------------
typedef struct {
    uint64_t   bitmap[MB_BITMAP_WORDS];   // 1 bit per slot: active/inactive
    mb_value_t values[MB_MAX_ACTIVE];     // compact packed values, no gaps
    uint32_t   active_count;              // how many slots currently active
} mb_mask_t;

// ---------------- Full field (10 masks) ----------------
typedef struct {
    mb_mask_t masks[MB_NUM_MASKS];
} mb_field_t;

// ---------------- Core ops ----------------

static inline void mb_init(mb_field_t *field) {
    memset(field, 0, sizeof(mb_field_t));
}

static inline int mb_is_active(const mb_mask_t *m, uint32_t slot) {
    return (m->bitmap[slot >> 6] >> (slot & 63)) & 1ULL;
}

// popcount of bits before 'slot' within this mask -> compact array index
static inline uint32_t mb_rank(const mb_mask_t *m, uint32_t slot) {
    uint32_t word_idx = slot >> 6;
    uint32_t bit_idx  = slot & 63;
    uint32_t rank = 0;

    for (uint32_t w = 0; w < word_idx; w++) {
        rank += __builtin_popcountll(m->bitmap[w]);
    }
    if (bit_idx) {
        uint64_t mask_bits = (1ULL << bit_idx) - 1ULL;
        rank += __builtin_popcountll(m->bitmap[word_idx] & mask_bits);
    }
    return rank;
}

// Write a value into slot (sets bit if not already active, shifts compact array)
static inline int mb_set(mb_mask_t *m, uint32_t slot, mb_value_t value) {
    if (slot >= MB_SLOTS_PER_MASK) return -1;

    if (mb_is_active(m, slot)) {
        // already active: just overwrite value in place
        uint32_t idx = mb_rank(m, slot);
        m->values[idx] = value;
        return 0;
    }

    if (m->active_count >= MB_MAX_ACTIVE) return -2; // full

    uint32_t idx = mb_rank(m, slot);

    // shift values right to make room at idx (compact array insert)
    for (uint32_t i = m->active_count; i > idx; i--) {
        m->values[i] = m->values[i - 1];
    }
    m->values[idx] = value;

    m->bitmap[slot >> 6] |= (1ULL << (slot & 63));
    m->active_count++;
    return 0;
}

// Read value at slot. Returns 0 on success, -1 if slot inactive.
static inline int mb_get(const mb_mask_t *m, uint32_t slot, mb_value_t *out) {
    if (slot >= MB_SLOTS_PER_MASK) return -1;
    if (!mb_is_active(m, slot)) return -1;

    uint32_t idx = mb_rank(m, slot);
    *out = m->values[idx];
    return 0;
}

// Remove value at slot (compact array delete + bitmap clear)
static inline int mb_clear(mb_mask_t *m, uint32_t slot) {
    if (slot >= MB_SLOTS_PER_MASK) return -1;
    if (!mb_is_active(m, slot)) return -1;

    uint32_t idx = mb_rank(m, slot);

    for (uint32_t i = idx; i < m->active_count - 1; i++) {
        m->values[i] = m->values[i + 1];
    }
    m->active_count--;
    m->bitmap[slot >> 6] &= ~(1ULL << (slot & 63));
    return 0;
}

// ---------------- Stats / size reporting ----------------
static inline size_t mb_bitmap_bytes_per_mask(void) {
    return sizeof(uint64_t) * MB_BITMAP_WORDS;
}

static inline size_t mb_payload_bytes(const mb_mask_t *m) {
    return sizeof(mb_value_t) * m->active_count;
}

static inline size_t mb_total_bytes(const mb_mask_t *m) {
    return mb_bitmap_bytes_per_mask() + mb_payload_bytes(m);
}

#endif // MASK_BITMAP_H

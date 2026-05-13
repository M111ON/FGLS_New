/*
 * frustum_slot64.h — DiamondBlock 64B Storage
 * ════════════════════════════════════════════════════════════════
 *
 * 54 slots × 64B = 3456B = GEO_FULL_N  (self-similar seam ✓)
 * Each slot = one DiamondBlock = 64B = 2⁶ bytes
 *
 * Slot layout (64B exact):
 *   core[4]        16B  — merkle roots per level (0..3)
 *   reserved_mask   2B  — coset silence bitmap (9 bits, one per coset)
 *   write_count     2B  — writes into this slot (overflow wraps)
 *   slope_lo        4B  — last slope fingerprint low 32 bits
 *   _pad           40B  — reserved, zero
 *
 * Block index:
 *   block_index  = addr / DIAMOND_BLOCK   (0..53 for Lv1)
 *   block_offset = addr % DIAMOND_BLOCK   (0..63)
 *   54 = GEAR_MESH = 2×3³ — aligns with both trit and face decomposition
 *
 * Write rule:
 *   slot = store[trit.coset * 6 + trit.face]  ← coset×face addressing
 *   if trit_coset_silent(slot.reserved_mask, trit.coset) → drop silently
 *   else slot.core[trit.level] = merkle_root  (latest wins)
 *
 * No malloc. No float. No heap.
 * Depends: frustum_trit.h
 * ════════════════════════════════════════════════════════════════
 */

#ifndef FRUSTUM_SLOT64_H
#define FRUSTUM_SLOT64_H

#include <stdint.h>
#include <string.h>
#include "frustum_trit.h"

/* ── constants ─────────────────────────────────────────────── */
#define DIAMOND_BLOCK    64u   /* 2⁶ bytes per slot              */
#define GEAR_MESH        54u   /* 2×3³ slots, tiles Lv1 exactly  */
#define FRUSTUM_DATA_SZ  3456u /* GEAR_MESH × DIAMOND_BLOCK      */

typedef char _fslot_sz_assert[(GEAR_MESH * DIAMOND_BLOCK == FRUSTUM_DATA_SZ) ? 1:-1];

/* ── DiamondBlock slot (64B exact) ────────────────────────── */
typedef struct {
    uint32_t  core[LEVEL_COUNT];  /* 16B: merkle root per level 0..3  */
    uint16_t  reserved_mask;      /*  2B: coset silence bitmap (9 bits)*/
    uint16_t  write_count;        /*  2B: writes into this slot        */
    uint32_t  slope_lo;           /*  4B: last slope fingerprint[31:0] */
    uint8_t   _pad[40];           /* 40B: reserved, zero               */
} FrustumSlot64;

typedef char _fslot64_sz[(sizeof(FrustumSlot64) == DIAMOND_BLOCK) ? 1:-1];

/* ── slot store — 54 DiamondBlocks ────────────────────────── */
typedef struct {
    FrustumSlot64 slots[GEAR_MESH];   /* 54 × 64B = 3456B */
    uint32_t      total_writes;       /* lifetime write counter */
    uint32_t      total_silenced;     /* dropped by reserved_mask */
} FrustumStore;

/* ── init ───────────────────────────────────────────────────── */
static inline void frustum_store_init(FrustumStore *fs)
{
    memset(fs, 0, sizeof(*fs));
}

/* ── slot index from TritAddr ──────────────────────────────── */
/*
 * Addressing: coset(0..8) × face(0..5) → 0..53
 * Bijective within GEAR_MESH=54 ✓  (9×6=54)
 */
static inline uint8_t frustum_slot_idx(const TritAddr *t)
{
    return (uint8_t)(t->coset * FACE_COUNT + t->face);
}

/* ── write TritAddr into store ─────────────────────────────── */
/*
 * merkle_root: caller computes and passes in — frustum doesn't hash.
 * Returns 1=written, 0=silenced by reserved_mask.
 */
static inline int frustum_write(FrustumStore    *fs,
                                 const TritAddr  *t,
                                 uint32_t         merkle_root)
{
    uint8_t idx = frustum_slot_idx(t);
    /* idx guaranteed 0..53 — no bounds check needed (9×6=54) */

    FrustumSlot64 *slot = &fs->slots[idx];

    /* coset silence check */
    if (trit_coset_silent(slot->reserved_mask, t->coset)) {
        fs->total_silenced++;
        return 0;
    }

    /* write: latest merkle_root wins for this level */
    slot->core[t->level]  = merkle_root;
    slot->slope_lo         = (uint32_t)(t->slope & 0xFFFFFFFFu);
    slot->write_count      = (uint16_t)(slot->write_count + 1u);
    fs->total_writes++;
    return 1;
}

/* ── silence a coset (set reserved_mask bit) ───────────────── */
static inline void frustum_silence_coset(FrustumStore *fs,
                                          uint8_t       coset,
                                          uint8_t       face)
{
    uint8_t idx = (uint8_t)(coset * FACE_COUNT + face);
    fs->slots[idx].reserved_mask |= (uint16_t)(1u << coset);
}

/* ── read core[level] from TritAddr ────────────────────────── */
static inline uint32_t frustum_read(const FrustumStore *fs,
                                     const TritAddr     *t)
{
    return fs->slots[frustum_slot_idx(t)].core[t->level];
}

/* ── stats ──────────────────────────────────────────────────── */
typedef struct {
    uint32_t total_writes;
    uint32_t total_silenced;
    uint32_t occupied_slots;   /* slots with at least one non-zero core */
    uint32_t silent_slots;     /* slots where all cosets silenced */
} FrustumStats;

static inline FrustumStats frustum_stats(const FrustumStore *fs)
{
    FrustumStats s;
    s.total_writes   = fs->total_writes;
    s.total_silenced = fs->total_silenced;
    s.occupied_slots = 0u;
    s.silent_slots   = 0u;
    for (uint8_t i = 0u; i < GEAR_MESH; i++) {
        const FrustumSlot64 *sl = &fs->slots[i];
        int has_data = 0;
        for (uint8_t l = 0u; l < LEVEL_COUNT; l++)
            if (sl->core[l]) { has_data = 1; break; }
        if (has_data)       s.occupied_slots++;
        if (sl->reserved_mask == 0x1FFu) s.silent_slots++;  /* all 9 bits set */
    }
    return s;
}

#endif /* FRUSTUM_SLOT64_H */

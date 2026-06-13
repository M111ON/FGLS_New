/*
 * shell_weight_map.h — POGLS Shell Weight Mapper (S3)
 * ════════════════════════════════════════════════════
 *
 * ZoneCard → Chord → OnionStack → uint64_t address
 *
 * ZoneCard is the passport. 12 bytes → full weight location.
 * No weight data stored here. Geometry IS the address.
 *
 * Mapping rules (deterministic, O(1)):
 *   entropy   → shell_id   (low=inner/Q4, high=outer/F16)
 *   card_type → chord_id   (SPARSE→HUB, BATCH→ORBITAL, LZ→CHIRAL)
 *   pattern   → seed       (lower 16 bits of pattern hash)
 *   stability → key_offset (stable=0, unstable=delta shift)
 *   layer_idx → capo fine  (GGUF layer number → anchor offset)
 *
 * Depends: shell_container.h, onion_stack.h, zone_card.h
 * No malloc. No float. No I/O.
 * ════════════════════════════════════════════════════
 */

#ifndef SHELL_WEIGHT_MAP_H
#define SHELL_WEIGHT_MAP_H

#include <stdint.h>
#include "onion_stack.h"
#include "zone_card.h"

/* ── Mapping constants ───────────────────────────────── */

/*
 * entropy → shell_id
 * entropy 0..255 → shell 0..11
 * low entropy  = structured/sparse = inner shell (high compress)
 * high entropy = random/dense      = outer shell (full fidelity)
 */
static inline uint8_t wmap_entropy_to_shell(uint8_t entropy)
{
    /* scale 0..255 → 0..11 */
    return (uint8_t)(((uint32_t)entropy * ONION_N_SHELLS) >> 8);
}

/*
 * card_type → chord_id
 * SPARSE → CHORD_HUB     (collapse to anchor, aggregate)
 * BATCH  → CHORD_ORBITAL (sequential walk, forward)
 * LZ     → CHORD_CHIRAL  (mirror pole, backup/residual)
 */
static inline uint8_t wmap_type_to_chord(uint8_t card_type)
{
    switch (card_type) {
        case CARD_SPARSE: return CHORD_HUB;
        case CARD_BATCH:  return CHORD_ORBITAL;
        case CARD_LZ:     return CHORD_CHIRAL;
        default:          return CHORD_CROSS;  /* unknown → cross-face */
    }
}

/*
 * stability → key_offset (capo)
 * stability 255 = fully stable → offset 0 (main anchor, no shift)
 * stability 0   = fully unstable → max offset (shadow zone)
 * maps to 0..575 (= JUNCTION/12 - 1 = one anchor spacing)
 */
static inline uint32_t wmap_stability_to_capo(uint8_t stability)
{
    /* invert: low stability = large offset */
    uint32_t inv = 255u - (uint32_t)stability;
    /* scale to anchor spacing: 575 = JUNCTION/12 - 1, with rounding */
    return (inv * 577u + 128u) >> 8;
}

/*
 * pattern + layer_idx → seed
 * pattern upper byte = hash fingerprint
 * layer_idx = GGUF layer number (0..N)
 * combine: seed = (pattern_upper << 16) | (layer_idx & 0xFFFF)
 */
static inline uint32_t wmap_to_seed(uint16_t pattern, uint32_t layer_idx)
{
    uint32_t pat_hi = (uint32_t)(pattern >> 8) & 0xFFu;
    return (pat_hi << 16) | (layer_idx & 0xFFFFu);
}

/* ── WeightAddr result ───────────────────────────────── */
typedef struct {
    uint64_t addr;       /* shell address (0..6911) */
    uint8_t  shell_id;   /* which onion layer (0..11) */
    uint8_t  chord_id;   /* which chord mode */
    uint32_t seed;       /* derived seed */
    uint32_t capo;       /* key_offset used */
} WeightAddr;

/* ── shell_weight_addr — ZoneCard → WeightAddr ───────── */
/*
 * Primary function: ZoneCard passport → weight location
 *
 * layer_idx = GGUF tensor layer index (0 = embed, 1..N = transformer layers)
 *
 * Returns WeightAddr with full decode info for verification/reverse.
 * O(1) — pure arithmetic.
 */
static inline WeightAddr shell_weight_addr(const OnionStack *o,
                                            const ZoneCard   *card,
                                            uint32_t          layer_idx)
{
    WeightAddr wa;

    wa.shell_id = wmap_entropy_to_shell(card->entropy);
    wa.chord_id = wmap_type_to_chord(card->card_type);
    wa.seed     = wmap_to_seed(card->pattern, layer_idx);
    wa.capo     = wmap_stability_to_capo(card->stability);

    Chord c = {
        .geometry   = (wa.shell_id & 1u) ? GEO_COMPOUND_OCTA
                                          : GEO_COMPOUND_TETRA,
        .seed       = wa.seed,
        .chord_id   = wa.chord_id,
        .key_offset = wa.capo,
    };

    wa.addr = onion_addr(o, wa.shell_id, &c);
    return wa;
}

/* ── shell_weight_card — addr → ZoneCard (reverse) ──── */
/*
 * Reverse map: given a WeightAddr, reconstruct a ZoneCard stub
 * for verification or cross-session handoff.
 *
 * NOTE: entropy/pattern are approximated from addr — not bit-exact
 * to original card. Use hash_val for exact verification if needed.
 *
 * Use case: session B receives addr → verify it came from same geometry
 */
static inline ZoneCard shell_weight_card(const OnionStack *o,
                                          const WeightAddr  *wa)
{
    (void)o;  /* geometry implicit in addr ranges */

    ZoneCard card;

    /* reverse shell_id → entropy (midpoint of shell's entropy band) */
    card.entropy = (uint8_t)(((uint32_t)wa->shell_id * 256u
                              + 128u) / ONION_N_SHELLS);

    /* reverse chord_id → card_type */
    switch (wa->chord_id) {
        case CHORD_HUB:     card.card_type = CARD_SPARSE; break;
        case CHORD_ORBITAL: card.card_type = CARD_BATCH;  break;
        case CHORD_CHIRAL:  card.card_type = CARD_LZ;     break;
        default:            card.card_type = CARD_BATCH;  break;
    }

    /* reverse seed → pattern (upper byte only) */
    uint8_t pat_hi = (uint8_t)((wa->seed >> 16) & 0xFFu);
    card.pattern = (uint16_t)(pat_hi << 8);

    /* reverse capo → stability */
    uint32_t inv = (wa->capo * 256u) / 576u;
    if (inv > 255u) inv = 255u;
    card.stability = (uint8_t)(255u - inv);

    /* id from addr (lower 16 bits) */
    card.id = (uint16_t)(wa->addr & 0xFFFFu);

    /* locality = default (needs neighbor context to compute properly) */
    card.locality = 128u;

    /* neighbors = unknown in reverse direction */
    card.neighbor_left  = NO_NEIGHBOR;
    card.neighbor_right = NO_NEIGHBOR;

    return card;
}

/* ── Batch: GGUF layer slice → WeightAddr array ──────── */
/*
 * Map an entire GGUF layer (array of ZoneCards) to addresses.
 * out[] must be pre-allocated by caller (n_cards entries).
 *
 * Use: iterate GGUF tensors → make ZoneCard per slice → call this.
 * O(n_cards) — one O(1) lookup per card.
 */
static inline void shell_weight_map_layer(const OnionStack *o,
                                           const ZoneCard   *cards,
                                           uint32_t          n_cards,
                                           uint32_t          layer_idx,
                                           WeightAddr        *out)
{
    for (uint32_t i = 0u; i < n_cards; i++) {
        out[i] = shell_weight_addr(o, &cards[i], layer_idx);
    }
}

/* ── Verify round-trip ───────────────────────────────── */
/*
 * Check that card → addr → card_stub produces consistent shell/chord.
 * Returns 1 = consistent, 0 = mismatch.
 *
 * Note: not bit-exact (entropy/pattern are approximated in reverse).
 * Checks: shell_id, chord_id match only.
 */
static inline int shell_weight_verify(const OnionStack *o,
                                       const ZoneCard   *card,
                                       uint32_t          layer_idx)
{
    WeightAddr wa   = shell_weight_addr(o, card, layer_idx);
    ZoneCard   stub = shell_weight_card(o, &wa);

    uint8_t shell_fwd = wmap_entropy_to_shell(card->entropy);
    uint8_t shell_rev = wmap_entropy_to_shell(stub.entropy);

    uint8_t chord_fwd = wmap_type_to_chord(card->card_type);
    uint8_t chord_rev = wmap_type_to_chord(stub.card_type);

    return (shell_fwd == shell_rev) && (chord_fwd == chord_rev);
}

#endif /* SHELL_WEIGHT_MAP_H */

/*
 * onion_stack.h — POGLS Onion Shell: hop + multi-shell radial stack (S2)
 * ════════════════════════════════════════════════════════════════════════
 *
 * Extends shell_container.h with:
 *   shell_hop()    — angular move: pentagon → pentagon (within 1 shell)
 *   OnionStack     — 12 shells radial stack + precomputed cross-shell LUT
 *   onion_init()   — init all shells + build LUT once
 *   onion_addr()   — route to any shell O(1)
 *   onion_cross()  — cross-shell radial jump via LUT O(1)
 *   onion_compress() — frustum depth = resolution tier (shell 0=apex/max compress)
 *
 * Mental model:
 *   Angular  = 12 frustum ประกบรอบ → 1 complete dodecahedron shell
 *   Radial   = 12 shell ซ้อน → onion layers (shell 0=inner/apex, shell 11=outer/base)
 *   Compress = apex (shell 0) = highest compression, base (shell 11) = F16 full fidelity
 *
 * Sacred numbers: FROZEN
 *   12   = N_ANCHORS = N_SHELLS = pentagon count
 *   720  = TRING_SLOTS
 *   3456 = GEO_FULL_N
 *   6912 = JUNCTION
 * ════════════════════════════════════════════════════════════════════════
 */

#ifndef ONION_STACK_H
#define ONION_STACK_H

#include <stdint.h>
#include <string.h>
#include "shell_container.h"

/* ── Constants ───────────────────────────────────────── */
#define ONION_N_SHELLS   12u    /* = N_ANCHORS, FROZEN */

/*
 * Dodecahedron pentagon adjacency — FROZEN
 *
 * Each pentagon has exactly 5 neighbors (not 11).
 * Topology of regular dodecahedron (face adjacency):
 *
 *   0: top cap
 *   1-5: upper ring
 *   6-10: lower ring
 *   11: bottom cap
 *
 * PENT_ADJ[i][j] = j-th neighbor of pentagon i (5 neighbors each)
 */
static const uint8_t PENT_ADJ[12][5] = {
    /* 0  top    */ { 1, 2, 3, 4, 5 },
    /* 1  upper  */ { 0, 2, 5, 6, 7 },
    /* 2  upper  */ { 0, 1, 3, 7, 8 },
    /* 3  upper  */ { 0, 2, 4, 8, 9 },
    /* 4  upper  */ { 0, 3, 5, 9,10 },
    /* 5  upper  */ { 0, 1, 4,10, 6 },
    /* 6  lower  */ { 1, 5,10,11, 7 },
    /* 7  lower  */ { 1, 2, 6,11, 8 },
    /* 8  lower  */ { 2, 3, 7,11, 9 },
    /* 9  lower  */ { 3, 4, 8,11,10 },
    /*10  lower  */ { 4, 5, 9,11, 6 },
    /*11  bottom */ { 6, 7, 8, 9,10 },
};

/* ── Hop result ──────────────────────────────────────── */
typedef struct {
    uint32_t addr;         /* junction address at boundary between two frustum */
    uint8_t  from_anchor;  /* source pentagon id */
    uint8_t  to_anchor;    /* destination pentagon id */
    uint8_t  valid;        /* 1=neighbors are adjacent, 0=not adjacent */
} HopResult;

/* ── shell_hop — angular move within 1 shell ─────────── */
/*
 * Move from one pentagon frustum to an adjacent one.
 * Only valid if from_anchor and to_anchor are true dodecahedron neighbors.
 *
 * Junction address formula:
 *   midpoint of two anchors in JUNCTION space, perturbed by shell seed
 *   = (anchor[from] + anchor[to] + seed%37) % JUNCTION
 *
 * Returns HopResult.valid=1 if adjacent, 0 if not neighbors.
 * O(1) — linear scan of 5 neighbors only.
 */
static inline HopResult shell_hop(const ShellContainer *s,
                                   uint8_t from_anchor,
                                   uint8_t to_anchor)
{
    HopResult r;
    r.from_anchor = from_anchor % SHELL_N_ANCHORS;
    r.to_anchor   = to_anchor   % SHELL_N_ANCHORS;
    r.valid       = 0u;
    r.addr        = 0u;

    /* check adjacency — O(1), max 5 iterations */
    for (uint8_t k = 0u; k < 5u; k++) {
        if (PENT_ADJ[r.from_anchor][k] == r.to_anchor) {
            r.valid = 1u;
            break;
        }
    }

    if (!r.valid) return r;  /* not adjacent — no hop */

    /* junction addr = midpoint of two anchor positions in JUNCTION space */
    uint32_t a = s->anchor[r.from_anchor];
    uint32_t b = s->anchor[r.to_anchor];
    uint32_t mid = (a + b) / 2u;

    /* perturb by seed to differentiate shells with same topology */
    r.addr = (mid + s->seed % 37u) % SHELL_JUNCTION;

    return r;
}

/*
 * shell_hop_path — walk a chain of pentagon hops, store addresses
 *
 * path[]   = array of anchor ids to visit in order
 * n_steps  = length of path[] (number of pentagons, including start)
 * out[]    = output hop addresses (length = n_steps - 1)
 *
 * Returns number of valid hops completed (stops at first non-adjacent pair).
 * O(n_steps × 5) — still effectively O(1) per hop.
 */
static inline uint8_t shell_hop_path(const ShellContainer *s,
                                      const uint8_t *path,
                                      uint8_t n_steps,
                                      uint32_t *out)
{
    if (n_steps < 2u) return 0u;
    uint8_t completed = 0u;
    for (uint8_t i = 0u; i < (uint8_t)(n_steps - 1u); i++) {
        HopResult r = shell_hop(s, path[i], path[i+1]);
        if (!r.valid) break;
        out[i] = r.addr;
        completed++;
    }
    return completed;
}

/* ── OnionStack — 12 shells radial stack ─────────────── */
/*
 * lut[from_shell][anchor_id] = addr in to_shell at same anchor
 *   precomputed once at init → cross-shell O(1) forever
 *
 * radial_ratio[i] = compression ratio of shell i (0=apex/max, 11=base/full)
 *   shell 0  → ratio = 12  (innermost, max compress, apex of frustum stack)
 *   shell 11 → ratio =  1  (outermost, full fidelity, F16 base)
 */
typedef struct {
    ShellContainer shells[ONION_N_SHELLS];
    uint32_t lut[ONION_N_SHELLS][SHELL_N_ANCHORS]; /* cross-shell junction LUT */
    uint8_t  radial_ratio[ONION_N_SHELLS];         /* compress tier per shell  */
} OnionStack;

/* ── onion_init ──────────────────────────────────────── */
/*
 * Init all 12 shells + precompute LUT.
 *
 * Each shell gets:
 *   - shell_id = i
 *   - subdivision = base_subdiv + i (outermost has most detail)
 *   - seed = seed XOR (i * 0xC0FFEE)  (unique per shell, deterministic)
 *   - mode alternates TETRA/OCTA for odd/even shells
 *
 * LUT[shell_i][anchor_j] = address at anchor_j on shell_i
 *   = shells[i].anchor[j]  (direct, O(1) lookup forever)
 *
 * radial_ratio: shell 0 = tier 12 (apex), shell 11 = tier 1 (base)
 */
static inline void onion_init(OnionStack *o,
                               uint32_t seed,
                               uint8_t base_subdiv)
{
    for (uint8_t i = 0u; i < ONION_N_SHELLS; i++) {
        uint32_t shell_seed = seed ^ ((uint32_t)i * 0xC0FFEEu);
        shell_init(&o->shells[i],
                   i,
                   (uint8_t)(base_subdiv + i),
                   shell_seed);

        /* populate LUT row for this shell — O(12) per shell, done once */
        for (uint8_t j = 0u; j < SHELL_N_ANCHORS; j++) {
            o->lut[i][j] = o->shells[i].anchor[j];
        }

        /* radial_ratio: innermost = 12 (max compress), outermost = 1 */
        o->radial_ratio[i] = (uint8_t)(ONION_N_SHELLS - i);
    }
}

/* ── onion_addr — route chord to specific shell ──────── */
/*
 * Returns uint64_t address within shell[shell_id] for given chord.
 * O(1) — delegates to shell_addr.
 */
static inline uint64_t onion_addr(const OnionStack *o,
                                   uint8_t shell_id,
                                   const Chord *c)
{
    if (shell_id >= ONION_N_SHELLS) return (uint64_t)-1;
    return shell_addr(&o->shells[shell_id], c);
}

/* ── onion_cross — radial jump via LUT ───────────────── */
/*
 * Jump from from_shell to to_shell at the same anchor_id.
 * Uses precomputed LUT → pure O(1) lookup.
 *
 * Returns junction address on to_shell, or UINT32_MAX on bad params.
 *
 * This is the "same fret, different string" operation:
 *   same anchor_id (fret position), different shell (string/radial layer)
 */
static inline uint32_t onion_cross(const OnionStack *o,
                                    uint8_t from_shell,
                                    uint8_t anchor_id,
                                    uint8_t to_shell)
{
    if (from_shell >= ONION_N_SHELLS) return UINT32_MAX;
    if (to_shell   >= ONION_N_SHELLS) return UINT32_MAX;
    if (anchor_id  >= SHELL_N_ANCHORS) return UINT32_MAX;

    /* LUT lookup — O(1), precomputed at init */
    return o->lut[to_shell][anchor_id];
}

/* ── onion_compress — frustum depth → resolution tier ── */
/*
 * The key insight: frustum apex = max compression, base = full fidelity.
 *
 * Returns shell_id (radial depth) for a given Q tier:
 *   Q4  → innermost shells (high compress)
 *   Q8  → mid shells
 *   F16 → outermost shells (full fidelity)
 *
 * tier: 0=Q4, 1=Q8, 2=F16 (or any 0..11 for fine-grained)
 * Returns shell_id 0..11
 */
#define ONION_TIER_Q4   0u
#define ONION_TIER_Q8   1u
#define ONION_TIER_F16  2u

static inline uint8_t onion_shell_for_tier(uint8_t tier)
{
    /* map 3 tiers to shell bands:
     *   Q4  → shell 0-3   (apex, max compress)
     *   Q8  → shell 4-7   (mid frustum)
     *   F16 → shell 8-11  (base, full fidelity)
     */
    if (tier == ONION_TIER_Q4)  return 0u;
    if (tier == ONION_TIER_Q8)  return 4u;
    return 8u;  /* F16 */
}

/*
 * onion_reconstruct_depth — how many shells to read for given fidelity
 *   Q4  → read shell 0 only         (1 shell)
 *   Q8  → read shells 0..7          (8 shells)
 *   F16 → read all shells 0..11     (12 shells)
 *
 * "Recipe not bytes" — read deeper = reconstruct more detail
 */
static inline uint8_t onion_reconstruct_depth(uint8_t tier)
{
    if (tier == ONION_TIER_Q4)  return 1u;
    if (tier == ONION_TIER_Q8)  return 8u;
    return ONION_N_SHELLS;  /* F16 */
}

/* ── onion_verify ────────────────────────────────────── */
/*
 * Verify all 12 shells.
 * Returns 0=all OK, or -(shell_id+1) for first failing shell.
 */
static inline int onion_verify(const OnionStack *o)
{
    for (uint8_t i = 0u; i < ONION_N_SHELLS; i++) {
        int r = shell_verify(&o->shells[i]);
        if (r != 0) return -(int)(i + 1u);
    }
    return 0;
}

#endif /* ONION_STACK_H */

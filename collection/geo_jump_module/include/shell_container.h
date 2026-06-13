/*
 * shell_container.h — POGLS Onion Shell Container (S1)
 * ══════════════════════════════════════════════════════
 *
 * 1 shell = 1 dodecahedron instance
 *   - 12 pentagon anchors (FIXED at birth)
 *   - tetra (temporal/Hilbert) ↔ octa (spatial/Peano) toggle O(1)
 *   - chord (geometry, seed, chord_id, key_offset) → address
 *
 * Depends on: geo_compound_cfg.h
 * No malloc. No float. No external deps.
 *
 * Sacred numbers: FROZEN
 *   720  = TRING_SLOTS
 *   3456 = GEO_FULL_N  = 2⁷×3³  tetra addr range
 *   6912 = JUNCTION    = 2⁸×3³  shared ceiling
 *   12   = N_ANCHORS   pentagon origins
 * ══════════════════════════════════════════════════════
 */

#ifndef SHELL_CONTAINER_H
#define SHELL_CONTAINER_H

#include <stdint.h>
#include <string.h>
#include "geo_compound_cfg.h"

/* ── Constants ───────────────────────────────────────── */
#define SHELL_N_ANCHORS   12u
#define SHELL_MAX_ID      11u
#define SHELL_TRING_SLOTS 720u
#define SHELL_GEO_FULL_N  3456u   /* tetra addr range — FROZEN */
#define SHELL_JUNCTION    6912u   /* shared ceiling   — FROZEN */

/* chord_id values (mirrors bermuda_router_v1 modes) */
#define CHORD_ORBITAL  0u   /* step +1 → adjacent, temporal forward  */
#define CHORD_CHIRAL   1u   /* half-cycle → mirror pole, backup       */
#define CHORD_CROSS    2u   /* partner zone → cross-face routing      */
#define CHORD_HUB      3u   /* zone anchor → collapse, aggregate      */

/* ── Chord: 4 values → reconstruct address ──────────── */
typedef struct {
    uint8_t  geometry;     /* 0=TETRA 1=OCTA                          */
    uint32_t seed;         /* codebook init state                     */
    uint8_t  chord_id;     /* CHORD_ORBITAL / CHIRAL / CROSS / HUB   */
    uint32_t key_offset;   /* capo: shift addr_base O(1)              */
} Chord;

/* ── ShellContainer ──────────────────────────────────── */
typedef struct {
    uint8_t  shell_id;                   /* 0-11 onion layer          */
    uint8_t  subdivision;                /* per-shell, variable       */
    uint8_t  mode;                       /* GEO_COMPOUND_TETRA/OCTA   */
    uint8_t  detached;                   /* 1=isolated 0=composite    */
    uint32_t seed;                       /* birth seed                */
    uint32_t key_offset;                 /* global capo for this shell*/
    uint32_t anchor[SHELL_N_ANCHORS];    /* pentagon addr — FIXED     */
} ShellContainer;

/* ── Init ────────────────────────────────────────────── */

/*
 * shell_init — set anchors at birth, never change after
 *
 * anchor[i] = deterministic from shell_id + subdivision + i
 * formula: (shell_id × JUNCTION + i × GEO_FULL_N/12 + seed) % JUNCTION
 * ensures 12 unique points spread across full address space
 */
static inline void shell_init(ShellContainer *s,
                               uint8_t shell_id,
                               uint8_t subdivision,
                               uint32_t seed,
                               uint8_t mode)
{
    s->shell_id    = shell_id;
    s->subdivision = subdivision;
    s->mode        = mode;
    s->detached    = 0u;
    s->seed        = seed;
    s->key_offset  = 0u;

    /* anchor spacing = JUNCTION / 12 = 576 */
    const uint32_t SPACING = SHELL_JUNCTION / SHELL_N_ANCHORS;  /* 576 */

    for (uint8_t i = 0u; i < SHELL_N_ANCHORS; i++) {
        /* base position: evenly spaced across junction space */
        uint32_t base = (uint32_t)i * SPACING;

        /* perturb by shell_id + seed to differentiate shells
         * keep within SPACING to avoid collision with neighbor anchor */
        uint32_t perturb = ((uint32_t)shell_id * 37u + seed) % SPACING;

        s->anchor[i] = (base + perturb) % SHELL_JUNCTION;
    }
}

/* ── Toggle ──────────────────────────────────────────── */

/*
 * shell_toggle — switch tetra↔octa O(1)
 * address space unchanged, interpretation changes
 * tetra = temporal walk (Hilbert), octa = spatial walk (Peano)
 */
static inline void shell_toggle(ShellContainer *s)
{
    s->mode = (s->mode == GEO_COMPOUND_TETRA)
              ? GEO_COMPOUND_OCTA
              : GEO_COMPOUND_TETRA;
}

/* ── Address ─────────────────────────────────────────── */

/*
 * shell_addr — chord → address
 *
 * Maps (geometry, seed, chord_id, key_offset) to a uint64_t address
 * within the shell's jurisdiction.
 *
 * Formula:
 *   base     = anchor[chord_id % 12]          nearest pentagon
 *   stride   = tetra: 37, octa: 41            walk step (coprime to range)
 *   step     = chord_id × stride              chord position on walk
 *   capo     = key_offset                     global shift
 *   addr     = (base + step + seed%SPACING + capo) % JUNCTION
 *
 * Deterministic: same chord → same addr every time
 * key_offset acts as capo: same chord, different shell position
 */
static inline uint64_t shell_addr(const ShellContainer *s, const Chord *c)
{
    /* pick nearest anchor by chord_id */
    uint8_t  anchor_idx = c->chord_id % SHELL_N_ANCHORS;
    uint32_t base       = s->anchor[anchor_idx];

    /* stride: tetra=37 (temporal, coprime 3456), octa=41 (spatial)
     * use shell mode as primary, chord geometry as override */
    uint8_t  geo    = c->geometry < 2u ? c->geometry : s->mode;
    uint32_t stride = (geo == GEO_COMPOUND_TETRA) ? 37u : 41u;

    /* walk: seed scatter + chord step */
    uint32_t seed_scatter = c->seed % (SHELL_JUNCTION / SHELL_N_ANCHORS);
    uint32_t chord_step   = (uint32_t)c->chord_id * stride;

    /* capo: key_offset from chord overrides shell default if nonzero */
    uint32_t capo = c->key_offset ? c->key_offset : s->key_offset;

    uint64_t addr = ((uint64_t)base
                   + seed_scatter
                   + chord_step
                   + capo) % SHELL_JUNCTION;

    /* clamp to mode range (shell mode = active scan strategy) */
    if (s->mode == GEO_COMPOUND_TETRA) {
        addr = addr % SHELL_GEO_FULL_N;                       /* 0..3455    */
    } else {
        addr = SHELL_GEO_FULL_N + (addr % SHELL_GEO_FULL_N); /* 3456..6911 */
    }

    return addr;
}

/* ── Verify ──────────────────────────────────────────── */

/*
 * shell_verify — sanity check
 * returns 0 = OK, -1 = bad id, -2 = anchor collision
 */
static inline int shell_verify(const ShellContainer *s)
{
    if (s->shell_id > SHELL_MAX_ID) return -1;

    /* check no two anchors are identical */
    for (uint8_t i = 0u; i < SHELL_N_ANCHORS; i++)
        for (uint8_t j = (uint8_t)(i + 1u); j < SHELL_N_ANCHORS; j++)
            if (s->anchor[i] == s->anchor[j]) return -2;

    return 0;
}

#endif /* SHELL_CONTAINER_H */

/*
 * shell_container.h — POGLS Onion Shell Container (S1)
 * ══════════════════════════════════════════════════════
 *
 * MIGRATED to geo_jump Y-triangle unified space.
 * No tetra/octa mode toggle. No compound type selection.
 * Single address space: GEO_FULL = 20736.
 *
 * 1 shell = 1 dodecahedron instance
 *   - 12 pentagon anchors (FIXED at birth)
 *   - Y-triangle routing via geo_jump operations
 *
 * Depends on: geo_jump.h
 * No malloc. No float.
 * ══════════════════════════════════════════════════════
 */

#ifndef SHELL_CONTAINER_H
#define SHELL_CONTAINER_H

#include <stdint.h>
#include <string.h>
#include "geo_jump.h"

/* ── Constants ───────────────────────────────────────── */
#define SHELL_N_ANCHORS      12u
#define SHELL_MAX_ID         11u
#define SHELL_GEO_FULL_N     GEO_FULL     /* 20736 — unified address space */
#define SHELL_JUNCTION       GEO_FULL     /* no residual split             */
#define SHELL_STRIDE_DEFAULT 37u          /* unified walk stride           */

/* chord_id values (mirrors bermuda_router_v1 modes) */
#define CHORD_ORBITAL  0u   /* step +1 → adjacent, temporal forward  */
#define CHORD_CHIRAL   1u   /* half-cycle → mirror pole, backup       */
#define CHORD_CROSS    2u   /* partner zone → cross-face routing      */
#define CHORD_HUB      3u   /* zone anchor → collapse, aggregate      */

/* ── Chord: 4 values → reconstruct address ──────────── */
typedef struct {
    uint32_t seed;         /* codebook init state                     */
    uint8_t  chord_id;     /* CHORD_ORBITAL / CHIRAL / CROSS / HUB   */
    uint32_t key_offset;   /* capo: shift addr_base O(1)              */
} Chord;

/* ── ShellContainer ──────────────────────────────────── */
typedef struct {
    uint8_t  shell_id;                   /* 0-11 onion layer          */
    uint8_t  subdivision;                /* per-shell, variable       */
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
 * formula: (shell_id × GEO_FULL + i × GEO_FULL/12 + seed) % GEO_FULL
 * ensures 12 unique points spread across full 20736 space
 */
static inline void shell_init(ShellContainer *s,
                                uint8_t shell_id,
                                uint8_t subdivision,
                                uint32_t seed)
{
    s->shell_id    = shell_id;
    s->subdivision = subdivision;
    s->detached    = 0u;
    s->seed        = seed;
    s->key_offset  = 0u;

    /* anchor spacing = GEO_FULL / 12 = 1728 */
    const uint32_t SPACING = SHELL_JUNCTION / SHELL_N_ANCHORS;

    for (uint8_t i = 0u; i < SHELL_N_ANCHORS; i++) {
        /* base position: evenly spaced across full space */
        uint32_t base = (uint32_t)i * SPACING;

        /* perturb by shell_id + seed to differentiate shells
         * keep within SPACING to avoid collision with neighbor anchor */
        uint32_t perturb = ((uint32_t)shell_id * SHELL_STRIDE_DEFAULT + seed) % SPACING;

        s->anchor[i] = (base + perturb) % SHELL_JUNCTION;
    }
}

/* ── Address ─────────────────────────────────────────── */

/*
 * shell_addr — chord → address
 *
 * Maps (seed, chord_id, key_offset) to a uint32_t address
 * within the shell's jurisdiction.
 *
 * Formula:
 *   base     = anchor[chord_id % 12]         nearest pentagon
 *   step     = chord_id × STRIDE             chord position on walk
 *   capo     = key_offset                    global shift
 *   addr     = (base + step + seed%SPACING + capo) % GEO_FULL
 *
 * Deterministic: same chord → same addr every time
 * key_offset acts as capo: same chord, different shell position
 */
static inline uint32_t shell_addr(const ShellContainer *s, const Chord *c)
{
    /* pick nearest anchor by chord_id */
    uint8_t  anchor_idx = c->chord_id % SHELL_N_ANCHORS;
    uint32_t base       = s->anchor[anchor_idx];

    /* unified stride — no tetra/octa split */
    uint32_t stride = SHELL_STRIDE_DEFAULT;

    /* walk: seed scatter + chord step */
    uint32_t seed_scatter = c->seed % (SHELL_JUNCTION / SHELL_N_ANCHORS);
    uint32_t chord_step   = (uint32_t)c->chord_id * stride;

    /* capo: key_offset from chord overrides shell default if nonzero */
    uint32_t capo = c->key_offset ? c->key_offset : s->key_offset;

    return (base + seed_scatter + chord_step + capo) % SHELL_JUNCTION;
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

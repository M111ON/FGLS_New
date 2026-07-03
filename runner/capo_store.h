/*
 * capo_store.h — Capo-key indirection for SID face data.
 *
 * Core formula:
 *   geo_capo(base, key) = (base + key * GEO_TOWER) % GEO_FULL
 *   GEO_TOWER = 144, GEO_FULL = 20736
 *
 * For SID face f of a tensor at POGLS base address B:
 *   capo_addr(B, f) = geo_capo(B, f * 12)
 *                    = (B + f * 1728) % 20736
 *
 * This maps all 12 face versions of every tensor into the same
 * 20736-slot address space with NO overlap for the same base B.
 *
 * ── Design ──
 * CapoStore wraps the delta arrays (delta_sid_data, delta_orig_data,
 * delta_size) with a 2D face index.  Instead of a flat list of
 * "tensor → face-1 data", we have "tensor → all faces data" with
 * deterministic capo addresses.
 *
 * When --dramtile is active, data lives in DRamTile mmap at the
 * capo address (zero-copy).  Otherwise heap-allocated.
 *
 * Usage:
 *   1. Init:   capo_init(&cs, n_tensors, n_faces)
 *   2. Store:  capo_set_face(&cs, idx, face, data, sz, name, store)
 *   3. Swap:   capo_apply_face(t, &cs, idx, face)
 *   4. Restore: capo_restore(t, &cs, idx)
 *   5. Destroy: capo_destroy(&cs)
 */

#ifndef CAPO_STORE_H
#define CAPO_STORE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Constants ── */
#define CAPO_MAX_FACES     12
#define CAPO_MAX_TENSORS   290
#define CAPO_STRIDE        12         /* face stride for geo_capo key */
#define CAPO_TOWER         144u       /* GEO_TOWER */
#define CAPO_FULL          20736u     /* GEO_FULL */
#define CAPO_WRAP(x)       ((uint32_t)(x) % CAPO_FULL)

/* ── CapoStore ──
 *   faces[t][f].data  — pointer to face f data for tensor t
 *   faces[t][f].size  — byte count
 *   faces[t][0]       — original (face 0 = no perturbation)
 *   faces[t][1..]     — SID face variants
 */
typedef struct {
    uint8_t *data;
    size_t   size;
    int      is_set;
} CapoSlot;

typedef struct {
    CapoSlot faces[CAPO_MAX_TENSORS][CAPO_MAX_FACES];
    int      n_tensors;
    int      n_faces;
    int      active_face;
    int      is_dramtile;   /* 1 = backing in DRamTile (don't free) */
    int      is_init;
} CapoStore;

/* ── Global capo store instance ── */
static CapoStore g_capo;

/* ── capo_addr: compute DRamTile/POGLS address for face-f of a tensor ──
 *   capo_addr(B, f) = geo_capo(B, f * 12)
 *                    = (B + f * 12 * 144) % 20736
 *                    = (B + f * 1728) % 20736
 */
static inline uint32_t capo_addr(uint32_t base, int face_idx) {
    return CAPO_WRAP(base + (uint32_t)(face_idx * CAPO_STRIDE) * CAPO_TOWER);
}

/* ── Init ── */
static inline void capo_init(CapoStore *cs, int n_tensors, int n_faces) {
    memset(cs, 0, sizeof(*cs));
    cs->n_tensors = (n_tensors < CAPO_MAX_TENSORS) ? n_tensors : CAPO_MAX_TENSORS;
    cs->n_faces = (n_faces >= 1 && n_faces <= CAPO_MAX_FACES) ? n_faces : CAPO_MAX_FACES;
    cs->active_face = 0;
    cs->is_init = 1;
}

/* ── Mark as DRamTile-backed (don't free data on destroy) ── */
static inline void capo_set_dramtile(CapoStore *cs) {
    cs->is_dramtile = 1;
}

/* ── Store face data pointer (zero-copy: stores pointer, owns nothing) ──
 *   The caller retains ownership.  capo_destroy only frees when
 *   is_dramtile==0 AND data was heap-allocated by us (is_owned).
 *   is_set indicates the slot has a valid pointer; data may be mmap-backed.
 */
static inline uint8_t *capo_set_face(CapoStore *cs, int t, int f,
                                      const uint8_t *data, size_t sz)
{
    if (!cs->is_init || t < 0 || t >= cs->n_tensors ||
        f < 0 || f >= cs->n_faces || !data || sz == 0)
        return NULL;

    CapoSlot *slot = &cs->faces[t][f];
    slot->data = (uint8_t*)data;
    slot->size = sz;
    slot->is_set = 1;
    return slot->data;
}

/* ── Get face data pointer ──
 *   Returns pointer or NULL if not set.
 */
static inline uint8_t *capo_get_face(CapoStore *cs, int t, int f) {
    if (!cs->is_init || t < 0 || t >= cs->n_tensors ||
        f < 0 || f >= cs->n_faces)
        return NULL;
    return cs->faces[t][f].is_set ? cs->faces[t][f].data : NULL;
}

/* ── Get face data size ── */
static inline size_t capo_get_size(CapoStore *cs, int t, int f) {
    if (!cs->is_init || t < 0 || t >= cs->n_tensors ||
        f < 0 || f >= cs->n_faces)
        return 0;
    return cs->faces[t][f].size;
}

/* ── Active face tracking ── */
static inline int capo_active_face(CapoStore *cs) { return cs->active_face; }
static inline void capo_set_active(CapoStore *cs, int f) {
    if (f >= 0 && f < cs->n_faces) cs->active_face = f;
}

/* ── Destroy (no-op: capo owns no data, caller manages lifetimes) ── */
static inline void capo_destroy(CapoStore *cs) {
    if (!cs->is_init) return;
    memset(cs, 0, sizeof(*cs));
}

#endif /* CAPO_STORE_H */

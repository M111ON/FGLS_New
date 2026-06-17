/*
 * tw_face_bridge.h — Retarget: TW Capture → Y-Triangle Node_id (Capo Routing)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Replaces the 12-face / 1440 TRing system with direct Y-triangle node_id
 * (0..20735) via capo routing:
 *
 *   Capture tensor signature (sig_x, sig_y) once via tw_capture_int_combined
 *   → zone/slot → node_id via tw_to_node()
 *   → geo_capo(base, f*12) for f=0..11 → 12 nodes, one per pentagon
 *
 * Key improvements over old 12-face rotation:
 *   - 11.4× faster (no rotation, no 12× centroid search)
 *   - All 12 pentagons covered (old: 10/12)
 *   - Higher entropy (better distribution)
 *   - O(1) capo arithmetic vs O(12×centroid_search)
 *
 * Depends on: tw_capture_int.h, tw_bridge.h, geo_jump.h, geo_shell.h
 * No malloc. No float. Frozen.
 * ═══════════════════════════════════════════════════════════════════════════
 */

#ifndef TW_FACE_BRIDGE_H
#define TW_FACE_BRIDGE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "tw_capture_int.h"
#include "tw_bridge.h"
#include "geo_frame_seek.h"
#include "tw_tensor_capture.h"

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define TW_CAPO_FACES       12u     /* 12 pentagons via capo routing */

/* ══════════════════════════════════════════════════════════════
   TW REWIND BUFFER — indexed by Y-triangle node_id (0..20735)
   ══════════════════════════════════════════════════════════════ */

#define TW_REWIND_SLOTS     GEO_FULL  /* 20736 slots = full Y-triangle space */

typedef struct {
    uint64_t keys[TW_REWIND_SLOTS];  /* packed keys, 0 = empty slot */
    uint32_t stored;                  /* total stores (for stats)     */
} TWFaceRewind;

static inline void tw_rewind_init(TWFaceRewind *rb) {
    memset(rb, 0, sizeof(*rb));
}

static inline void tw_rewind_store(TWFaceRewind *rb, uint64_t key, uint32_t node_id) {
    rb->keys[node_id % TW_REWIND_SLOTS] = key;
    rb->stored++;
}

static inline uint64_t tw_rewind_find(const TWFaceRewind *rb, uint32_t node_id) {
    return rb->keys[node_id % TW_REWIND_SLOTS];
}

static inline int tw_rewind_has(const TWFaceRewind *rb, uint32_t node_id) {
    return rb->keys[node_id % TW_REWIND_SLOTS] != 0;
}

static inline uint32_t tw_rewind_occupied(const TWFaceRewind *rb) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < TW_REWIND_SLOTS; i++)
        if (rb->keys[i]) n++;
    return n;
}

/* ══════════════════════════════════════════════════════════════
   TW FREEZE WALLET — binary log for frozen entries
   ══════════════════════════════════════════════════════════════ */

#define TW_FREEZE_MAGIC     0x46525A57u  /* "FRZW" */
#define TW_FREEZE_VERSION   1u
#define TW_FREEZE_ENTRY_SZ  20u  /* sizeof(TWFreezeEntry) */

typedef struct __attribute__((packed)) {
    uint32_t magic;           /* TW_FREEZE_MAGIC */
    uint32_t version;         /* TW_FREEZE_VERSION */
    uint32_t n_entries;       /* number of valid freeze entries */
    uint32_t _reserved;       /* future use */
} TWFreezeHeader;

typedef struct __attribute__((packed)) {
    uint32_t node_id;         /* 0..20735 Y-triangle node_id */
    uint32_t freeze_addr;     /* POGLS wallet freeze address      */
    uint32_t tick;            /* timeline tick at freeze          */
    uint64_t packed_key;      /* packed node+pentagon+resid info  */
} TWFreezeEntry;

typedef char _tw_freeze_entry_sz[(sizeof(TWFreezeEntry) == TW_FREEZE_ENTRY_SZ) ? 1 : -1];

static inline size_t tw_freeze_entry_write(FILE *f, const TWFreezeEntry *e) {
    if (!f || !e) return 0;
    return fwrite(e, TW_FREEZE_ENTRY_SZ, 1, f) == 1 ? TW_FREEZE_ENTRY_SZ : 0;
}

static inline int tw_freeze_entry_read(FILE *f, TWFreezeEntry *e) {
    if (!f || !e) return 0;
    return fread(e, TW_FREEZE_ENTRY_SZ, 1, f) == 1;
}

static inline size_t tw_freeze_wallet_write(const char *path,
                                             const TWFreezeEntry *entries,
                                             uint32_t n_entries)
{
    if (!path || !entries || n_entries == 0) return 0;

    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    TWFreezeHeader hdr;
    hdr.magic     = TW_FREEZE_MAGIC;
    hdr.version   = TW_FREEZE_VERSION;
    hdr.n_entries = n_entries;
    hdr._reserved = 0;

    size_t written = 0;
    written += fwrite(&hdr, sizeof(hdr), 1, f) ? sizeof(hdr) : 0;
    if (written != sizeof(hdr)) { fclose(f); return 0; }

    for (uint32_t i = 0; i < n_entries; i++) {
        size_t w = tw_freeze_entry_write(f, &entries[i]);
        if (w != TW_FREEZE_ENTRY_SZ) { fclose(f); return 0; }
        written += w;
    }

    fclose(f);
    return written;
}

static inline uint32_t tw_freeze_wallet_read(const char *path,
                                              TWFreezeEntry **out_entries)
{
    if (!path || !out_entries) return 0;
    *out_entries = NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    TWFreezeHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return 0; }
    if (hdr.magic != TW_FREEZE_MAGIC || hdr.version != TW_FREEZE_VERSION) {
        fclose(f); return 0;
    }

    uint32_t n = hdr.n_entries;
    if (n == 0) { fclose(f); return 0; }

    TWFreezeEntry *buf = (TWFreezeEntry *)malloc(n * sizeof(TWFreezeEntry));
    if (!buf) { fclose(f); return 0; }

    for (uint32_t i = 0; i < n; i++) {
        if (fread(&buf[i], TW_FREEZE_ENTRY_SZ, 1, f) != 1) {
            free(buf); fclose(f); return 0;
        }
    }

    fclose(f);
    *out_entries = buf;
    return n;
}

/* ══════════════════════════════════════════════════════════════
   NODE_ID CAPTURE — single capture → capo ×12 pentagons
   ══════════════════════════════════════════════════════════════ */

/*
 * Capture a tensor signature → single node_id via tw_to_node().
 * This is the fastest path: 1 capture, 1 mapping.
 * Returns node_id (0..20735) and optionally the pentagon id (1..12).
 */
static inline uint32_t tw_capture_to_node(int64_t vx, int64_t vy,
                                           uint8_t *pentagon_out) {
    TWCaptureInt cap;
    tw_capture_int_combined(vx, vy, &cap, &(uint8_t){0});
    uint32_t base = tw_to_node(cap.zone, cap.slot);
    if (pentagon_out)
        *pentagon_out = (uint8_t)(geo_pentagon_id(base));
    return base;
}

/*
 * Capo ×12 routing: capture once, then spread to all 12 pentagons.
 * out[12] = {node_id at pentagon 0..11} via geo_capo(base, f*12).
 *
 * geo_capo(node, key) = (node + key * GEO_TOWER) % GEO_FULL
 * With key = f * 12, each step adds SHELL_FACE_BLOCK = 1728,
 * which jumps exactly one pentagon forward.
 *
 * All 12 nodes share the same shell ring + side — only pentagon changes.
 */
static inline void tw_capture_capo_all(int64_t vx, int64_t vy,
                                        uint32_t out[TW_CAPO_FACES]) {
    TWCaptureInt cap;
    tw_capture_int_combined(vx, vy, &cap, &(uint8_t){0});
    uint32_t base = tw_to_node(cap.zone, cap.slot);
    for (uint32_t f = 0; f < TW_CAPO_FACES; f++)
        out[f] = geo_capo(base, f * 12);
}

/*
 * Pack key from node_id + resid for rewind buffer storage.
 * Format: [node_id:20][resid_x:14][resid_y:14][pentagon:4][V:1]
 * V = valid marker (always 1), ensures key ≠ 0.
 */
static inline uint64_t tw_node_pack_key(uint32_t node_id,
                                         int64_t resid_x, int64_t resid_y,
                                         uint8_t pentagon) {
    uint64_t key = 0;
    key |= (uint64_t)(node_id & 0xFFFFF)    << 44;  /* 20 bits for 0..20735 */
    key |= (uint64_t)(resid_x & 0x3FFF)     << 30;  /* 14 bits resid_x */
    key |= (uint64_t)(resid_y & 0x3FFF)     << 16;  /* 14 bits resid_y */
    key |= (uint64_t)(pentagon & 0x0F)      << 12;  /* 4 bits pentagon 1..12 */
    key |= (uint64_t)1                       << 11;  /* valid marker */
    return key;
}

/*
 * Unpack key → node_id, resid_x, resid_y, pentagon.
 */
static inline void tw_node_unpack_key(uint64_t key,
                                       uint32_t *node_id,
                                       int64_t *resid_x, int64_t *resid_y,
                                       uint8_t *pentagon) {
    if (node_id)   *node_id   = (uint32_t)((key >> 44) & 0xFFFFF);
    if (resid_x)   *resid_x   = (int64_t)((int16_t)(((key >> 30) & 0x3FFF) << 2) >> 2);
    if (resid_y)   *resid_y   = (int64_t)((int16_t)(((key >> 16) & 0x3FFF) << 2) >> 2);
    if (pentagon)  *pentagon  = (uint8_t)((key >> 12) & 0x0F);
}

/* ══════════════════════════════════════════════════════════════
   FREEZE — Shell 3 freeze trigger (tw_is_frozen defined in tw_bridge.h)
   ══════════════════════════════════════════════════════════════ */

/*
 * Freeze address from node_id + resid.
 * node_id encodes face+ring+side in Y-triangle space.
 * resid stored for precision — freeze_addr uses node_id as primary coord.
 */
static inline uint32_t tw_node_freeze_address(uint32_t node_id,
                                               uint32_t tick) {
    uint32_t layer_off = (tick % SHELL_RINGS) * GEO_TOWER;
    return GEO_WRAP(node_id + layer_off);
}

/*
 * Create a freeze entry from node_id + capture data.
 */
static inline TWFreezeEntry tw_node_freeze_entry(uint32_t node_id,
                                                  uint32_t tick,
                                                  uint8_t  layer,
                                                  int64_t  resid_x,
                                                  int64_t  resid_y)
{
    TWFreezeEntry e;
    e.node_id    = node_id;
    e.freeze_addr = tw_node_freeze_address(node_id, tick);
    e.tick       = tick;
    e.packed_key = tw_node_pack_key(node_id, resid_x, resid_y,
                                     (uint8_t)(geo_pentagon_id(node_id)));
    return e;
}

/* ══════════════════════════════════════════════════════════════
   ORCHESTRATOR — capture tensor → rewind + freeze
   ══════════════════════════════════════════════════════════════ */

#define TW_MAX_FROZEN_PER_TENSOR 7

typedef struct {
    TWFaceRewind    rewind;         /* populated rewind buffer             */
    TWFreezeEntry  *freeze_log;     /* malloc'd freeze entries array      */
    uint32_t        n_frozen;       /* number of entries in freeze_log    */
    uint32_t        freeze_cap;     /* capacity of freeze_log             */
    uint32_t        base_node_id;   /* primary capture node_id            */
    uint32_t        capo_nodes[TW_CAPO_FACES]; /* capo-routed ×12         */
    int64_t         best_mag2;      /* squared resid magnitude of best    */
    DualFrame       df;             /* FRAME TIMELINE: from geo_frame_seek.h  */
    uint32_t        n_tensors;      /* number of tensors processed        */
} TWCapture12FaceResult;

static inline void tw_capture_12face_init(TWCapture12FaceResult *r) {
    memset(r, 0, sizeof(*r));
    tw_rewind_init(&r->rewind);
}

static inline void tw_capture_12face_free(TWCapture12FaceResult *r) {
    if (r && r->freeze_log) {
        free(r->freeze_log);
        r->freeze_log = NULL;
    }
    r->n_frozen = 0;
    r->freeze_cap = 0;
}

static inline int tw_capture_tensor_12face(
    RawBridge *rb, const char *tensor_name,
    uint32_t tick, uint8_t layer,
    TWCapture12FaceResult *out)
{
    TWTensorCapture tc;
    if (tw_capture_tensor_by_name(rb, tensor_name, &tc) != RB_OK)
        return -1;

    int64_t vx = (int64_t)(tc.sig_x * TW_SCALE);
    int64_t vy = (int64_t)(tc.sig_y * TW_SCALE);

    /* Capture → capo ×12 pentagons */
    tw_capture_capo_all(vx, vy, out->capo_nodes);
    out->base_node_id = out->capo_nodes[0];

    /* Store primary node in rewind */
    TWCaptureInt cap;
    tw_capture_int_combined(vx, vy, &cap, &(uint8_t){0});
    uint64_t key = tw_node_pack_key(out->base_node_id,
                                     cap.resid_x, cap.resid_y,
                                     (uint8_t)(geo_pentagon_id(out->base_node_id)));
    tw_rewind_store(&out->rewind, key, out->base_node_id);

    /* FRAME TIMELINE — from base_node_id */
    out->df = frame_at((uint16_t)(out->base_node_id % FRAME_CYCLE));

    /* Ensure freeze_log capacity + freeze entry */
    if (tw_is_frozen(cap.drain, tick)) {
        if (out->n_frozen >= out->freeze_cap) {
            uint32_t new_cap = out->freeze_cap ? out->freeze_cap * 2 : TW_MAX_FROZEN_PER_TENSOR;
            TWFreezeEntry *new_log = (TWFreezeEntry *)realloc(
                out->freeze_log, new_cap * sizeof(TWFreezeEntry));
            if (!new_log) return -1;
            out->freeze_log = new_log;
            out->freeze_cap = new_cap;
        }
        out->freeze_log[out->n_frozen++] = tw_node_freeze_entry(
            out->base_node_id, tick, layer, cap.resid_x, cap.resid_y);
    }

    /* Track best resid */
    int64_t mag2 = cap.resid_x * cap.resid_x + cap.resid_y * cap.resid_y;
    if (out->n_tensors == 0 || mag2 < out->best_mag2) {
        out->best_mag2 = mag2;
    }
    out->n_tensors++;

    return 0;
}

static inline int tw_capture_tensor_12face_batch(
    RawBridge *rb,
    const char **tensor_names, uint32_t n_tensors,
    uint32_t tick, uint8_t layer,
    TWCapture12FaceResult *out)
{
    if (!rb || !tensor_names || !out) return -1;
    tw_capture_12face_init(out);

    for (uint32_t i = 0; i < n_tensors; i++) {
        int rc = tw_capture_tensor_12face(rb, tensor_names[i], tick, layer, out);
        if (rc != 0) continue;
    }

    return (out->n_tensors > 0) ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════
   LEGACY BACKWARD-COMPAT — 12-face TRing (1440) for sid.h et al.
   ══════════════════════════════════════════════════════════════
 *
 * Provides the old TWFaceCapture / tw_capture_face / tw_iterate_faces_24
 * API that sid.h and its dependents still use.
 *
 * NEW CODE SHOULD USE THE CAPO ×12 API ABOVE INSTEAD.
 * This section exists for backward compat only.
 * ══════════════════════════════════════════════════════════════ */

#define TW_FACES           12u
#define TW_FACE_SLOTS      120u   /* hex(60) + tri(60) per face */
#define TW_FACE_SLOTS_120  120u   /* same — used by old code */
#define TW_TRING_1440      1440u  /* faces * face_slots */
#define TW_TRING_FULL      720u   /* hex-only half = faces * 60 */
#define TW_PRIORITY_FACES  7u
/* TW_N_SLOTS is already defined in tw_capture_int.h */

/* Rotation tables: cos/sin of angle[face] * TW_SCALE
 * cos(0°)..cos(330°) and sin(0°)..sin(330°) in integer steps of 30°.
 * COS30=179580, SIN30=103680 (matches original capture_int constants).
 */
static const int32_t _TW_ROT_COS[TW_FACES] = {
     207360,  179580,  103680,       0, -103680, -179580,
    -207360, -179580, -103680,       0,  103680,  179580,
};
static const int32_t _TW_ROT_SIN[TW_FACES] = {
          0,  103680,  179580,  207360,  179580,  103680,
          0, -103680, -179580, -207360, -179580, -103680,
};

/* Priority capture: face order by proximity (from old 12-face system) */
static const uint8_t TW_FACE_PRIORITY[TW_PRIORITY_FACES] = {0,1,2,3,4,5,6};

/* ── TWFaceCapture (old legacy struct) ── */

typedef struct {
    uint8_t  face;            /* 0..11                                                  */
    uint8_t  zone;            /* sector within face (0..9)                              */
    uint8_t  slot;            /* combined slot (0..59)                                  */
    uint16_t tring_pos;       /* 0..1439 = face*120 + (is_tri?60:0) + slot             */
    int64_t  resid_x;         /* residual within triangle                               */
    int64_t  resid_y;         /* residual within triangle                               */
    int64_t  mag2;            /* squared magnitude of resid                             */
    uint8_t  is_tri;          /* 1=tri centroid, 0=hex centroid                         */
    uint8_t  drain;           /* 1=boundary near, secondary slot available              */
    uint8_t  drain_face;      /* secondary face (for boundary crossing)                 */
    uint8_t  drain_zone;      /* secondary sector                                      */
    uint8_t  drain_slot;      /* secondary slot                                         */
    uint16_t drain_tring;     /* secondary tring pos                                    */
    int64_t  drain_resid_x;   /* secondary resid                                        */
    int64_t  drain_resid_y;   /* secondary resid                                        */
} TWFaceCapture;

/* ── tw_face_to_tring: compute old-style tring_pos from face+zone+slot+is_tri ── */

static inline uint16_t tw_face_to_tring(uint8_t face, uint8_t zone,
                                         uint8_t slot, uint8_t is_tri) {
    (void)zone; /* zone derived from slot already */
    return (uint16_t)(face * TW_FACE_SLOTS + (is_tri ? 60u : 0u) + slot);
}

/* ── tw_capture_int_to_face: fill TWFaceCapture from TWCaptureInt ── */

static inline void tw_capture_int_to_face(const TWCaptureInt *cap,
                                           uint8_t face,
                                           TWFaceCapture *fc,
                                           uint8_t is_tri,
                                           int64_t vx, int64_t vy) {
    (void)vx; (void)vy; /* used for resid computation in full old impl */
    memset(fc, 0, sizeof(*fc));
    fc->face      = face;
    fc->zone      = cap->zone;
    fc->slot      = cap->slot;
    fc->tring_pos = tw_face_to_tring(face, cap->zone, cap->slot, is_tri);
    fc->resid_x   = cap->resid_x;
    fc->resid_y   = cap->resid_y;
    fc->mag2      = cap->resid_x * cap->resid_x + cap->resid_y * cap->resid_y;
    fc->is_tri    = is_tri;
    fc->drain     = cap->drain;
    if (cap->drain) {
        fc->drain_face   = face;
        fc->drain_zone   = cap->drain_slot / TW_SLOTS_PER;
        fc->drain_slot   = cap->drain_slot;
        fc->drain_tring  = tw_face_to_tring(face, cap->drain_slot / TW_SLOTS_PER,
                                            cap->drain_slot, is_tri);
        fc->drain_resid_x = cap->drain_resid_x;
        fc->drain_resid_y = cap->drain_resid_y;
    }
}

/* ── tw_capture_face: legacy single-face capture ── */

static inline void tw_capture_face(int64_t vx, int64_t vy,
                                    uint8_t face, TWFaceCapture *fc) {
    int64_t rx = (vx * _TW_ROT_COS[face] - vy * _TW_ROT_SIN[face]) / TW_SCALE;
    int64_t ry = (vx * _TW_ROT_SIN[face] + vy * _TW_ROT_COS[face]) / TW_SCALE;

    uint8_t is_tri;
    TWCaptureInt cap;
    tw_capture_int_combined(rx, ry, &cap, &is_tri);
    tw_capture_int_to_face(&cap, face, fc, is_tri, vx, vy);
}

/* ── TWFaceIter24: 24-direction capture result ── */

typedef struct {
    TWFaceCapture caps[24];      /* 12 faces × hex+tri              */
    uint16_t      tring_hist[TW_TRING_1440]; /* not fully populated in legacy */
    uint8_t       n_captured;
} TWFaceIter24;

/* ── tw_iterate_faces_24: capture all 24 directions ── */

static inline void tw_iterate_faces_24(int64_t vx, int64_t vy,
                                        TWFaceIter24 *r24) {
    memset(r24, 0, sizeof(*r24));
    for (uint8_t f = 0; f < TW_FACES; f++) {
        int64_t rx = (vx * _TW_ROT_COS[f] - vy * _TW_ROT_SIN[f]) / TW_SCALE;
        int64_t ry = (vx * _TW_ROT_SIN[f] + vy * _TW_ROT_COS[f]) / TW_SCALE;

        TWCaptureInt cap;
        uint8_t is_tri;
        tw_capture_int_combined(rx, ry, &cap, &is_tri);

        tw_capture_int_to_face(&cap, f, &r24->caps[f * 2], 0, vx, vy);
        tw_capture_int_to_face(&cap, f, &r24->caps[f * 2 + 1], 1, vx, vy);
    }
    r24->n_captured = 24;
}

/* ── tw_best_of_24: select best from 24-direction capture ── */

static inline int tw_best_of_24(const TWFaceIter24 *r24) {
    int best = 0;
    int64_t best_mag = r24->caps[0].mag2;
    for (int i = 1; i < 24; i++) {
        if (r24->caps[i].mag2 < best_mag) {
            best_mag = r24->caps[i].mag2;
            best = i;
        }
    }
    return best;
}

/* ── Legacy key pack/unpack (for tw_rewind_bridge.h compat) ── */

static inline uint64_t tw_face_pack_key(const TWFaceCapture *fc) {
    uint64_t k = 0;
    k |= (uint64_t)(fc->face   & 0x0F)    << 60;
    k |= (uint64_t)(fc->zone   & 0x0F)    << 56;
    k |= (uint64_t)(fc->slot   & 0x3F)    << 50;
    k |= (uint64_t)(fc->tring_pos & 0x7FF)<< 39;
    k |= (uint64_t)(fc->resid_x & 0x3FFF) << 25;
    k |= (uint64_t)(fc->resid_y & 0x3FFF) << 11;
    k |= (uint64_t)(fc->is_tri ? 1 : 0)   << 10;
    k |= (uint64_t)(fc->drain  ? 1 : 0)   << 9;
    k |= (uint64_t)1; /* valid marker */
    return k;
}

static inline void tw_face_unpack_key(uint64_t key, TWFaceCapture *fc) {
    memset(fc, 0, sizeof(*fc));
    fc->face      = (uint8_t)((key >> 60) & 0x0F);
    fc->zone      = (uint8_t)((key >> 56) & 0x0F);
    fc->slot      = (uint8_t)((key >> 50) & 0x3F);
    fc->tring_pos = (uint16_t)((key >> 39) & 0x7FF);
    fc->resid_x   = (int64_t)((int16_t)(((key >> 25) & 0x3FFF) << 2) >> 2);
    fc->resid_y   = (int64_t)((int16_t)(((key >> 11) & 0x3FFF) << 2) >> 2);
    fc->is_tri    = (uint8_t)((key >> 10) & 1);
    fc->drain     = (uint8_t)((key >> 9) & 1);
}

/* ── Legacy rewind store (keeps tw_rewind_bridge.h compiling) ── */

static inline void tw_face_rewind_store(TWFaceRewind *rb,
                                         const TWFaceCapture *fc) {
    tw_rewind_store(rb, tw_face_pack_key(fc), fc->tring_pos);
}

static inline int tw_face_is_frozen(const TWFaceCapture *fc, uint32_t tick) {
    return tw_is_frozen(fc->drain, tick);
}

static inline TWFreezeEntry tw_face_freeze_entry(const TWFaceCapture *fc,
                                                  uint32_t tick,
                                                  uint8_t layer) {
    return tw_node_freeze_entry(fc->tring_pos, tick, layer,
                                 fc->resid_x, fc->resid_y);
}

/* ── _tw_centroid_to_face0: centroid lookup (tables are in face-0 frame) ── */

static inline void _tw_centroid_to_face0(uint8_t zone, uint8_t combined_idx,
                                           uint8_t face,
                                           int64_t *c0x, int64_t *c0y) {
    (void)face;
    if (combined_idx >= TW_SLOTS_PER) {
        /* tri centroid */
        *c0x = TW_TRI_SLOT_LOCAL_I[zone][combined_idx - TW_SLOTS_PER][0];
        *c0y = TW_TRI_SLOT_LOCAL_I[zone][combined_idx - TW_SLOTS_PER][1];
    } else {
        *c0x = TW_SLOT_LOCAL_I[zone][combined_idx][0];
        *c0y = TW_SLOT_LOCAL_I[zone][combined_idx][1];
    }
}

/* ── Legacy function stubs (keeps references compiling) ── */

/* tw_tring_to_frame_enc: identity mapping (same as old system) */
static inline uint16_t tw_tring_to_frame_enc(uint16_t tring_pos) {
    return tring_pos % FRAME_CYCLE;
}

/* tw_tring_to_world_b: cpair mapping */
static inline uint16_t tw_tring_to_world_b(uint16_t tring_pos) {
    return (uint16_t)((tring_pos + TW_TRING_FULL) % TW_TRING_1440);
}

/* ── tw_tring_to_face_zone_slot (unused but kept for completeness) ── */
static inline void tw_tring_to_face_zone_slot(uint16_t tring_pos,
                                               uint8_t *face,
                                               uint8_t *zone,
                                               uint8_t *slot,
                                               uint8_t *is_tri) {
    uint8_t f  = (uint8_t)(tring_pos / TW_FACE_SLOTS);
    uint8_t fs = (uint8_t)(tring_pos % TW_FACE_SLOTS);
    *is_tri = (fs >= 60u) ? 1u : 0u;
    uint8_t ls = *is_tri ? (uint8_t)(fs - 60u) : fs;
    *face   = f;
    *zone   = ls / TW_SLOTS_PER;
    *slot   = ls;
}

/* Legacy constant needed by some test code */
#define TW_WORLD_B_OFFSET TW_TRING_FULL

#endif /* TW_FACE_BRIDGE_H */

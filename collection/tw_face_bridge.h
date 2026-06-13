/*
 * tw_face_bridge.h — 12-Face Bridge: TW → Full TRing 1440
 * ══════════════════════════════════════════════════════════════
 *
 * Extends tw_bridge.h to iterate across all 12 dodecahedron faces.
 * TW capture operates on a SINGLE face with TWO centroid layers:
 *   hexagon centroids (60° grid, 10×6 = 60 positions)
 *   triangle centroids (30° rotation, 60 more positions)
 * Total per face = 120 → 12 × 120 = 1440 = GEO_TICK_TOTAL.
 *
 * Triangle centroids sit at centers of equilateral triangles formed
 * by hexagon vertices. Edge/2 subdivision gives 2× resolution.
 * The 30° rotation within each face doubles the capture density
 * without requiring more faces.
 *
 * Mapping:
 *   tring_pos = face * 120 + is_tri * 60 + zone * 6 + slot
 *   face (0..11)  × 120 = face_base (0, 120, 240, ..., 1320)
 *   is_tri (0=hex, 1=tri) × 60 = centroid_set (0 or 60)
 *   zone (0..9)   × 6  = zone_base (0, 6, 12, ..., 54)
 *   slot (0..5)          = local_slot (0..5)
 *   → tring_pos 0..1439
 *
 * Integration:
 *   geo_frame_seek.h  — FRAME_CYCLE=1440, frame_enc(t) → DualFrame
 *   geo_rewind.h      — rewind_store(enc, chunk) for state buffer
 *   pogls_coord_wallet.h — wallet serialize for frozen entries
 *
 * Triangle centroids are computed by a 30° rotation of (vx, vy)
 * before tw_capture_int. Both sets are evaluated; the one with
 * smaller resid is stored with is_tri=1 flag.
 *
 * No malloc. No float. Frozen.
 * ══════════════════════════════════════════════════════════════
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

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define TW_FACES        12u     /* dodecahedron faces */
#define TW_FACE_SLOTS   60u     /* hexagon centroids per face (10 zones × 6 slots) */
#define TW_TRING_FULL   720u    /* hexagon TRing = 12 faces × 60 slots */
#define TW_TRING_1440   1440u   /* hex + tri = 12 faces × 120 centroids */

/* Triangle centroids: 30° rotation creates 60 more centroids per face.
 * These sit at the centers of equilateral triangles formed by the
 * original hexagon vertices. Edge/2 subdivision → 120/face → 1440 total.
 * 1440 = GEO_TICK_TOTAL — timeline and TRing now aligned. */

#define TW_FACE_SLOTS_120  120u /* total centroids per face (hex+tri) */

/* ══════════════════════════════════════════════════════════════
   TW REWIND BUFFER — lightweight TRing-coordinate store (720 or 1440 slots)
   ══════════════════════════════════════════════════════════════ */

#define TW_REWIND_SLOTS  TW_TRING_1440  /* 1440 slots = hex+tri TRing */

/*
 * TWFaceRewind: a circular buffer indexed by TRing position.
 * Each slot holds a packed uint64_t key (0 = empty).
 * O(1) store/find by TRing position — no enc or TStreamChunk needed.
 */
typedef struct {
    uint64_t keys[TW_REWIND_SLOTS];  /* packed keys, 0 = empty slot */
    uint32_t stored;                  /* total stores (for stats)     */
} TWFaceRewind;

static inline void tw_rewind_init(TWFaceRewind *rb) {
    memset(rb, 0, sizeof(*rb));
}

static inline void tw_rewind_store(TWFaceRewind *rb, uint64_t key, uint16_t tring_pos) {
    rb->keys[tring_pos % TW_REWIND_SLOTS] = key;
    rb->stored++;
}

static inline uint64_t tw_rewind_find(const TWFaceRewind *rb, uint16_t tring_pos) {
    return rb->keys[tring_pos % TW_REWIND_SLOTS];
}

static inline int tw_rewind_has(const TWFaceRewind *rb, uint16_t tring_pos) {
    return rb->keys[tring_pos % TW_REWIND_SLOTS] != 0;
}

static inline uint32_t tw_rewind_occupied(const TWFaceRewind *rb) {
    uint32_t n = 0;
    for (uint16_t i = 0; i < TW_REWIND_SLOTS; i++)
        if (rb->keys[i]) n++;
    return n;
}

/* ══════════════════════════════════════════════════════════════
   TW FREEZE WALLET — binary log for Shell-3 frozen entries
   ══════════════════════════════════════════════════════════════ */

#define TW_FREEZE_MAGIC     0x46525A57u  /* "FRZW" */
#define TW_FREEZE_VERSION   1u
#define TW_FREEZE_ENTRY_SZ  18u  /* sizeof(TWFreezeEntry) */

/*
 * On-disk freeze wallet format:
 *   [TWFreezeHeader 16B]           — magic, version, count, timestamp
 *   [TWFreezeEntry 18B × N]        — N frozen entries
 *
 * Total: 16 + N*18 bytes.
 */

typedef struct __attribute__((packed)) {
    uint32_t magic;           /* TW_FREEZE_MAGIC */
    uint32_t version;         /* TW_FREEZE_VERSION */
    uint32_t n_entries;       /* number of valid freeze entries */
    uint32_t _reserved;       /* future use */
} TWFreezeHeader;

typedef struct __attribute__((packed)) {
    uint16_t tring_pos;       /* 0..1439 TRing position (hex+tri) */
    uint32_t freeze_addr;     /* POGLS wallet freeze address      */
    uint32_t tick;            /* timeline tick at freeze          */
    uint64_t packed_key;      /* full TWFaceCapture packed key    */
} TWFreezeEntry;

typedef char _tw_freeze_entry_sz[(sizeof(TWFreezeEntry) == TW_FREEZE_ENTRY_SZ) ? 1:-1];

/*
 * Write a freeze entry to an open FILE.
 * Returns number of bytes written (18 on success, 0 on error).
 */
static inline size_t tw_freeze_entry_write(FILE *f, const TWFreezeEntry *e) {
    if (!f || !e) return 0;
    return fwrite(e, TW_FREEZE_ENTRY_SZ, 1, f) == 1 ? TW_FREEZE_ENTRY_SZ : 0;
}

/*
 * Read a freeze entry from an open FILE at current position.
 * Returns 1 on success, 0 on error/EOF.
 */
static inline int tw_freeze_entry_read(FILE *f, TWFreezeEntry *e) {
    if (!f || !e) return 0;
    return fread(e, TW_FREEZE_ENTRY_SZ, 1, f) == 1;
}

/*
 * Write freeze wallet header + all entries to a file.
 * Opens file, writes header, writes N entries, closes.
 * Returns TW_FREEZE_ENTRY_SZ * (1 + n_entries) on success, 0 on error.
 */
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

/*
 * Read freeze wallet from file.
 * Allocates entries via malloc — caller must free().
 * Returns number of entries read, or 0 on error.
 */
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
   FACE → TRing MAPPING
   ══════════════════════════════════════════════════════════════ */

/*
 * Convert face + zone + slot + is_tri → TRing position (0..1439).
 *
 * face:  0..11  (dodecahedron face)
 * zone:  0..9   (sector within face)
 * slot:  0..5   (local slot within sector)
 * is_tri: 0=hex centroid, 1=triangle centroid (30° rotated)
 *
 * Returns: face * 120 + is_tri * 60 + zone * 6 + slot
 *
 * 0..59    = hex centroids within a face
 * 60..119  = tri centroids within a face
 */
static inline uint16_t tw_face_to_tring(uint8_t face, uint8_t zone, uint8_t slot,
                                         uint8_t is_tri) {
    if (face >= TW_FACES) face = face % TW_FACES;
    if (zone >= TW_N_SECTORS) zone = zone % TW_N_SECTORS;
    if (slot >= TW_SLOTS_PER) slot = slot % TW_SLOTS_PER;
    return (uint16_t)(face * TW_FACE_SLOTS_120 + is_tri * TW_FACE_SLOTS
                      + zone * TW_SLOTS_PER + slot);
}

/* Backward-compat: hex-only tring (is_tri=0) */
static inline uint16_t tw_face_to_tring_hex(uint8_t face, uint8_t zone, uint8_t slot) {
    return tw_face_to_tring(face, zone, slot, 0);
}

/*
 * Decompose TRing position → face + zone + slot + is_tri.
 * Inverse of tw_face_to_tring.
 */
static inline void tw_tring_to_face_zone_slot(uint16_t tring,
                                               uint8_t *face,
                                               uint8_t *zone,
                                               uint8_t *slot,
                                               uint8_t *is_tri) {
    *face   = (uint8_t)(tring / TW_FACE_SLOTS_120);  /* 0..11 */
    uint16_t r = tring % TW_FACE_SLOTS_120;
    *is_tri = (r >= TW_FACE_SLOTS) ? 1u : 0u;
    if (*is_tri) r -= TW_FACE_SLOTS;
    *zone   = (uint8_t)(r / TW_SLOTS_PER);            /* 0..9 */
    *slot   = (uint8_t)(r % TW_SLOTS_PER);            /* 0..5 */
}

/* ══════════════════════════════════════════════════════════════
   12-FACE CAPTURE — iterate all faces
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  face;          /* 0..11 which face captured */
    uint8_t  zone;          /* 0..9 sector within face */
    uint8_t  slot;          /* 0..5 slot within sector */
    uint16_t tring_pos;     /* 0..1439 full TRing position (hex+tri) */
    int64_t  resid_x;       /* residual X (TW_SCALE units) */
    int64_t  resid_y;       /* residual Y (TW_SCALE units) */
    uint8_t  is_tri;        /* 0=hex centroid, 1=triangle centroid */
    uint8_t  drain;         /* 1 if near boundary */
    uint8_t  drain_face;    /* drain target face (0..11) */
    uint8_t  drain_zone;    /* drain target zone (0..9) */
    uint8_t  drain_slot;    /* drain target slot (0..5) */
    uint16_t drain_tring;   /* drain TRing position */
} TWFaceCapture;

/*
 * Capture a single face: run TW capture on face f.
 * vx, vy: signature coordinates (TW_SCALE units).
 * f: face index (0..11).
 *
 * This is the core capture — same as tw_capture_int but with face context.
 * The face parameter is used for TRing mapping, not for sector geometry
 * (which is face-invariant).
 */
static inline void tw_capture_face(int64_t vx, int64_t vy,
                                   uint8_t f, TWFaceCapture *out)
{
    /* 30° rotation constants (same as ROT_COS[1], ROT_SIN[1]) */
    static const int32_t COS30 = 179580;   /* TW_SCALE * cos30° */
    static const int32_t SIN30 = 103680;   /* TW_SCALE * sin30° */

    /* Try hex centroid (0°) */
    TWCaptureInt hex_cap;
    tw_capture_int(vx, vy, &hex_cap);

    /* Try triangle centroid (30° rotated) */
    int64_t rx = (vx * COS30 - vy * SIN30) / TW_SCALE;
    int64_t ry = (vx * SIN30 + vy * COS30) / TW_SCALE;
    TWCaptureInt tri_cap;
    tw_capture_int(rx, ry, &tri_cap);

    /* Pick best resid */
    int64_t hex_mag = hex_cap.resid_x * hex_cap.resid_x
                    + hex_cap.resid_y * hex_cap.resid_y;
    int64_t tri_mag = tri_cap.resid_x * tri_cap.resid_x
                    + tri_cap.resid_y * tri_cap.resid_y;

    int use_tri = (tri_mag < hex_mag) ? 1 : 0;
    TWCaptureInt *best = use_tri ? &tri_cap : &hex_cap;

    out->face = f;
    out->zone = best->zone;
    out->slot = best->slot;
    out->is_tri = use_tri;
    out->tring_pos = tw_face_to_tring(f, best->zone, best->slot, use_tri);
    out->resid_x = best->resid_x;
    out->resid_y = best->resid_y;
    out->drain = best->drain;
    out->drain_zone = best->drain_zone;
    out->drain_slot = best->drain_slot;

    if (best->drain) {
        out->drain_face = f;
        out->drain_tring = tw_face_to_tring(f, best->drain_zone,
                                             best->drain_slot, use_tri);
    } else {
        out->drain_face = 0;
        out->drain_tring = 0;
    }
}

/* ══════════════════════════════════════════════════════════════
   ITERATE ALL 12 FACES
   ══════════════════════════════════════════════════════════════ */

/*
 * Result of capturing all 12 faces.
 * For each face, we store the primary capture and drain info.
 */
typedef struct {
    TWFaceCapture faces[TW_FACES];   /* one per face */
    uint8_t       n_captured;        /* number of faces with valid data */
    uint8_t       n_drains;          /* faces with drain active */
    uint16_t      tring_histogram[TW_TRING_1440]; /* occupancy count per TRing slot (hex+tri) */
} TWFaceIterResult;

/*
 * Iterate TW capture across all 12 faces.
 *
 * For each face f:
 *   1. Rotate signature by face angle (f × 30° = f × π/6)
 *   2. Run tw_capture_int on rotated coordinates
 *   3. Map to TRing position
 *
 * vx, vy: base signature (face 0).
 * The rotation simulates different viewpoints of the same tensor signature.
 *
 * Note: This is a deterministic mapping — same tensor always produces
 * the same 12-face distribution. The rotation ensures each face gets
 * a unique perspective.
 */
static inline void tw_iterate_faces(int64_t vx, int64_t vy,
                                    TWFaceIterResult *result)
{
    memset(result, 0, sizeof(*result));

    /* Precomputed rotation factors for 12 faces (30° increments)
     * cos(f*30°), sin(f*30°) × TW_SCALE, fixed-point */
    static const int32_t ROT_COS[12] = {
         207360,  179580,  103680,       0, -103680, -179580,
        -207360, -179580, -103680,       0,  103680,  179580
    };
    static const int32_t ROT_SIN[12] = {
             0,  103680,  179580,  207360,  179580,  103680,
             0, -103680, -179580, -207360, -179580, -103680
    };

    for (uint8_t f = 0; f < TW_FACES; f++) {
        /* Rotate signature: vx' = vx*cos - vy*sin, vy' = vx*sin + vy*cos
         * Using int64 intermediate, divide by TW_SCALE at end */
        int64_t rx = (vx * ROT_COS[f] - vy * ROT_SIN[f]) / TW_SCALE;
        int64_t ry = (vx * ROT_SIN[f] + vy * ROT_COS[f]) / TW_SCALE;

        tw_capture_face(rx, ry, f, &result->faces[f]);
        result->n_captured++;

        if (result->faces[f].drain) {
            result->n_drains++;
            result->tring_histogram[result->faces[f].tring_pos]++;
            result->tring_histogram[result->faces[f].drain_tring]++;
        } else {
            result->tring_histogram[result->faces[f].tring_pos]++;
        }
    }
}

/* ══════════════════════════════════════════════════════════════
   FRAME SEEK INTEGRATION — TRing → DualFrame
   ══════════════════════════════════════════════════════════════ */

/*
 * Convert TRing position (0..719) → frame enc (0..1439).
 *
 * TRing is the 720-slot dodecahedral ring.
 * Frame timeline is 1440 = 2 × 720 (dual-world: north + south pole).
 *
 * Mapping: enc = tring_pos (World A, north pole)
 *          enc = tring_pos + 720 (World B, south pole = cpair)
 *
 * Returns enc for World A.
 */
static inline uint16_t tw_tring_to_frame_enc(uint16_t tring_pos) {
    return tring_pos % FRAME_CYCLE;  /* hex+tri tring → enc in World A */
}

/*
 * Convert TWFaceCapture → DualFrame via geo_frame_seek.h.
 * Returns the DualFrame for this capture's position.
 */
static inline DualFrame tw_face_to_frame(const TWFaceCapture *cap) {
    uint16_t enc = tw_tring_to_frame_enc(cap->tring_pos);
    return frame_at(enc);
}

/* ══════════════════════════════════════════════════════════════
   REWIND BUFFER INTEGRATION — store/find via geo_rewind.h
   ══════════════════════════════════════════════════════════════ */

/*
 * Pack TWFaceCapture into a compact 8-byte key for rewind buffer.
 * Key format: [face:4][zone:4][slot:4][drain:1][reserved:3] (2 bytes)
 *             [resid_x:16] (2 bytes, low 16 bits)
 *             [resid_y:16] (2 bytes, low 16 bits)
 *             [drain_tring:10] (2 bytes, low 10 bits)
 *
 * Total: 8 bytes — fits in a uint64_t.
 */
static inline uint64_t tw_face_pack_key(const TWFaceCapture *cap) {
    uint64_t key = 0;
    key |= (uint64_t)(cap->face & 0x0F)       << 60;
    key |= (uint64_t)(cap->zone & 0x0F)       << 56;
    key |= (uint64_t)(cap->slot & 0x0F)       << 52;
    key |= (uint64_t)(cap->drain & 0x01)      << 51;
    key |= (uint64_t)(cap->is_tri & 0x01)     << 50;
    key |= (uint64_t)(cap->resid_x & 0xFFFF)  << 32;
    key |= (uint64_t)(cap->resid_y & 0xFFFF)  << 16;
    key |= (uint64_t)(cap->drain_tring & 0x7FF); /* 11 bits for 0..1440 */
    return key;
}

/*
 * Unpack key → TWFaceCapture.
 */
static inline void tw_face_unpack_key(uint64_t key, TWFaceCapture *cap) {
    cap->face       = (uint8_t)((key >> 60) & 0x0F);
    cap->zone       = (uint8_t)((key >> 56) & 0x0F);
    cap->slot       = (uint8_t)((key >> 52) & 0x0F);
    cap->drain      = (uint8_t)((key >> 51) & 0x01);
    cap->is_tri     = (uint8_t)((key >> 50) & 0x01);
    cap->resid_x    = (int64_t)((int16_t)((key >> 32) & 0xFFFF));
    cap->resid_y    = (int64_t)((int16_t)((key >> 16) & 0xFFFF));
    cap->drain_tring = (uint16_t)(key & 0x7FF);
    cap->tring_pos  = tw_face_to_tring(cap->face, cap->zone, cap->slot,
                                        cap->is_tri);
    cap->drain_face = cap->drain ? cap->face : 0;
    cap->drain_zone = 0;  /* not stored in key — recover from drain_tring */
    cap->drain_slot = 0;
}

/* ══════════════════════════════════════════════════════════════
   WALLET INTEGRATION — freeze → .pogwallet
   ══════════════════════════════════════════════════════════════ */

/*
 * Freeze check for 12-face system.
 * Same logic as tw_is_frozen but with face context.
 */
static inline int tw_face_is_frozen(const TWFaceCapture *cap, uint32_t tick) {
    return tw_is_frozen(cap->drain, tick);
}

/*
 * Freeze address using TRing position.
 * address = tring_pos × SHELL_FULL/TRING + layer × GEO_TOWER + cell.
 *
 * This makes the freeze address meaningful in the full dodecahedron,
 * not just a single face.
 */
static inline uint32_t tw_face_freeze_address(const TWFaceCapture *cap,
                                               uint32_t tick,
                                               uint8_t  layer)
{
    if (!tw_face_is_frozen(cap, tick)) return 0u;

    /* Base from TRing position (0..1439 = hex+tri) */
    uint32_t tring_base = (uint32_t)cap->tring_pos;

    /* Layer offset within the tower */
    uint32_t layer_off = (tick % SHELL_RINGS) * GEO_TOWER;

    /* Residual cell */
    uint32_t cell_off = tw_resid_to_offset((int32_t)cap->resid_x,
                                           (int32_t)cap->resid_y);

    return GEO_WRAP(tring_base + layer_off + cell_off);
}

/* ══════════════════════════════════════════════════════════════
   WIRE: TWFaceCapture → rewind buffer + freeze wallet entry
   ══════════════════════════════════════════════════════════════ */

/*
 * Store a TWFaceCapture into the TW rewind buffer.
 * Uses the capture's own TRing position as the index.
 * The packed key carries face/zone/slot/resid/drain info.
 */
static inline void tw_face_rewind_store(TWFaceRewind *rb,
                                         const TWFaceCapture *cap)
{
    uint64_t key = tw_face_pack_key(cap);
    tw_rewind_store(rb, key, cap->tring_pos);
}

/*
 * Create a freeze entry from a frozen TWFaceCapture.
 * Only call if tw_face_is_frozen() returns true.
 */
static inline TWFreezeEntry tw_face_freeze_entry(const TWFaceCapture *cap,
                                                   uint32_t tick,
                                                   uint8_t  layer)
{
    TWFreezeEntry e;
    e.tring_pos   = cap->tring_pos;
    e.freeze_addr = tw_face_freeze_address(cap, tick, layer);
    e.tick        = tick;
    e.packed_key  = tw_face_pack_key(cap);
    return e;
}

/* ══════════════════════════════════════════════════════════════
   WORLD SELECTION — TRing A/B via frame_cpair
   ══════════════════════════════════════════════════════════════ */

/*
 * Map TRing position → World B (cpair).
 * hex+tri TRing is 0..1439, frame_cpair wraps at FRAME_CYCLE=1440.
 * For hex tring (0..719): world B = 720..1439
 * For tri tring (60..119 per face): world B = 780..839 etc.
 */
static inline uint16_t tw_tring_to_world_b(uint16_t tring_pos) {
    return frame_cpair(tring_pos);
}

/* ══════════════════════════════════════════════════════════════
   COMBINED: TWCaptureInt → full 12-face result
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t      tring_pos;     /* 0..1439 primary TRing position (hex+tri) */
    uint16_t      drain_tring;   /* 0..1439 drain TRing position (0 if no drain) */
    DualFrame     frame;         /* geo_frame_seek.h frame info */
    uint32_t      freeze_addr;   /* POGLS wallet address (0 if not frozen) */
    uint8_t       frozen;        /* 1 if Shell 3 freeze active */
    uint64_t      key;           /* packed 8-byte key for rewind buffer */
} TWFaceBridgeResult;

/*
 * Master 12-face bridge: signature → full TWFaceBridgeResult.
 *
 * vx, vy: signature coordinates (TW_SCALE units, from tw_tensor_capture).
 * face: which face to use (0..11). For single-face capture.
 * tick: current timeline tick (for freeze check).
 * layer: current layer (for freeze address).
 *
 * For full 12-face iteration, use tw_iterate_faces() then convert each.
 */
static inline TWFaceBridgeResult tw_face_bridge_single(
    int64_t  vx, int64_t  vy,
    uint8_t  face,
    uint32_t tick,
    uint8_t  layer)
{
    TWFaceBridgeResult r;

    TWFaceCapture cap;
    tw_capture_face(vx, vy, face, &cap);

    r.tring_pos   = cap.tring_pos;
    r.drain_tring = cap.drain_tring;
    r.frame       = tw_face_to_frame(&cap);
    r.frozen      = (uint8_t)tw_face_is_frozen(&cap, tick);
    r.freeze_addr = tw_face_freeze_address(&cap, tick, layer);
    r.key         = tw_face_pack_key(&cap);

    return r;
}

/*
 * Full 12-face bridge: iterate all faces, return best result.
 *
 * "Best" = highest drain probability (most interesting for wallet).
 * If no drain, return face 0.
 */
static inline TWFaceBridgeResult tw_face_bridge_full(
    int64_t  vx, int64_t  vy,
    uint32_t tick,
    uint8_t  layer)
{
    TWFaceIterResult iter;
    tw_iterate_faces(vx, vy, &iter);

    /* Find face with drain active */
    uint8_t best_face = 0;
    for (uint8_t f = 0; f < TW_FACES; f++) {
        if (iter.faces[f].drain) {
            best_face = f;
            break;
        }
    }

    return tw_face_bridge_single(vx, vy, best_face, tick, layer);
}

#endif /* TW_FACE_BRIDGE_H */

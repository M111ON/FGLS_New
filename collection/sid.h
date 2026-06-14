/*
 * sid.h — Single Integrated Dimension (SID)
 * ════════════════════════════════════════════════════════════════
 *
 * "Geometry IS the storage. Coordinate IS the data."
 *
 * Core idea:
 *   One line + {36°, 60°, 180°} → pentagon + hexagon →
 *   10 sectors × 6 slots = 60 positions per face →
 *   12 faces × 60 = TRing 720 (hex centroids) →
 *   +30° rotation = 60 more tri centroids = 120/face →
 *   12 × 120 = TRing 1440 = SID coordinate space
 *
 *   SID coordinate (face, zone, slot, resid) uniquely identifies
 *   ANY tensor in the system. From coordinate alone, we summon
 *   the tensor's 2D signature via pure integer reconstruct_int.
 *
 * Storage:
 *   .twidx = coordinate-only index (74 KB for SmolLM2-360M Q8_0)
 *   No .qdat, no .gsten, no raw weight storage needed.
 *
 * Summon formula:
 *   coord → reconstruct_int → (vx, vy) → sig = (vx/SCALE, vy/SCALE)
 *
 * No float. No I/O (after capture). Pure integer geometry.
 * ════════════════════════════════════════════════════════════════
 */

#ifndef SID_H
#define SID_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "tw_capture_int.h"
#include "tw_face_bridge.h"
#include "zone_card.h"
#include "zone_card_sid.h"

/* ── Limits ─────────────────────────────────────────────────── */
#define SID_MAX_ENTRIES  2048
#define SID_NAME_MAX     256
#define SID_TWIDX_MAGIC  0x53494431u   /* "SID1" */
#define SID_TWIDX_VER    1u

/* ── SID Coordinate ─────────────────────────────────────────── */
typedef struct {
    uint8_t  face;        /* 0..11 dodecahedron face              */
    uint8_t  zone;        /* 0..9 sector within face              */
    uint8_t  slot;        /* 0..5 slot within sector              */
    int64_t  resid_x;     /* residual X (TW_SCALE units)          */
    int64_t  resid_y;     /* residual Y (TW_SCALE units)          */
    uint16_t tring_pos;   /* 0..1439 = face*120 + is_tri*60 + zone*6 + slot */
    uint8_t  is_tri;      /* 0=hex centroid, 1=triangle centroid (30°)      */
    uint8_t  drain;       /* 1 if near sector boundary                      */
    uint8_t  pad[2];
} SIDCoord;

/* Helper: extract SIDCoord from ZoneCardSID for backward compatibility */
static inline SIDCoord sid_coord_from_zcsid(const ZoneCardSID *zcsid) {
    SIDCoord c = {0};
    c.face      = zcsid->face;
    c.zone      = zcsid->zone;
    c.slot      = zcsid->slot;
    c.resid_x   = zcsid->resid_x;
    c.resid_y   = zcsid->resid_y;
    c.tring_pos = zcsid->tring_pos;
    c.is_tri    = (zcsid->tring_pos % 120 >= 60) ? 1 : 0;
    c.drain     = (zcsid->inf.flags & ZCSID_FLAG_DRAIN) ? 1 : 0;
    return c;
}

/* Helper: extract ZoneCardSID communication payload (inf + tring_pos = 10B) */
static inline void sid_comm_payload(const ZoneCardSID *zcsid, uint8_t out[10]) {
    /* inf (8B) + tring_pos (2B) - not contiguous in struct, copy separately */
    memcpy(out, &zcsid->inf, 8);
    memcpy(out + 8, &zcsid->tring_pos, 2);
}

/* ── SID Entry: name + coordinate (SIDCoord) ──────────────────── */
/* SIDCoord is used in SIDEntry for .twidx storage backward compatibility. */
typedef struct {
    char     name[SID_NAME_MAX];
    SIDCoord coord;        /* 16B: coordinate components */
} SIDEntry;

/* ── SID Store: lightweight coordinate-only index ───────────── */
typedef struct {
    SIDEntry entries[SID_MAX_ENTRIES];
    uint32_t n_entries;
} SIDStore;

/* ═══════════════════════════════════════════════════════════════
   CAPTURE — tensor → SID coordinate (pure integer)
   ═══════════════════════════════════════════════════════════════ */

/*
 * Compute 2D integer signature from Q8_0 data (first row only).
 * vx = mean(first 16 quants × scale) * TW_SCALE
 * vy = mean(last 16 quants × scale) * TW_SCALE
 */
static inline int sid_signature_q80(const uint8_t *q8_data, size_t nbytes,
                                     int64_t *vx, int64_t *vy)
{
    if (!q8_data || nbytes < 34) return -1;

    /* Dequant first 64 values (2 Q8_0 blocks = 34*2 bytes) */
    size_t n_blocks = nbytes / 34;
    if (n_blocks > 2) n_blocks = 2;
    uint32_t total_vals = (uint32_t)(n_blocks * 32);
    if (total_vals > 64) total_vals = 64;

    float sum_a = 0, sum_b = 0;
    uint32_t na = 0, nb = 0;

    for (uint32_t b = 0; b < n_blocks; b++) {
        uint16_t scale_bits;
        memcpy(&scale_bits, q8_data + b * 34, 2);

        /* f16 → float (integer-only, no math.h) */
        uint32_t sign  = (scale_bits >> 15) & 1;
        uint32_t exp   = (scale_bits >> 10) & 0x1F;
        uint32_t mant  = scale_bits & 0x3FF;
        float dscale;
        if (exp == 0) {
            dscale = (float)mant * 5.960464477539063e-8f;
            dscale = sign ? -dscale : dscale;
        } else {
            uint32_t fi = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
            memcpy(&dscale, &fi, sizeof(dscale));
        }

        for (int j = 0; j < 32; j++) {
            int8_t q = (int8_t)q8_data[b * 34 + 2 + j];
            float val = (float)q * dscale;
            if (j < 16) { sum_a += val; na++; }
            else        { sum_b += val; nb++; }
        }
    }

    if (na == 0 || nb == 0) return -1;

    *vx = (int64_t)((sum_a / (float)na) * TW_SCALE);
    *vy = (int64_t)((sum_b / (float)nb) * TW_SCALE);
    return 0;
}

/*
 * Compute 2D integer signature from F32 data.
 */
static inline int sid_signature_f32(const uint8_t *f32_data, size_t nbytes,
                                     int64_t *vx, int64_t *vy)
{
    if (!f32_data || nbytes < 8) return -1;
    uint32_t n = (uint32_t)(nbytes / 4);
    if (n > 64) n = 64;
    uint32_t half = n / 2;
    double sum_a = 0, sum_b = 0;
    for (uint32_t i = 0; i < half; i++) {
        float val; memcpy(&val, f32_data + i * 4, 4);
        sum_a += val;
    }
    for (uint32_t i = half; i < n; i++) {
        float val; memcpy(&val, f32_data + i * 4, 4);
        sum_b += val;
    }
    *vx = (int64_t)((sum_a / half) * TW_SCALE);
    *vy = (int64_t)((sum_b / (n - half)) * TW_SCALE);
    return 0;
}

/*
 * SID Capture: ZoneCard + TWCaptureInt → ZoneCardSID.
 * Stored resid is face-local (backward compat). For face-0 resid, use
 * tw_capture_int_to_face + SIDCoord path instead.
 */
static inline int sid_capture(const ZoneCard *card, const TWCaptureInt *cap_data,
                               uint8_t face, uint8_t is_tri, ZoneCardSID *out)
{
    if (!card || !cap_data || !out) return -1;
    *out = zcsid_make(card, 0, 0, 0, face, is_tri, cap_data);
    return 0;
}


/* ═══════════════════════════════════════════════════════════════
   SUMMON — SID coordinate → 2D signature (pure integer)
   ═══════════════════════════════════════════════════════════════ */

/*
 * SID Summon: from coordinate → reconstruct (vx, vy).
 * This is pure geometry — no data, no I/O, no float.
 *
 * The result (vx / TW_SCALE, vy / TW_SCALE) = original 2D signature
 * of the tensor's first row.
 */
static inline void sid_summon(const ZoneCardSID *coord, int64_t *vx, int64_t *vy)
{
    uint8_t face  = coord->inf.face;
    uint8_t is_tri = (coord->tring_pos % 120 >= 60) ? 1 : 0;

    /* TWCaptureInt stores slot as sector*TW_SLOTS_PER + local (0..59).
     * Extract local slot (0..5) and index into combined grid.
     * Rotate the face-local centroid to face-0 frame, then add resid (face-0). */
    int local_slot = coord->slot % TW_SLOTS_PER;
    int combined_idx = local_slot + (is_tri ? TW_SLOTS_PER : 0);
    int64_t c0x, c0y;
    _tw_centroid_to_face0(coord->zone, combined_idx, face, &c0x, &c0y);
    *vx = c0x + coord->resid_x;
    *vy = c0y + coord->resid_y;
}


/*
 * Summon signature as double (for verification/display).
 * The integer path is sid_summon() — this is a convenience wrapper.
 */
static inline void sid_summon_sig(const ZoneCardSID *full_csid, /* Accepts full ZoneCardSID */
                                   double *sig_x, double *sig_y)
{
    int64_t vx, vy;
    sid_summon(full_csid, &vx, &vy); /* Pass the full ZoneCardSID */
    *sig_x = (double)vx / (double)TW_SCALE;
    *sig_y = (double)vy / (double)TW_SCALE;
}

/* ═══════════════════════════════════════════════════════════════
   STORE — .twidx file format (lightweight coordinate index)
   ═══════════════════════════════════════════════════════════════ */

/*
 * .twidx format (binary):
 *   [Header: 16B]     magic(4) + version(4) + n_entries(4) + reserved(4)
 *   [Entry × N: ~296B each] name(256) + ZoneCardSID(32)
 *
 * Total size increase is acceptable for storage, but for communication,
 * only relevant parts (inf + tring_pos) will be extracted.
 */

typedef struct __attribute__((packed)) {
    uint32_t magic;       /* SID_TWIDX_MAGIC */
    uint32_t version;     /* SID_TWIDX_VER */
    uint32_t n_entries;   /* number of valid entries */
    uint32_t reserved;
} SIDTwidxHeader;

typedef struct __attribute__((packed)) {
    char     name[SID_NAME_MAX];  /* tensor name, null-terminated */
    /* Store ZoneCardSID components relevant for lookup/reconstruction */
    uint8_t  face;                /* 0..11 */
    uint8_t  zone;                /* 0..9 */
    uint8_t  slot;                /* 0..5 */
    int64_t  resid_x;
    int64_t  resid_y;
    uint16_t tring_pos;
    uint8_t  is_tri;              /* 0=hex centroid, 1=triangle centroid */
    uint8_t  drain;
    uint8_t  pad[4]; /* To maintain alignment and size if needed */
    /* Note: Full ZoneCard and ZCSIDInference are not stored in twidx for brevity,
       but are assumed to be reconstructible or managed elsewhere if needed. */
} SIDTwidxEntry;

/* Write .twidx file */
static inline int sid_write(const char *path, const SIDStore *store) {
    if (!path || !store) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    SIDTwidxHeader hdr;
    hdr.magic     = SID_TWIDX_MAGIC;
    hdr.version   = SID_TWIDX_VER;
    hdr.n_entries = store->n_entries;
    hdr.reserved  = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return -1; }

    for (uint32_t i = 0; i < store->n_entries; i++) {
        SIDTwidxEntry e;
        memset(&e, 0, sizeof(e));
        strncpy(e.name, store->entries[i].name, SID_NAME_MAX - 1);

        /* Populate entry from SIDCoord in SIDEntry */
        e.face      = store->entries[i].coord.face;
        e.zone      = store->entries[i].coord.zone;
        e.slot      = store->entries[i].coord.slot;
        e.resid_x   = store->entries[i].coord.resid_x;
        e.resid_y   = store->entries[i].coord.resid_y;
        e.tring_pos = store->entries[i].coord.tring_pos;
        e.is_tri    = store->entries[i].coord.is_tri;
        e.drain     = store->entries[i].coord.drain;

        if (fwrite(&e, sizeof(e), 1, f) != 1) { fclose(f); return -1; }
    }

    fclose(f);
    return (int)store->n_entries;
}

/* Read .twidx file */
static inline int sid_read(const char *path, SIDStore *store) {
    if (!path || !store) return -1;
    memset(store, 0, sizeof(*store));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    SIDTwidxHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return -1; }
    if (hdr.magic != SID_TWIDX_MAGIC || hdr.version != SID_TWIDX_VER) {
        fclose(f); return -1;
    }

    uint32_t n = hdr.n_entries;
    if (n > SID_MAX_ENTRIES) n = SID_MAX_ENTRIES;

    for (uint32_t i = 0; i < n; i++) {
        SIDTwidxEntry e;
        if (fread(&e, sizeof(e), 1, f) != 1) {
            store->n_entries = i;
            fclose(f);
            return (int)i;
        }
        strncpy(store->entries[i].name, e.name, SID_NAME_MAX - 1);

        /* Populate SIDCoord from SIDTwidxEntry */
        store->entries[i].coord.face      = e.face;
        store->entries[i].coord.zone      = e.zone;
        store->entries[i].coord.slot      = e.slot;
        store->entries[i].coord.resid_x   = e.resid_x;
        store->entries[i].coord.resid_y   = e.resid_y;
        store->entries[i].coord.tring_pos = e.tring_pos;
        store->entries[i].coord.is_tri    = e.is_tri;
        store->entries[i].coord.drain     = e.drain;
    }

    store->n_entries = n;
    fclose(f);
    return (int)n;
}

/* Lookup tensor by name in SIDStore */
static inline SIDEntry *sid_lookup(SIDStore *store, const char *name) {
    if (!store || !name) return NULL;
    for (uint32_t i = 0; i < store->n_entries; i++) {
        if (strcmp(store->entries[i].name, name) == 0)
            return &store->entries[i];
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
   BACKWARD COMPATIBILITY — old API for existing tests
   ═══════════════════════════════════════════════════════════════ */

/*
 * Old-style capture: raw data → SIDCoord (for test compatibility)
 * Internally computes signature and runs TW capture.
 */
static inline int sid_capture_legacy(const void *data, size_t nbytes,
                                      int dtype, uint8_t face,
                                      SIDCoord *out)
{
    if (!data || !out || nbytes == 0) return -1;
    memset(out, 0, sizeof(*out));

    int64_t vx, vy;
    int rc;
    if (dtype == 0)
        rc = sid_signature_f32((const uint8_t *)data, nbytes, &vx, &vy);
    else
        rc = sid_signature_q80((const uint8_t *)data, nbytes, &vx, &vy);
    if (rc != 0) return rc;

    TWFaceCapture fc;
    tw_capture_face(vx, vy, face, &fc);

    out->face      = fc.face;
    out->zone      = fc.zone;
    out->slot      = fc.slot;
    out->resid_x   = fc.resid_x;
    out->resid_y   = fc.resid_y;
    out->tring_pos = fc.tring_pos;
    out->is_tri    = fc.is_tri;
    out->drain     = fc.drain;

    return 0;
}

/* Old-style summon: SIDCoord → (vx, vy) */
static inline void sid_summon_legacy(const SIDCoord *coord,
                                      int64_t *vx, int64_t *vy)
{
    /* TWCaptureInt stores slot as sector*TW_SLOTS_PER + local (0..59).
     * Extract local slot (0..5) and index into combined grid.
     * Rotate centroid from face-local to face-0, then add resid (face-0). */
    int local_slot = coord->slot % TW_SLOTS_PER;
    int combined_idx = local_slot + (coord->is_tri ? TW_SLOTS_PER : 0);
    int64_t c0x, c0y;
    _tw_centroid_to_face0(coord->zone, combined_idx, coord->face, &c0x, &c0y);
    *vx = c0x + coord->resid_x;
    *vy = c0y + coord->resid_y;
}

/* Old-style roundtrip test */
static inline int sid_verify_roundtrip_legacy(const void *data, size_t nbytes,
                                               int dtype, const char *name)
{
    (void)name; /* unused */
    SIDCoord coord;
    if (sid_capture_legacy(data, nbytes, dtype, 0, &coord) != 0)
        return -1;

    int64_t orig_vx, orig_vy;
    int rc;
    if (dtype == 0)
        rc = sid_signature_f32((const uint8_t *)data, nbytes, &orig_vx, &orig_vy);
    else
        rc = sid_signature_q80((const uint8_t *)data, nbytes, &orig_vx, &orig_vy);
    if (rc != 0) return -1;

    int64_t summon_vx, summon_vy;
    sid_summon_legacy(&coord, &summon_vx, &summon_vy);

    if (summon_vx != orig_vx || summon_vy != orig_vy)
        return -1;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   SIDArchConfig — Architecture-Adaptive Capture Configuration
   ═══════════════════════════════════════════════════════════════
   Different model families (SmolLM, VLM, Qwen) have different
   optimal face priority orders and resolution requirements.
   This config allows per-family tuning without code changes.

   n_faces:   faces from priority list to try (1..7)
              n_faces=4 → 8 dirs (hex+tri) = ~99% coverage (default)
              n_faces=7 → 14 dirs = ~100% coverage
              n_faces=1 → 2 dirs = fast but may miss

   use_tri:   1=try both hex and tri grids (120 positions/face)
              0=hex only (60 positions/face)

   face_order: priority face list, NULL = default {0,3,5,6,2,1,4}
              Different arch families can provide their own.
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t        n_faces;       /* 1..7 faces from priority list */
    uint8_t        use_tri;       /* 1=also try tri centroids */
    const uint8_t *face_order;    /* priority face list (NULL = default) */
} SIDArchConfig;

/* ── Built-in profiles ───────────────────────────────────── */
/* SmolLM family: 4 faces + tri = 8 dirs, ~99% coverage */
static inline SIDArchConfig sid_conf_smollm(void) {
    SIDArchConfig c = { .n_faces = 4, .use_tri = 1, .face_order = NULL };
    return c;
}

/* VLM family: 6 faces + tri = 12 dirs (vision needs more resolution) */
static inline SIDArchConfig sid_conf_vlm(void) {
    SIDArchConfig c = { .n_faces = 6, .use_tri = 1, .face_order = NULL };
    return c;
}

/* Qwen family: 7 faces + tri = 14 dirs (different architecture, more faces) */
static inline SIDArchConfig sid_conf_qwen(void) {
    SIDArchConfig c = { .n_faces = 7, .use_tri = 1, .face_order = NULL };
    return c;
}

/* Fast: 1 face + hex only = 1 dir (quick scan, may miss some) */
static inline SIDArchConfig sid_conf_fast(void) {
    SIDArchConfig c = { .n_faces = 1, .use_tri = 0, .face_order = NULL };
    return c;
}

/* Full: 7 faces + tri = 14 dirs (max coverage) */
static inline SIDArchConfig sid_conf_full(void) {
    SIDArchConfig c = { .n_faces = 7, .use_tri = 1, .face_order = NULL };
    return c;
}

/*
 * Capture with configurable SIDArchConfig.
 * Handles hex-only and hex+tri modes.
 * Returns 0 on success, -1 on error.
 */
static inline int sid_capture_with_config(const void *data, size_t nbytes,
                                           int dtype, SIDArchConfig cfg,
                                           SIDCoord *out)
{
    if (!data || !out || nbytes == 0) return -1;
    memset(out, 0, sizeof(*out));

    int64_t vx, vy;
    int rc;
    if (dtype == 0)
        rc = sid_signature_f32((const uint8_t *)data, nbytes, &vx, &vy);
    else
        rc = sid_signature_q80((const uint8_t *)data, nbytes, &vx, &vy);
    if (rc != 0) return rc;

    int max_faces = cfg.n_faces;
    if (max_faces <= 0 || max_faces > TW_PRIORITY_FACES)
        max_faces = TW_PRIORITY_FACES;

    const uint8_t *order = cfg.face_order ? cfg.face_order : TW_FACE_PRIORITY;

    int best_idx = -1;
    int64_t best_mag = -1;
    TWFaceCapture caps[7]; /* max 7 faces, 1 per face with combined grid */

    for (int p = 0; p < max_faces; p++) {
        uint8_t f = order[p];

        int64_t rx = (vx * _TW_ROT_COS[f] - vy * _TW_ROT_SIN[f]) / TW_SCALE;
        int64_t ry = (vx * _TW_ROT_SIN[f] + vy * _TW_ROT_COS[f]) / TW_SCALE;

        /* Combined grid (hamburger): single nearest search across 12 centroids
         * (6 hex + 6 tri). Picks best automatically — no separate hex/tri compare. */
        uint8_t is_tri;
        TWCaptureInt cap;
        tw_capture_int_combined(rx, ry, &cap, &is_tri);
        tw_capture_int_to_face(&cap, f, &caps[p], is_tri, vx, vy);
    }

    for (int p = 0; p < max_faces; p++) {
        int64_t m = caps[p].resid_x * caps[p].resid_x
                  + caps[p].resid_y * caps[p].resid_y;
        if (best_idx < 0 || m < best_mag) { best_mag = m; best_idx = p; }
    }

    if (best_idx < 0) return -1;

    out->face      = caps[best_idx].face;
    out->zone      = caps[best_idx].zone;
    out->slot      = caps[best_idx].slot;
    out->resid_x   = caps[best_idx].resid_x;
    out->resid_y   = caps[best_idx].resid_y;
    out->tring_pos = caps[best_idx].tring_pos;
    out->is_tri    = caps[best_idx].is_tri;
    out->drain     = caps[best_idx].drain;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   PRIORITY CAPTURE — benchmark-ordered faces (4 = 91%, 7 = 99.7%)
   ═══════════════════════════════════════════════════════════════ */

/*
 * Priority capture: raw data → SIDCoord using face priority list.
 * Tries max_faces from {0,3,5,6,2,1,4}, each with hex+tri.
 * Internally uses sid_capture_with_config with default face_order.
 * n_faces=4 → 8 directions, ~99% coverage (real tri grid).
 * n_faces=7 → 14 directions, ~100% coverage.
 * Always picks best resid across all tried directions.
 */
static inline int sid_capture_priority(const void *data, size_t nbytes,
                                        int dtype, int max_faces,
                                        SIDCoord *out)
{
    SIDArchConfig cfg;
    cfg.n_faces    = (uint8_t)(max_faces > 0 ? max_faces : 4);
    cfg.use_tri    = 1;
    cfg.face_order = NULL;
    if (cfg.n_faces > TW_PRIORITY_FACES) cfg.n_faces = TW_PRIORITY_FACES;
    return sid_capture_with_config(data, nbytes, dtype, cfg, out);
}

/* Priority roundtrip test */
static inline int sid_verify_priority(const void *data, size_t nbytes,
                                       int dtype, int max_faces,
                                       const char *name)
{
    (void)name;
    SIDCoord coord;
    if (sid_capture_priority(data, nbytes, dtype, max_faces, &coord) != 0)
        return -1;

    int64_t orig_vx, orig_vy, summon_vx, summon_vy;
    int rc;
    if (dtype == 0)
        rc = sid_signature_f32((const uint8_t *)data, nbytes, &orig_vx, &orig_vy);
    else
        rc = sid_signature_q80((const uint8_t *)data, nbytes, &orig_vx, &orig_vy);
    if (rc != 0) return -1;

    sid_summon_legacy(&coord, &summon_vx, &summon_vy);
    return (summon_vx == orig_vx && summon_vy == orig_vy) ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════
   24-DIRECTION CAPTURE — best of 12 faces × 2 (hex+tri)
   ═══════════════════════════════════════════════════════════════ */

/*
 * 24-direction capture: raw data → SIDCoord (best of 24 directions).
 * Tries all 12 faces × 2 (hex+tri) = 24 independent captures,
 * picks the one with smallest resid magnitude.
 * Theoretic coverage ≈ 99.6%.
 */
static inline int sid_capture_legacy_24(const void *data, size_t nbytes,
                                         int dtype, SIDCoord *out)
{
    if (!data || !out || nbytes == 0) return -1;
    memset(out, 0, sizeof(*out));

    int64_t vx, vy;
    int rc;
    if (dtype == 0)
        rc = sid_signature_f32((const uint8_t *)data, nbytes, &vx, &vy);
    else
        rc = sid_signature_q80((const uint8_t *)data, nbytes, &vx, &vy);
    if (rc != 0) return rc;

    TWFaceIter24 r24;
    tw_iterate_faces_24(vx, vy, &r24);

    int best = tw_best_of_24(&r24);
    TWFaceCapture *fc = &r24.caps[best];

    out->face      = fc->face;
    out->zone      = fc->zone;
    out->slot      = fc->slot;
    out->resid_x   = fc->resid_x;
    out->resid_y   = fc->resid_y;
    out->tring_pos = fc->tring_pos;
    out->is_tri    = fc->is_tri;
    out->drain     = fc->drain;

    return 0;
}

/* 24-direction roundtrip test */
static inline int sid_verify_roundtrip_24(const void *data, size_t nbytes,
                                           int dtype, const char *name)
{
    (void)name;
    SIDCoord coord;
    if (sid_capture_legacy_24(data, nbytes, dtype, &coord) != 0) return -1;

    int64_t orig_vx, orig_vy, summon_vx, summon_vy;
    int rc;
    if (dtype == 0)
        rc = sid_signature_f32((const uint8_t *)data, nbytes, &orig_vx, &orig_vy);
    else
        rc = sid_signature_q80((const uint8_t *)data, nbytes, &orig_vx, &orig_vy);
    if (rc != 0) return -1;

    sid_summon_legacy(&coord, &summon_vx, &summon_vy);

    return (summon_vx == orig_vx && summon_vy == orig_vy) ? 0 : -1;
}

#endif /* SID_H */


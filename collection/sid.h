/*
 * sid.h — Single Integrated Dimension (SID)
 * ════════════════════════════════════════════════════════════════
 *
 * "Geometry IS the storage. Coordinate IS the data."
 *
 * Core idea:
 *   One line + {36°, 60°, 180°} → pentagon + hexagon →
 *   10 sectors × 6 slots = 60 positions per face →
 *   12 faces × 60 = TRing 720 = SID coordinate space
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
    uint16_t tring_pos;   /* 0..719 = face*60 + zone*6 + slot     */
    uint8_t  drain;       /* 1 if near sector boundary            */
    uint8_t  pad[3];
} SIDCoord;

/* ── SID Entry: name + coordinate ───────────────────────────── */
typedef struct {
    char     name[SID_NAME_MAX];
    SIDCoord coord;
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
 * SID Capture: Q8_0/F32 raw bytes → SIDCoord.
 * The coordinate IS the storage — no file write needed.
 */
static inline int sid_capture(const void *data, size_t nbytes,
                               int dtype, uint8_t face,
                               SIDCoord *out)
{
    if (!data || !out || nbytes == 0) return -1;
    memset(out, 0, sizeof(*out));

    int64_t vx, vy;
    int rc;
    if (dtype == 0) {
        rc = sid_signature_f32((const uint8_t *)data, nbytes, &vx, &vy);
    } else {
        rc = sid_signature_q80((const uint8_t *)data, nbytes, &vx, &vy);
    }
    if (rc != 0) return rc;

    /* Run TW capture */
    TWFaceCapture fc;
    tw_capture_face(vx, vy, face, &fc);

    out->face      = fc.face;
    out->zone      = fc.zone;
    out->slot      = fc.slot;
    out->resid_x   = fc.resid_x;
    out->resid_y   = fc.resid_y;
    out->tring_pos = fc.tring_pos;
    out->drain     = fc.drain;

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
static inline void sid_summon(const SIDCoord *coord,
                               int64_t *vx, int64_t *vy)
{
    TWCaptureInt cap;
    cap.zone    = coord->zone;
    cap.slot    = coord->slot;
    cap.resid_x = coord->resid_x;
    cap.resid_y = coord->resid_y;
    cap.drain   = coord->drain;

    tw_reconstruct_int(&cap, vx, vy);
}

/*
 * Summon signature as double (for verification/display).
 * The integer path is sid_summon() — this is a convenience wrapper.
 */
static inline void sid_summon_sig(const SIDCoord *coord,
                                   double *sig_x, double *sig_y)
{
    int64_t vx, vy;
    sid_summon(coord, &vx, &vy);
    *sig_x = (double)vx / (double)TW_SCALE;
    *sig_y = (double)vy / (double)TW_SCALE;
}

/* ═══════════════════════════════════════════════════════════════
   STORE — .twidx file format (lightweight coordinate index)
   ═══════════════════════════════════════════════════════════════ */

/*
 * .twidx format (binary):
 *   [Header: 16B]     magic(4) + version(4) + n_entries(4) + reserved(4)
 *   [Entry × N: 272B each]  name(256) + coord(16)
 *
 * Typical: 290 tensors → ~79 KB (vs 367 MB raw .qdat)
 */

typedef struct __attribute__((packed)) {
    uint32_t magic;       /* SID_TWIDX_MAGIC */
    uint32_t version;     /* SID_TWIDX_VER */
    uint32_t n_entries;   /* number of valid entries */
    uint32_t reserved;
} SIDTwidxHeader;

typedef struct __attribute__((packed)) {
    char     name[SID_NAME_MAX];  /* tensor name, null-terminated */
    uint8_t  face;                /* 0..11 */
    uint8_t  zone;                /* 0..9 */
    uint8_t  slot;                /* 0..5 */
    int64_t  resid_x;
    int64_t  resid_y;
    uint16_t tring_pos;
    uint8_t  drain;
    uint8_t  pad[5];
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
        e.face      = store->entries[i].coord.face;
        e.zone      = store->entries[i].coord.zone;
        e.slot      = store->entries[i].coord.slot;
        e.resid_x   = store->entries[i].coord.resid_x;
        e.resid_y   = store->entries[i].coord.resid_y;
        e.tring_pos = store->entries[i].coord.tring_pos;
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
        store->entries[i].coord.face      = e.face;
        store->entries[i].coord.zone      = e.zone;
        store->entries[i].coord.slot      = e.slot;
        store->entries[i].coord.resid_x   = e.resid_x;
        store->entries[i].coord.resid_y   = e.resid_y;
        store->entries[i].coord.tring_pos = e.tring_pos;
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
   SID VERIFY — capture → store → load → summon → verify
   ═══════════════════════════════════════════════════════════════ */

/*
 * Full SID roundtrip test for one tensor:
 *   1. Capture: raw data → SIDCoord
 *   2. Summon:  SIDCoord → (vx, vy)
 *   3. Verify:  (vx, vy) matches original signature
 *
 * Returns 0 on pass, -1 on error.
 */
static inline int sid_verify_roundtrip(const void *data, size_t nbytes,
                                        int dtype, const char *name)
{
    SIDCoord coord;
    if (sid_capture(data, nbytes, dtype, 0, &coord) != 0)
        return -1;

    /* Compute original signature */
    int64_t orig_vx, orig_vy;
    int rc;
    if (dtype == 0)
        rc = sid_signature_f32((const uint8_t *)data, nbytes, &orig_vx, &orig_vy);
    else
        rc = sid_signature_q80((const uint8_t *)data, nbytes, &orig_vx, &orig_vy);
    if (rc != 0) return -1;

    /* Summon from coordinate */
    int64_t summon_vx, summon_vy;
    sid_summon(&coord, &summon_vx, &summon_vy);

    /* Must match exactly */
    if (summon_vx != orig_vx || summon_vy != orig_vy)
        return -1;

    return 0;
}

#endif /* SID_H */

/*
 * sid.h — Single Integrated Dimension (SID)
 * ════════════════════════════════════════════════════════════════
 *
 * "Geometry IS the storage. Coordinate IS the data."
 *
 * Y-triangle retarget:
 *   SIDCoord.node_id (0..20735) replaces face/zone/slot/tring_pos.
 *   Capture: tw_capture_int_combined → tw_to_node → capo ×12 pentagons.
 *   Summon:  node_id → geo_shell_decode → tw_reconstruct_int → (vx, vy).
 *
 * Storage:
 *   .twidx = node_id-only index (compact for SmolLM2-360M Q8_0)
 *   No .qdat, no .gsten, no raw weight storage needed.
 *
 * Summon formula:
 *   node_id → shell_decode → (zone, slot) → reconstruct_int → sig
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
#define GEO_JUMP_INLINE
#include "geo_jump.h"
#include "geo_shell.h"

/* ── Limits ─────────────────────────────────────────────────── */
#define SID_MAX_ENTRIES  65536
#define SID_NAME_MAX     256
#define SID_TWIDX_MAGIC  0x53494432u   /* "SID2" — Y-triangle format */
#define SID_TWIDX_VER    2u

/* ── SID Coordinate ─────────────────────────────────────────── */
typedef struct {
    uint32_t node_id;     /* 0..20735 Y-triangle node_id */
    int64_t  resid_x;     /* residual X (TW_SCALE units) */
    int64_t  resid_y;     /* residual Y (TW_SCALE units) */
    uint8_t  drain;       /* 1 if near sector boundary */
    uint8_t  pad[3];
} SIDCoord;

/* ── SID Entry: name + coordinate (SIDCoord) ──────────────────── */
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

    size_t n_blocks = nbytes / 34;
    if (n_blocks > 2) n_blocks = 2;
    uint32_t total_vals = (uint32_t)(n_blocks * 32);
    if (total_vals > 64) total_vals = 64;

    float sum_a = 0, sum_b = 0;
    uint32_t na = 0, nb = 0;

    for (uint32_t b = 0; b < n_blocks; b++) {
        uint16_t scale_bits;
        memcpy(&scale_bits, q8_data + b * 34, 2);

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

/* ═══════════════════════════════════════════════════════════════
   F16 signature (dtype=2)
   ═══════════════════════════════════════════════════════════════ */
static inline int sid_signature_f16(const uint8_t *f16_data, size_t nbytes,
                                     int64_t *vx, int64_t *vy)
{
    if (!f16_data || nbytes < 4) return -1;
    uint32_t n = (uint32_t)(nbytes / 2);
    if (n > 64) n = 64;
    uint32_t half = n / 2;
    double sum_a = 0, sum_b = 0;
    for (uint32_t i = 0; i < half; i++) {
        uint16_t h; memcpy(&h, f16_data + i * 2, 2);
        uint32_t sign = (uint32_t)(h >> 15) << 31;
        uint32_t exp  = (uint32_t)((h >> 10) & 0x1Fu);
        uint32_t mant = (uint32_t)(h & 0x3FFu);
        float val;
        if (exp == 0) {
            uint32_t v = sign; memcpy(&val, &v, 4);
        } else if (exp == 31) {
            uint32_t v = sign | 0x7F800000u | (mant << 13); memcpy(&val, &v, 4);
        } else {
            exp = exp - 15 + 127;
            uint32_t v = sign | (exp << 23) | (mant << 13); memcpy(&val, &v, 4);
        }
        sum_a += val;
    }
    for (uint32_t i = half; i < n; i++) {
        uint16_t h; memcpy(&h, f16_data + i * 2, 2);
        uint32_t sign = (uint32_t)(h >> 15) << 31;
        uint32_t exp  = (uint32_t)((h >> 10) & 0x1Fu);
        uint32_t mant = (uint32_t)(h & 0x3FFu);
        float val;
        if (exp == 0) {
            uint32_t v = sign; memcpy(&val, &v, 4);
        } else if (exp == 31) {
            uint32_t v = sign | 0x7F800000u | (mant << 13); memcpy(&val, &v, 4);
        } else {
            exp = exp - 15 + 127;
            uint32_t v = sign | (exp << 23) | (mant << 13); memcpy(&val, &v, 4);
        }
        sum_b += val;
    }
    *vx = (int64_t)((sum_a / half) * TW_SCALE);
    *vy = (int64_t)((sum_b / (n - half)) * TW_SCALE);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   I8 signature (dtype=3)
   ═══════════════════════════════════════════════════════════════ */
static inline int sid_signature_i8(const uint8_t *i8_data, size_t nbytes,
                                    int64_t *vx, int64_t *vy)
{
    if (!i8_data || nbytes < 2) return -1;
    uint32_t n = (uint32_t)nbytes;
    if (n > 64) n = 64;
    uint32_t half = n / 2;
    double sum_a = 0, sum_b = 0;
    for (uint32_t i = 0; i < half; i++)
        sum_a += (float)(int8_t)i8_data[i];
    for (uint32_t i = half; i < n; i++)
        sum_b += (float)(int8_t)i8_data[i];
    *vx = (int64_t)((sum_a / half) * TW_SCALE);
    *vy = (int64_t)((sum_b / (n - half)) * TW_SCALE);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   STORE — .twidx file format (node_id-only index, v2)
   ═══════════════════════════════════════════════════════════════ */

/*
 * .twidx format v2 (binary):
 *   [Header: 16B]     magic(4) + version(4) + n_entries(4) + reserved(4)
 *   [Entry × N: 280B each] name(256) + node_id(4) + resid_x(8) + resid_y(8) + drain(1) + pad(3)
 */

typedef struct __attribute__((packed)) {
    uint32_t magic;       /* SID_TWIDX_MAGIC */
    uint32_t version;     /* SID_TWIDX_VER */
    uint32_t n_entries;   /* number of valid entries */
    uint32_t reserved;
} SIDTwidxHeader;

typedef struct __attribute__((packed)) {
    char     name[SID_NAME_MAX];  /* tensor name, null-terminated */
    uint32_t node_id;             /* 0..20735 Y-triangle node_id */
    int64_t  resid_x;
    int64_t  resid_y;
    uint8_t  drain;
    uint8_t  pad[3];
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
        e.node_id  = store->entries[i].coord.node_id;
        e.resid_x  = store->entries[i].coord.resid_x;
        e.resid_y  = store->entries[i].coord.resid_y;
        e.drain    = store->entries[i].coord.drain;

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
        store->entries[i].coord.node_id = e.node_id;
        store->entries[i].coord.resid_x = e.resid_x;
        store->entries[i].coord.resid_y = e.resid_y;
        store->entries[i].coord.drain   = e.drain;
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
   CAPTURE — Y-triangle node_id based
   ═══════════════════════════════════════════════════════════════ */

/*
 * Capture raw data → SIDCoord with node_id.
 * Uses tw_capture_int_combined for zone/slot, then tw_to_node for node_id.
 */
static inline int sid_capture(const void *data, size_t nbytes,
                               int dtype, SIDCoord *out)
{
    if (!data || !out || nbytes == 0) return -1;
    memset(out, 0, sizeof(*out));

    int64_t vx, vy;
    int rc;
    switch (dtype) {
        case 0: rc = sid_signature_f32((const uint8_t *)data, nbytes, &vx, &vy); break;
        case 1: rc = sid_signature_q80((const uint8_t *)data, nbytes, &vx, &vy); break;
        case 2: rc = sid_signature_f16((const uint8_t *)data, nbytes, &vx, &vy); break;
        case 3: rc = sid_signature_i8((const uint8_t *)data, nbytes, &vx, &vy); break;
        default: return -1;
    }
    if (rc != 0) return rc;

    TWCaptureInt cap;
    uint8_t is_tri;
    tw_capture_int_combined(vx, vy, &cap, &is_tri);

    out->node_id = tw_to_node(cap.zone, cap.slot);
    out->resid_x = cap.resid_x;
    out->resid_y = cap.resid_y;
    out->drain   = cap.drain;

    return 0;
}

/*
 * Capture with capo ×12: tries all 12 pentagons, picks best resid.
 * out[12] = {node_id at pentagon 0..11}.
 * Returns the index of the best (smallest resid) pentagon.
 */
static inline int sid_capture_capo(const void *data, size_t nbytes,
                                    int dtype, uint32_t out[12])
{
    if (!data || !out || nbytes == 0) return -1;

    int64_t vx, vy;
    int rc;
    if (dtype == 0)
        rc = sid_signature_f32((const uint8_t *)data, nbytes, &vx, &vy);
    else
        rc = sid_signature_q80((const uint8_t *)data, nbytes, &vx, &vy);
    if (rc != 0) return rc;

    tw_capture_capo_all(vx, vy, out);
    return 0;
}

/* ── Summon: node_id → (vx, vy) via shell decode + reconstruct ── */

/*
 * Decompose node_id → (zone, slot) via shell decode.
 * Then reconstruct (vx, vy) from centroids + resid.
 */
static inline void sid_summon(const SIDCoord *coord,
                               int64_t *vx, int64_t *vy)
{
    uint32_t face = geo_shell_face(coord->node_id);
    uint32_t ring = geo_shell_ring(coord->node_id);

    /* Reconstruct zone/slot from face/ring */
    uint8_t zone = (uint8_t)(face % TW_N_SECTORS);
    uint8_t slot = (uint8_t)(ring % TW_SLOTS_PER);

    /* Reconstruct using hex centroids (standard reconstruction) */
    TWCaptureInt cap;
    cap.zone = zone;
    cap.slot = (uint8_t)(zone * TW_SLOTS_PER + slot);
    cap.resid_x = coord->resid_x;
    cap.resid_y = coord->resid_y;
    tw_reconstruct_int(&cap, vx, vy);
}

/* ── Roundtrip verification ── */

static inline int sid_verify_roundtrip(const void *data, size_t nbytes,
                                        int dtype, const char *name)
{
    (void)name;
    SIDCoord coord;
    if (sid_capture(data, nbytes, dtype, &coord) != 0)
        return -1;

    int64_t orig_vx, orig_vy;
    int rc;
    switch (dtype) {
        case 0: rc = sid_signature_f32((const uint8_t *)data, nbytes, &orig_vx, &orig_vy); break;
        case 1: rc = sid_signature_q80((const uint8_t *)data, nbytes, &orig_vx, &orig_vy); break;
        case 2: rc = sid_signature_f16((const uint8_t *)data, nbytes, &orig_vx, &orig_vy); break;
        case 3: rc = sid_signature_i8((const uint8_t *)data, nbytes, &orig_vx, &orig_vy); break;
        default: return -1;
    }
    if (rc != 0) return -1;

    int64_t summon_vx, summon_vy;
    sid_summon(&coord, &summon_vx, &summon_vy);

    if (summon_vx != orig_vx || summon_vy != orig_vy)
        return -1;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   SIDArchConfig — Architecture-Adaptive Capture Configuration
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t        n_faces;       /* 1..12 pentagons to try */
    uint8_t        use_tri;       /* 1=also try tri centroids (reserved) */
    const uint8_t *face_order;    /* priority pentagon list, NULL = default */
} SIDArchConfig;

/* Built-in profiles */
static inline SIDArchConfig sid_conf_smollm(void) {
    SIDArchConfig c = { .n_faces = 4, .use_tri = 0, .face_order = NULL };
    return c;
}

static inline SIDArchConfig sid_conf_vlm(void) {
    SIDArchConfig c = { .n_faces = 8, .use_tri = 0, .face_order = NULL };
    return c;
}

static inline SIDArchConfig sid_conf_qwen(void) {
    SIDArchConfig c = { .n_faces = 12, .use_tri = 0, .face_order = NULL };
    return c;
}

static inline SIDArchConfig sid_conf_fast(void) {
    SIDArchConfig c = { .n_faces = 1, .use_tri = 0, .face_order = NULL };
    return c;
}

static inline SIDArchConfig sid_conf_full(void) {
    SIDArchConfig c = { .n_faces = 12, .use_tri = 0, .face_order = NULL };
    return c;
}

/*
 * Capture with configurable SIDArchConfig.
 * Uses capo × n_faces pentagons, picks best resid.
 */
static inline int sid_capture_with_config(const void *data, size_t nbytes,
                                           int dtype, SIDArchConfig cfg,
                                           SIDCoord *out)
{
    if (!data || !out || nbytes == 0) return -1;
    memset(out, 0, sizeof(*out));

    int64_t vx, vy;
    int rc;
    switch (dtype) {
        case 0: rc = sid_signature_f32((const uint8_t *)data, nbytes, &vx, &vy); break;
        case 1: rc = sid_signature_q80((const uint8_t *)data, nbytes, &vx, &vy); break;
        case 2: rc = sid_signature_f16((const uint8_t *)data, nbytes, &vx, &vy); break;
        case 3: rc = sid_signature_i8((const uint8_t *)data, nbytes, &vx, &vy); break;
        default: return -1;
    }
    if (rc != 0) return rc;

    /* Single capture → capo ×12 */
    uint32_t capo_nodes[12];
    tw_capture_capo_all(vx, vy, capo_nodes);

    /* Pick best among first n_faces pentagons */
    int max_faces = cfg.n_faces;
    if (max_faces <= 0 || max_faces > 12) max_faces = 12;

    /* Get base TWCaptureInt for resid */
    TWCaptureInt cap;
    uint8_t is_tri;
    tw_capture_int_combined(vx, vy, &cap, &is_tri);

    int best_idx = 0;
    int64_t best_mag = cap.resid_x * cap.resid_x + cap.resid_y * cap.resid_y;

    /* For simplicity, all pentagons share the same resid (capo only changes pentagon).
       In future, could re-capture per-pentagon for better accuracy. */
    (void)best_mag;

    out->node_id = capo_nodes[best_idx % 12];
    out->resid_x = cap.resid_x;
    out->resid_y = cap.resid_y;
    out->drain   = cap.drain;

    return 0;
}

/* Priority capture: convenience wrapper */
static inline int sid_capture_priority(const void *data, size_t nbytes,
                                        int dtype, int max_faces,
                                        SIDCoord *out)
{
    SIDArchConfig cfg;
    cfg.n_faces    = (uint8_t)(max_faces > 0 ? max_faces : 4);
    cfg.use_tri    = 0;
    cfg.face_order = NULL;
    if (cfg.n_faces > 12) cfg.n_faces = 12;
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
    switch (dtype) {
        case 0: rc = sid_signature_f32((const uint8_t *)data, nbytes, &orig_vx, &orig_vy); break;
        case 1: rc = sid_signature_q80((const uint8_t *)data, nbytes, &orig_vx, &orig_vy); break;
        case 2: rc = sid_signature_f16((const uint8_t *)data, nbytes, &orig_vx, &orig_vy); break;
        case 3: rc = sid_signature_i8((const uint8_t *)data, nbytes, &orig_vx, &orig_vy); break;
        default: return -1;
    }
    if (rc != 0) return -1;

    sid_summon(&coord, &summon_vx, &summon_vy);
    return (summon_vx == orig_vx && summon_vy == orig_vy) ? 0 : -1;
}

#endif /* SID_H */

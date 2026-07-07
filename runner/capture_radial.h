/*
 * capture_radial.h — Radial capture pipeline (replaces capture_pipeline.h)
 *
 * Uses geo_radial_capture.h instead of tw_capture_int.h.
 * No content signature, no freeze wallet, no rewind.
 * Just: tensor name → radial address → twin address.
 */

#ifndef CAPTURE_RADIAL_H
#define CAPTURE_RADIAL_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_radial_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
   STRUCTURES
   ═══════════════════════════════════════════════════════════════ */

#define CRAD_MAX_TENSORS 4096
#define CRAD_TIMESTAMP_SZ 64

/* Result entry for one captured tensor */
typedef struct {
    char     name[68];       /* tensor name (aligned to 8) */
    uint64_t addr;           /* radial address (0..167 + twin bit) */
    uint64_t twin;           /* twin address */
} CradEntry;

/* Capture result */
typedef struct {
    char          timestamp[CRAD_TIMESTAMP_SZ];
    uint32_t      n_captured;
    uint32_t      n_verified;
    int           lossless_ok;
    CradEntry     entries[CRAD_MAX_TENSORS];
} CradResult;

/* Descriptor for a tensor to capture */
typedef struct {
    const char *name;
    const void *data;
    size_t      nbytes;
    int         dtype;
} CradTensor;

/* ═══════════════════════════════════════════════════════════════
   INIT / TIMESTAMP
   ═══════════════════════════════════════════════════════════════ */

static inline void _crad_timestamp(char *buf, size_t sz) {
    /* __DATE__ = "Mmm dd yyyy" (single-digit day has leading space) */
    /* __TIME__ = "hh:mm:ss" */
    int day = (__DATE__[4] == ' ' ? __DATE__[5] - '0'
               : (__DATE__[4] - '0') * 10 + (__DATE__[5] - '0'));
    snprintf(buf, sz, "%.3s%02d%.4s_%.2s%.2s%.2s",
             __DATE__, day, __DATE__ + 7,
             __TIME__, __TIME__ + 3, __TIME__ + 6);
}

static inline void crad_init(CradResult *cr) {
    memset(cr, 0, sizeof(*cr));
    _crad_timestamp(cr->timestamp, sizeof(cr->timestamp));
}

/* ═══════════════════════════════════════════════════════════════
   CAPTURE — tensor name → radial address
   ═══════════════════════════════════════════════════════════════ */

static inline int crad_capture(CradResult *cr, const char *name) {
    if (!cr || !name || name[0] == '\0') return -1;
    if (cr->n_captured >= CRAD_MAX_TENSORS) return -2;

    CradEntry *e = &cr->entries[cr->n_captured];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';

    e->addr  = rc_capture_name(name);
    e->twin  = rc_twin_swap(e->addr);

    cr->n_captured++;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   SUMMARY
   ═══════════════════════════════════════════════════════════════ */

static inline void crad_summary(const CradResult *cr, FILE *out) {
    fprintf(out, "[crad] %u tensors captured\n", cr->n_captured);
    for (uint32_t i = 0; i < cr->n_captured && i < 16; i++) {
        const CradEntry *e = &cr->entries[i];
        int vi = (int)((e->addr & ~RC_TWIN_BIT) / RC_N_NODES);
        int ni = (int)((e->addr & ~RC_TWIN_BIT) % RC_N_NODES);
        int is_twin = (e->addr & RC_TWIN_BIT) ? 1 : 0;
        fprintf(out, "  [%3u] %-40s → v%02d n%d%s  addr=0x%04llx twin=0x%04llx\n",
                i, e->name, vi, ni, is_twin ? " [B]" : "",
                (unsigned long long)e->addr,
                (unsigned long long)e->twin);
    }
    if (cr->n_captured > 16)
        fprintf(out, "  ... (%u more)\n", cr->n_captured - 16);
}

/* ═══════════════════════════════════════════════════════════════
   VERIFICATION
   ═══════════════════════════════════════════════════════════════ */

/* Verify: capture same name twice → same address (deterministic) */
static inline void crad_verify(CradResult *cr, const char **names, int n_names) {
    if (!cr || !names) return;
    int ok = 1;
    for (int i = 0; (uint32_t)i < cr->n_captured && i < n_names; i++) {
        uint64_t a = rc_capture_name(names[i]);
        if (a != cr->entries[i].addr) {
            fprintf(stderr, "[crad] verify FAIL: %s → 0x%llx (expected 0x%llx)\n",
                    names[i], (unsigned long long)a,
                    (unsigned long long)cr->entries[i].addr);
            ok = 0;
        }
    }
    cr->n_verified = (uint32_t)(cr->n_captured < (uint32_t)n_names
                                ? cr->n_captured : n_names);
    cr->lossless_ok = ok;
    fprintf(stderr, "[crad] verification: %s (%u tensors)\n",
            ok ? "OK" : "FAIL", cr->n_verified);
}

/* ═══════════════════════════════════════════════════════════════
   STORE — write capture to file
   ═══════════════════════════════════════════════════════════════ */

#define CRAD_STORE_MAGIC   0x44415243  /* "CRAD" */
#define CRAD_STORE_VERSION 1

static inline size_t crad_write_store(const CradResult *cr, const char *outdir) {
    if (!cr || cr->n_captured == 0) return 0;

    char path[512];
    snprintf(path, sizeof(path), "%s/crad-%s.rad", outdir, cr->timestamp);

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[crad] ERROR: cannot open %s\n", path); return 0; }

    /* Header */
    uint32_t magic   = CRAD_STORE_MAGIC;
    uint32_t version = CRAD_STORE_VERSION;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&cr->n_captured, 4, 1, f);
    fwrite(cr->timestamp, 1, 32, f);

    /* Entries */
    for (uint32_t i = 0; i < cr->n_captured; i++) {
        const CradEntry *e = &cr->entries[i];
        uint16_t name_len = (uint16_t)strlen(e->name);
        if (name_len > 255) name_len = 255;
        fwrite(&name_len, 2, 1, f);
        fwrite(e->name, 1, name_len, f);
        fwrite(&e->addr, 8, 1, f);
        fwrite(&e->twin, 8, 1, f);
    }

    long total = ftell(f);
    fclose(f);
    if (total > 0)
        fprintf(stderr, "[crad] store: %s (%ld bytes, %u tensors)\n",
                path, total, cr->n_captured);
    return (size_t)(total > 0 ? total : 0);
}

/* ═══════════════════════════════════════════════════════════════
   FULL PIPELINE
   ═══════════════════════════════════════════════════════════════ */

static inline void crad_run_full(CradResult *cr, const char *outdir,
                                  const char **names, int n_names) {
    crad_init(cr);
    for (int i = 0; i < n_names; i++) {
        if (names[i]) crad_capture(cr, names[i]);
    }
    crad_write_store(cr, outdir);
    crad_verify(cr, names, n_names);
    crad_summary(cr, stdout);
}

/* ═══════════════════════════════════════════════════════════════
   TENSOR ADDRESS LOOKUP — for SID/page-table integration
   ═══════════════════════════════════════════════════════════════ */

/* Look up a tensor name in a captured result.
   Returns the address, or 0 if not found. */
static inline uint64_t crad_find_addr(const CradResult *cr, const char *name) {
    if (!cr || !name) return 0;
    for (uint32_t i = 0; i < cr->n_captured; i++) {
        if (strcmp(cr->entries[i].name, name) == 0)
            return cr->entries[i].addr;
    }
    return 0;
}

/* Get all captured names and addresses as parallel arrays.
   Returns number of entries. */
static inline uint32_t crad_get_all(const CradResult *cr,
                                     const char **names_out,
                                     uint64_t *addrs_out,
                                     uint64_t *twins_out,
                                     uint32_t max_out) {
    if (!cr || !names_out || !addrs_out) return 0;
    uint32_t n = cr->n_captured < max_out ? cr->n_captured : max_out;
    for (uint32_t i = 0; i < n; i++) {
        names_out[i] = cr->entries[i].name;
        addrs_out[i] = cr->entries[i].addr;
        if (twins_out) twins_out[i] = cr->entries[i].twin;
    }
    return n;
}

#ifdef __cplusplus
}
#endif

#endif /* CAPTURE_RADIAL_H */

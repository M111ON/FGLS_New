#pragma once
/*
 * hb_manifest.h — cycle_id → gpx5 file path registry
 *
 * HbManifest is a flat array of (cycle_id, path) pairs.
 * Used by hb_tile_stream_mc() to resolve which file to open
 * for a given HbFiboLutEntry.cycle_id without a full decode.
 *
 * Max cycles: HB_MANIFEST_MAX (stack-friendly default 64).
 * Extend or heap-alloc for larger multi-cycle jobs.
 */

#include <stdint.h>
#include <string.h>

#define HB_MANIFEST_MAX   64u
#define HB_PATH_MAX      256u

typedef struct {
    uint16_t cycle_id;
    char     path[HB_PATH_MAX];
} HbManifestEntry;

typedef struct {
    HbManifestEntry entries[HB_MANIFEST_MAX];
    uint32_t        n;          /* active entry count */
} HbManifest;

/* ── init ── */
static inline void hb_manifest_init(HbManifest *m) {
    memset(m, 0, sizeof(*m));
}

/* ── add entry; returns 0 on success, -1 if full ── */
static inline int hb_manifest_add(HbManifest *m,
                                   uint16_t    cycle_id,
                                   const char *path)
{
    if (!m || !path || m->n >= HB_MANIFEST_MAX) return -1;
    m->entries[m->n].cycle_id = cycle_id;
    strncpy(m->entries[m->n].path, path, HB_PATH_MAX - 1);
    m->entries[m->n].path[HB_PATH_MAX - 1] = '\0';
    m->n++;
    return 0;
}

/* ── O(n) lookup; n ≤ 64 so linear is fine ── */
static inline const char *hb_manifest_path(const HbManifest *m,
                                             uint16_t          cycle_id)
{
    if (!m) return NULL;
    for (uint32_t i = 0; i < m->n; i++) {
        if (m->entries[i].cycle_id == cycle_id)
            return m->entries[i].path;
    }
    return NULL;
}

/*
 * fgls_twin_store.h — FGLS Storage Bridge
 * ════════════════════════════════════════
 * Bridge: DodecaEntry (TPOGLS) → FrustumSlot64 → GiantArray → CubeFileStore
 *
 * Mapping:
 *   trit = (addr ^ value) % 27          ← 3³ ternary index
 *   coset = trit / 3                    → GiantCube index  0..8
 *   face  = trit % 6 (spoke)            → FrustumSlot64    0..5
 *   level = trit % 4                    → core[level]      0..3
 *
 * Geometry:
 *   6 frustum ชิ้น ประกอบเป็น 1 GiantCube
 *   12 GiantCube × 384B = 4608B GiantArray payload
 *   + header(36) + meta(252) = 4896B CubeFileStore
 *
 * Delete = reserved_mask bitmask (structural silence)
 *   → coset masked → payload zero-filled → GPU stride skips
 *   → data reconstructible from seed, path inaccessible
 *
 * No malloc. No float. No heap.
 * ════════════════════════════════════════
 */

#ifndef FGLS_TWIN_STORE_H
#define FGLS_TWIN_STORE_H

#include <stdint.h>
#include <string.h>
/* geo_giant_array.h, geo_cube_file_store.h, geo_dodeca.h
   included by the translation unit before this header     */

/* ════════════════════════════════════════
   MAPPING CONSTANTS
   ════════════════════════════════════════ */
#define FTS_TRIT_MOD    27u   /* 3³ — shared invariant */
#define FTS_COSET_DIV    3u   /* trit → coset: trit/3  */
#define FTS_FACE_MOD     6u   /* trit → face:  trit%6  */
#define FTS_LEVEL_MOD    4u   /* trit → level: trit%4  */

/* ── FtsTritAddr: geometric address derived from (addr, value) ── */
typedef struct {
    uint8_t trit;    /* 0..26 — 3³ index                */
    uint8_t coset;   /* 0..8  — GiantCube index         */
    uint8_t face;    /* 0..5  — FrustumSlot direction   */
    uint8_t level;   /* 0..3  — core[level] inside slot */
} FtsTritAddr;

/* ── FtsTwinStore: top-level context ──────────────────────────── */
typedef struct {
    GiantArray    ga;             /* 12 GiantCubes                   */
    CubeFileStore cfs;            /* serialized 4896B file           */
    uint32_t      reserved_mask;  /* bitmask: deleted cosets         */
    uint32_t      write_count;
    uint32_t      delete_count;
    uint32_t      overflow_count; /* coset masked → bypassed         */
} FtsTwinStore;

/* ════════════════════════════════════════
   INIT
   ════════════════════════════════════════ */
static inline void fts_init(FtsTwinStore *s, uint64_t root_seed) {
    memset(s, 0, sizeof(*s));
    giant_array_init(&s->ga, root_seed);
}

/* ════════════════════════════════════════
   TRIT ADDRESS DERIVATION
   (addr, value) → geometric position
   ════════════════════════════════════════ */
static inline FtsTritAddr fts_trit_addr(uint64_t addr, uint64_t value) {
    uint32_t trit  = (uint32_t)((addr ^ value) % FTS_TRIT_MOD);
    FtsTritAddr t;
    t.trit  = (uint8_t)trit;
    t.coset = (uint8_t)(trit / FTS_COSET_DIV);   /* 0..8 */
    t.face  = (uint8_t)(trit % FTS_FACE_MOD);    /* 0..5 */
    t.level = (uint8_t)(trit % FTS_LEVEL_MOD);   /* 0..3 */
    return t;
}

/* ════════════════════════════════════════
   WRITE: DodecaEntry → FrustumSlot64
   ════════════════════════════════════════ */
/*
 * Map one DodecaEntry into the correct FrustumSlot64 inside GiantArray.
 *
 * DodecaEntry fields → FrustumSlot64:
 *   merkle_root  → core[level]          (geometry fingerprint)
 *   sha256_hi    → core[(level+1)%4]    (integrity word)
 *   offset       → addr[level]          (semantic distance)
 *   hop_count    → addr[(level+1)%4]    (path depth)
 *   segment      → world[level]         (scroll position, 0..255 → 0/1)
 *
 * Returns: 0=ok  -1=coset reserved (deleted)  -2=null entry
 */
static inline int fts_write(FtsTwinStore      *s,
                              uint64_t           addr,
                              uint64_t           value,
                              const DodecaEntry *e)
{
    if (!e) return -2;

    FtsTritAddr ta = fts_trit_addr(addr, value);

    /* coset deleted → structural silence */
    if (s->reserved_mask & (1u << ta.coset)) {
        s->overflow_count++;
        return -1;
    }

    FrustumSlot64 *slot = &s->ga.cubes[ta.coset].faces[ta.face];

    /* write into the level lane */
    uint8_t lv  = ta.level;
    uint8_t lv1 = (uint8_t)((lv + 1u) % 4u);

    slot->core[lv]  = e->merkle_root;
    slot->core[lv1] = e->sha256_hi;
    slot->addr[lv]  = (uint32_t)e->offset;
    slot->addr[lv1] = (uint32_t)e->hop_count;
    slot->world[lv] = (uint8_t)(e->segment & 1u);
    slot->coset     = ta.coset;
    slot->frustum_id= ta.face;
    slot->axis      = (uint8_t)(ta.face / 2u);
    slot->dir       = ta.face;

    /* recompute checksum */
    uint32_t chk = 0u;
    for (uint8_t i = 0; i < 4u; i++)
        chk ^= (uint32_t)(slot->core[i] ^ (slot->core[i] >> 32));
    slot->checksum = chk;
    slot->_pad     = 0u;

    /* update GiantCube master fold */
    GiantCube *gc = &s->ga.cubes[ta.coset];
    uint32_t fold = 0u;
    for (uint8_t f = 0; f < 6u; f++)
        fold ^= gc->faces[f].checksum;
    gc->master_core_lo = fold;

    s->write_count++;
    return 0;
}

/* ════════════════════════════════════════
   DELETE: structural silence on coset
   ════════════════════════════════════════ */
/*
 * Set reserved_mask bit → coset silenced
 * Future writes to same trit range → fts_write returns -1
 * Payload zero-filled on next gcfs_serialize
 * Data reconstructible from seed (not destroyed, just inaccessible)
 */
static inline void fts_delete_coset(FtsTwinStore *s, uint8_t coset) {
    if (coset >= 12u) return;
    s->reserved_mask |= (1u << coset);
    s->delete_count++;
}

/*
 * Convenience: delete by addr+value
 * Silences the entire coset that addr maps to
 */
static inline void fts_delete(FtsTwinStore *s,
                                uint64_t      addr,
                                uint64_t      value)
{
    FtsTritAddr ta = fts_trit_addr(addr, value);
    fts_delete_coset(s, ta.coset);
}

/* ════════════════════════════════════════
   SERIALIZE: GiantArray → CubeFileStore → 4896B buffer
   ════════════════════════════════════════ */
static inline void fts_serialize(FtsTwinStore *s) {
    s->ga.dispatch_id++;
    gcfs_serialize(&s->cfs, &s->ga,
                   GCFS_MODE_SNAPSHOT,
                   s->reserved_mask);
}

/*
 * Write 4896B flat buffer — caller provides buf[4896]
 * Call fts_serialize() first to refresh cfs
 */
static inline void fts_write_buf(const FtsTwinStore *s,
                                  uint8_t out[GCFS_TOTAL_BYTES])
{
    gcfs_write(&s->cfs, out);
}

/* ════════════════════════════════════════
   QUERY: check if addr is accessible
   Returns 0 = accessible, -1 = deleted (coset reserved)
   ════════════════════════════════════════ */
static inline int fts_accessible(const FtsTwinStore *s,
                                   uint64_t addr, uint64_t value)
{
    FtsTritAddr ta = fts_trit_addr(addr, value);
    return (s->reserved_mask & (1u << ta.coset)) ? -1 : 0;
}

/* ════════════════════════════════════════
   STATS
   ════════════════════════════════════════ */
typedef struct {
    uint32_t writes;
    uint32_t deletes;
    uint32_t overflows;
    uint32_t active_cosets;   /* 12 - popcount(reserved_mask) */
    uint32_t reserved_mask;
} FtsTwinStats;

static inline FtsTwinStats fts_stats(const FtsTwinStore *s) {
    FtsTwinStats st;
    st.writes         = s->write_count;
    st.deletes        = s->delete_count;
    st.overflows      = s->overflow_count;
    st.reserved_mask  = s->reserved_mask;
    /* count active cosets */
    uint32_t rc = s->reserved_mask;
    uint32_t pop = 0u;
    while (rc) { pop += rc & 1u; rc >>= 1; }
    st.active_cosets  = 12u - pop;
    return st;
}

#endif /* FGLS_TWIN_STORE_H */

/*
 * lc_hdr.h — LC-GCFS minimal stub for bench/test compilation
 * Reconstructed from tgw_ground_lcgw.h usage.
 * Real implementation lives in lc_gcfs_pkg.
 *
 * Symbols provided:
 *   LC_LEVEL_0, RewindBuffer, LCDeleteResult
 *   LCGWChunk, LCGWFile, lcgw_files[]
 *   lcgw_init(), lcgw_open(), lcgw_build_from_seed()
 *   lcgw_delete(), lcgw_read_payload()
 *
 * Sacred: 4896 = GCFS_TOTAL_BYTES, 16 = max chunks per file
 *         306  = raw bytes per chunk (4896/16)
 */

#ifndef LC_HDR_H
#define LC_HDR_H

#include <stdint.h>
#include <string.h>

/* ── level enum ─────────────────────────────────────────────── */
typedef enum { LC_LEVEL_0 = 0, LC_LEVEL_1, LC_LEVEL_2 } LCLevel;

/* ── rewind buffer (used by ghost delete) ────────────────────── */
#define LC_REWIND_SLOTS 8u
typedef struct {
    uint32_t entries[LC_REWIND_SLOTS];
    uint8_t  count;
} RewindBuffer;

/* ── delete result ───────────────────────────────────────────── */
typedef struct {
    int      status;    /* 0=ok, -1=err */
    uint32_t cut_a;
    uint32_t cut_b;
    uint32_t cut_c;
} LCDeleteResult;

/* ── chunk ───────────────────────────────────────────────────── */
#define LCGW_RAW_BYTES  306u   /* 4896 / 16 chunks */
typedef struct {
    uint8_t  raw[LCGW_RAW_BYTES];
    uint8_t  valid;
    uint8_t  ghosted;
    uint8_t  _pad[2];
} LCGWChunk;

/* ── file slot ───────────────────────────────────────────────── */
#define LCGW_MAX_CHUNKS  16u
#define LCGW_FILE_SLOTS  32u   /* enough for multi-test runs */
typedef struct {
    int        open;
    uint32_t   chunk_count;
    LCGWChunk  chunks[LCGW_MAX_CHUNKS];
    char       name[16];
    LCLevel    level;
} LCGWFile;

/* ── global file table ───────────────────────────────────────── */
static LCGWFile lcgw_files[LCGW_FILE_SLOTS];
static int      _lcgw_initialized = 0;

/* ── init (idempotent) ───────────────────────────────────────── */
static inline void lcgw_init(void)
{
    if (_lcgw_initialized) return;
    memset(lcgw_files, 0, sizeof(lcgw_files));
    for (int i = 0; i < (int)LCGW_FILE_SLOTS; i++)
        lcgw_files[i].open = 0;
    _lcgw_initialized = 1;
}

/* ── open: allocate a file slot, init chunks ─────────────────── */
static inline int lcgw_open(const char *name, uint32_t chunk_count,
                              const uint64_t *seeds, LCLevel level)
{
    (void)seeds;
    if (chunk_count == 0 || chunk_count > LCGW_MAX_CHUNKS) return -1;

    for (int i = 0; i < (int)LCGW_FILE_SLOTS; i++) {
        if (!lcgw_files[i].open) {
            lcgw_files[i].open        = 1;
            lcgw_files[i].chunk_count = chunk_count;
            lcgw_files[i].level       = level;
            memset(lcgw_files[i].chunks, 0,
                   chunk_count * sizeof(LCGWChunk));
            /* copy name safely */
            uint32_t ni = 0;
            while (name[ni] && ni < 15u) {
                lcgw_files[i].name[ni] = name[ni];
                ni++;
            }
            lcgw_files[i].name[ni] = '\0';
            return i;
        }
    }
    return -1;   /* no free slot */
}

/* ── build_from_seed: deterministic chunk fill ───────────────── */
static inline void lcgw_build_from_seed(uint8_t *raw, uint64_t seed,
                                         uint32_t write_count)
{
    /* xorshift64-based fill — same pattern as real LC-GCFS */
    uint64_t s = seed ^ ((uint64_t)write_count * 0x9e3779b97f4a7c15ull);
    for (uint32_t i = 0; i < LCGW_RAW_BYTES; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        raw[i] = (uint8_t)(s & 0xFFu);
    }
}

/* ── delete: triple-cut ghost (content stays, path severed) ──── */
static inline LCDeleteResult lcgw_delete(int gslot, RewindBuffer *rb)
{
    LCDeleteResult res = {-1, 0, 0, 0};
    if (gslot < 0 || gslot >= (int)LCGW_FILE_SLOTS) return res;
    if (!lcgw_files[gslot].open) return res;

    LCGWFile *gf = &lcgw_files[gslot];

    /* triple-cut: ghost all chunks but keep raw data */
    for (uint32_t ci = 0; ci < gf->chunk_count; ci++)
        gf->chunks[ci].ghosted = 1;

    /* log rewind entry */
    if (rb && rb->count < LC_REWIND_SLOTS)
        rb->entries[rb->count++] = (uint32_t)gslot;

    res.status = 0;
    res.cut_a  = (uint32_t)gslot;
    res.cut_b  = gf->chunk_count;
    res.cut_c  = 0xDEADu;
    return res;
}

/* ── palette (used by lcgw_read_payload) ─────────────────────── */
typedef struct {
    uint32_t entries[4];
    uint32_t count;
} LCPalette;

/* ── read_payload: NULL if ghosted, ptr to raw if valid ─────── */
static inline const uint8_t *lcgw_read_payload(int gslot,
                                                 uint32_t chunk_idx,
                                                 LCPalette *pal_out)
{
    if (pal_out) memset(pal_out, 0, sizeof(*pal_out));
    if (gslot < 0 || gslot >= (int)LCGW_FILE_SLOTS) return NULL;
    LCGWFile *gf = &lcgw_files[gslot];
    if (!gf->open) return NULL;
    if (chunk_idx >= gf->chunk_count) return NULL;
    LCGWChunk *ch = &gf->chunks[chunk_idx];
    if (!ch->valid || ch->ghosted) return NULL;   /* present but unreachable */
    return ch->raw;
}

/* ── rewind: undo ghost for a slot recorded in RewindBuffer ──── *
 * For each chunk: clears .ghosted flag, reconstructs raw from    *
 * the seed stored in lane->seeds[ci] + write_count_at_build.     *
 * Caller must supply the seed array and base write_count.        *
 * Returns 0=ok, -1=slot not found in rb or not ghosted.          *
 * ─────────────────────────────────────────────────────────────── */
static inline int lcgw_rewind(int gslot, RewindBuffer *rb,
                               const uint64_t *seeds,   /* LCGW_GROUND_SLOTS */
                               uint32_t        base_wc) /* write_count at first chunk */
{
    if (gslot < 0 || gslot >= (int)LCGW_FILE_SLOTS) return -1;
    if (!rb) return -1;

    /* verify gslot is in the rewind buffer */
    int found = 0;
    for (uint8_t i = 0; i < rb->count; i++) {
        if ((int)rb->entries[i] == gslot) { found = 1; break; }
    }
    if (!found) return -1;

    LCGWFile *gf = &lcgw_files[gslot];
    if (!gf->open) return -1;

    /* restore each chunk: rebuild raw, clear ghosted */
    for (uint32_t ci = 0; ci < gf->chunk_count; ci++) {
        LCGWChunk *ch = &gf->chunks[ci];
        if (!ch->valid) continue;       /* never written — skip */
        lcgw_build_from_seed(ch->raw, seeds[ci], base_wc + ci);
        ch->ghosted = 0;                /* path restored */
    }

    /* remove entry from RewindBuffer (shift left) */
    for (uint8_t i = 0; i < rb->count; i++) {
        if ((int)rb->entries[i] == gslot) {
            for (uint8_t j = i; j + 1 < rb->count; j++)
                rb->entries[j] = rb->entries[j + 1];
            rb->count--;
            break;
        }
    }
    return 0;
}

/* ── reset: close all file slots (bench use only) ────────────── */
static inline void lcgw_reset(void)
{
    memset(lcgw_files, 0, sizeof(lcgw_files));
    _lcgw_initialized = 0;
}

#endif /* LC_HDR_H */

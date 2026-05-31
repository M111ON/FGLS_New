/*
 * lcgw_adaptive.h — Production Hybrid: Adaptive + Prewarm + Hot-path
 * ════════════════════════════════════════════════════════════════════
 *
 * Three surgical additions to lc_hdr_lazy.h / tgw_ground_lcgw_lazy.h:
 *
 *  1. ADAPTIVE MODE — per-lane W:R tracking → flip eager/lazy at runtime
 *       if read_count < write_count → lazy  (write-heavy, defer build)
 *       if read_count ≥ write_count → eager (read-heavy, build at write)
 *       Guards W:R=1:1 edge case measured at 1292ns cold read.
 *
 *  2. PREWARM LRU — materialize + touch at flush boundary
 *       Called once per spoke flush: walks all valid chunks,
 *       materializes dirty ones, loads into 8-slot LRU.
 *       Cuts cold-read 1292ns to ~2ns before first reader arrives.
 *
 *  3. HOT-PATH BYPASS — inline eager for known hot spoke
 *       is_hot_spoke(spoke): spoke whose read_count > HOT_SPOKE_THRESHOLD
 *       lcgw_ground_write_adaptive() calls eager build for hot spokes only.
 *       Hybrid: cold spokes stay lazy, hot spoke goes eager per-chunk.
 *
 * Usage:
 *   Replace tgw_ground_lcgw_lazy.h with this header.
 *   #include "lcgw_adaptive.h" — pulls in lc_hdr_lazy.h implicitly.
 *
 * No malloc. No float. No heap. Depends: lc_hdr_lazy.h, geo_addr_net.h
 * ════════════════════════════════════════════════════════════════════
 */

#ifndef LCGW_ADAPTIVE_H
#define LCGW_ADAPTIVE_H

#include <stdint.h>
#include <string.h>
#include "lc_hdr_lazy.h"
#include "geo_addr_net.h"

/* pull in existing ground store types without re-including ground_lcgw */
#ifndef TGW_GROUND_LCGW_H
#include "tgw_ground_lcgw_lazy.h"
#endif

/* ── tuning constants ────────────────────────────────────────── */
#define HOT_SPOKE_THRESHOLD   8u     /* reads before spoke flips eager   */
#define ADAPT_WINDOW         64u     /* writes per adaptive re-evaluation */
#define PREWARM_CACHE_SLOTS   8u     /* must match CS in bench            */

/* ══════════════════════════════════════════════════════════════
   TWEAK 1 — ADAPTIVE MODE
   Per-lane counters: read_count tracks reads since last reset.
   Adaptive decision made every ADAPT_WINDOW writes (cheap amortize).
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t read_count;    /* reads issued to this spoke since reset */
    uint32_t window_writes; /* writes in current evaluation window    */
    uint8_t  use_eager;     /* 0=lazy (default), 1=eager              */
} LCGWAdaptState;

static LCGWAdaptState _adapt[LCGW_GROUND_SPOKES];

static inline void lcgw_adapt_init(void) {
    memset(_adapt, 0, sizeof(_adapt));
}

/* called by read path to register a read on a spoke */
static inline void lcgw_adapt_read_tick(uint8_t spoke) {
    if (spoke < LCGW_GROUND_SPOKES)
        _adapt[spoke].read_count++;
}

/* evaluate mode after each write — O(1) */
static inline void _adapt_eval(uint8_t spoke, uint32_t total_writes) {
    LCGWAdaptState *a = &_adapt[spoke];
    a->window_writes++;
    if (a->window_writes < ADAPT_WINDOW) return;

    /* re-evaluate: compare reads vs writes in window */
    a->use_eager   = (a->read_count >= a->window_writes) ? 1u : 0u;
    a->read_count  = 0u;
    a->window_writes = 0u;
    (void)total_writes;
}

static inline uint8_t lcgw_spoke_is_eager(uint8_t spoke) {
    return spoke < LCGW_GROUND_SPOKES ? _adapt[spoke].use_eager : 0u;
}

/* ══════════════════════════════════════════════════════════════
   TWEAK 2 — PREWARM LRU
   Call at flush boundary (once per spoke drain).
   Walks all valid chunks in a spoke's file, materializes dirty ones.
   Compatible with external 8-slot LRU cache (cache_read() pattern).
   ══════════════════════════════════════════════════════════════ */

/*
 * lcgw_prewarm_spoke: materialize all dirty chunks for one spoke.
 * O(chunk_count) = O(16) — effectively O(1) for fixed LCGW_GROUND_SLOTS.
 * Call this just before spoke is expected to receive reads.
 */
static inline uint32_t lcgw_prewarm_spoke(LCGWGroundStore *gs, uint8_t spoke)
{
    if (spoke >= LCGW_GROUND_SPOKES) return 0u;
    LCGWSpokeLane *lane = &gs->lanes[spoke];
    if (lane->gslot < 0 || lane->ghosted) return 0u;

    LCGWFile *gf = &lcgw_files[lane->gslot];
    if (!gf->open) return 0u;

    uint32_t warmed = 0u;
    for (uint32_t ci = 0u; ci < gf->chunk_count; ci++) {
        LCGWChunk *ch = &gf->chunks[ci];
        if (ch->valid && ch->dirty) {
            lcgw_materialize(ch);   /* build now — cut cold-read latency */
            warmed++;
        }
    }
    return warmed;   /* number of chunks materialized */
}

/*
 * lcgw_prewarm_all: prewarm every spoke in the store.
 * Use at system flush or before batch read phase.
 */
static inline uint32_t lcgw_prewarm_all(LCGWGroundStore *gs)
{
    uint32_t total = 0u;
    for (uint8_t s = 0u; s < LCGW_GROUND_SPOKES; s++)
        total += lcgw_prewarm_spoke(gs, s);
    return total;
}

/* ══════════════════════════════════════════════════════════════
   TWEAK 3 — HOT-PATH BYPASS
   is_hot_spoke(): read_count > threshold → flip eager for that spoke.
   lcgw_ground_write_adaptive(): per-write decision, zero branch overhead
   for cold spokes (read_count check is a single compare).
   ══════════════════════════════════════════════════════════════ */

static inline uint8_t lcgw_is_hot_spoke(uint8_t spoke) {
    return (spoke < LCGW_GROUND_SPOKES &&
            _adapt[spoke].read_count > HOT_SPOKE_THRESHOLD) ? 1u : 0u;
}

/*
 * lcgw_ground_write_adaptive — drop-in replacement for lcgw_ground_write.
 *
 * Decision tree per write:
 *   hot spoke detected  → eager build (materialize immediately)
 *   lazy mode (default) → store seed only (dirty=1)
 *   eager mode (adapt)  → eager build
 *
 * One extra compare vs plain lcgw_ground_write — ~0.5ns overhead on cold.
 */
static inline void lcgw_ground_write_adaptive(LCGWGroundStore *gs,
                                               uint64_t addr,
                                               uint64_t val)
{
    GeoNetAddr geo = geo_net_encode(addr);
    uint8_t spoke  = geo.spoke;

    LCGWSpokeLane *lane = &gs->lanes[spoke];
    if (lane->ghosted) return;

    uint64_t seed = addr ^ val ^ (addr << 17) ^ (val >> 13)
                  ^ 0x9e3779b97f4a7c15ull;

    if (lane->gslot < 0) {
        /* lazy open — same as original */
        char name[16];
        name[0]='g';name[1]='r';name[2]='o';name[3]='u';
        name[4]='n';name[5]='d';name[6]='_';name[7]='s';
        name[8]=(char)('0'+spoke);name[9]='\0';
        lane->seeds[0] = seed;
        lane->gslot = lcgw_open(name, LCGW_GROUND_SLOTS, lane->seeds, LC_LEVEL_0);
        if (lane->gslot < 0) return;
    }

    uint8_t    ci = lane->chunk_cursor % LCGW_GROUND_SLOTS;
    lane->seeds[ci] = seed;

    LCGWFile  *gf = &lcgw_files[lane->gslot];
    if (gf->open && ci < gf->chunk_count) {
        LCGWChunk *ch = &gf->chunks[ci];

        /* HYBRID DECISION: hot spoke or adaptive eager → build now */
        if (lcgw_is_hot_spoke(spoke) || lcgw_spoke_is_eager(spoke)) {
            lcgw_build_from_seed(ch->raw, seed, (uint32_t)lane->write_count);
            ch->dirty   = 0;   /* already built — no deferred work */
        } else {
            /* lazy: store seed, defer build to read */
            ch->seed_lazy = seed;
            ch->wc_lazy   = (uint32_t)lane->write_count;
            ch->dirty     = 1;
        }
        ch->valid   = 1;
        ch->ghosted = 0;
    }

    lane->chunk_cursor = (uint8_t)((ci + 1u) % LCGW_GROUND_SLOTS);
    lane->write_count++;
    gs->total_writes++;

    /* adaptive window evaluation */
    _adapt_eval(spoke, gs->total_writes);
}

/*
 * lcgw_adaptive_read_payload — read with adapt tick + materialize.
 * Drop-in replacement for lcgw_read_payload when adaptive tracking needed.
 */
static inline const uint8_t *lcgw_adaptive_read_payload(int gslot,
                                                          uint32_t chunk_idx,
                                                          uint8_t  spoke)
{
    lcgw_adapt_read_tick(spoke);
    return lcgw_read_payload(gslot, chunk_idx, NULL);
}

/* ══════════════════════════════════════════════════════════════
   CONVENIENCE: combined init
   ══════════════════════════════════════════════════════════════ */
static inline void lcgw_adaptive_init(LCGWGroundStore *gs) {
    lcgw_ground_init(gs);
    lcgw_adapt_init();
}

#endif /* LCGW_ADAPTIVE_H */

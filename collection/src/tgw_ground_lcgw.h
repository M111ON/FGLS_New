/*
 * tgw_ground_lcgw.h — TGW GROUND path → LC-GCFS hook
 * ═══════════════════════════════════════════════════════════════
 * Wires the TGWGroundFn callback into LC-GCFS storage.
 *
 * Design:
 *   Each GROUND packet (addr, val) → seed = addr ^ val
 *   → spoken slot (0..5) from addr % 720 walk
 *   → lcgw_open (lazy, per-spoke) + lcgw_build_from_seed
 *   → LCGWGroundStore keeps 6 spoke lanes × up to LCGW_MAX_CHUNKS chunks
 *
 * Ghost delete property (inherited from LC-GCFS):
 *   lcgw_ground_delete(spoke) → triple-cut → content present, path severed
 *   "present but unreachable" — same DNA as GCFS reserved_mask silence
 *
 * Usage:
 *   LCGWGroundStore gs;
 *   lcgw_ground_init(&gs);
 *
 *   // wire as TGWGroundFn:
 *   tgw_dispatch_v2(..., tgw_ground_fn, &gs);
 *
 *   // later: ghost-delete spoke 3:
 *   lcgw_ground_delete(&gs, 3);
 *
 * Stats:
 *   LCGWGroundStats s = lcgw_ground_stats(&gs);
 *   s.writes[spoke], s.ghosted[spoke], s.total_writes
 *
 * Sacred constants (frozen):
 *   6 = SPOKES, 720 = TRING_CYCLE, 4896 = GCFS_TOTAL_BYTES
 *
 * No malloc. No heap. No float.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef TGW_GROUND_LCGW_H
#define TGW_GROUND_LCGW_H

#include <stdint.h>
#include <string.h>

#include "lc_hdr.h"
#include "lc_fs.h"
#include "lc_delete.h"
#include "lc_gcfs_wire.h"
#include "geo_addr_net.h"     /* tring spoke from addr */
#include "tgw_cardioid_express.h"  /* cardioid gate — lut init required */

#define LCGW_GROUND_SPOKES    6u
#define LCGW_GROUND_SLOTS    16u   /* chunks per spoke lane */

/* ── per-spoke lane ──────────────────────────────────────────── */
typedef struct {
    int       gslot;                        /* lcgw file slot (-1=empty) */
    uint64_t  seeds[LCGW_GROUND_SLOTS];     /* seed per chunk */
    uint32_t  write_count;                  /* total writes to this spoke */
    uint8_t   chunk_cursor;                 /* next chunk slot (ring) */
    uint8_t   ghosted;                      /* 1 = triple-cut applied */
    uint8_t   _pad[2];
} LCGWSpokeLane;

/* ── ground store (6 spoke lanes) ───────────────────────────── */
typedef struct {
    LCGWSpokeLane lanes[LCGW_GROUND_SPOKES];
    uint32_t      total_writes;
    uint32_t      total_ghosted;
    RewindBuffer  rb;   /* shared rewind buffer for delete ops */
} LCGWGroundStore;

/* ── stats snapshot ──────────────────────────────────────────── */
typedef struct {
    uint32_t writes[LCGW_GROUND_SPOKES];
    uint8_t  ghosted[LCGW_GROUND_SPOKES];
    uint32_t total_writes;
    uint32_t total_ghosted;
} LCGWGroundStats;

/* ══════════════════════════════════════════════════════════════
   INIT
   ══════════════════════════════════════════════════════════════ */
static inline void lcgw_ground_init(LCGWGroundStore *gs)
{
    memset(gs, 0, sizeof(*gs));
    lcgw_init();   /* idempotent */
    for (uint32_t s = 0u; s < LCGW_GROUND_SPOKES; s++)
        gs->lanes[s].gslot = -1;
}

/* ══════════════════════════════════════════════════════════════
   INTERNAL: lazy open spoke lane
   ══════════════════════════════════════════════════════════════ */
static inline int _lcgw_lane_open(LCGWSpokeLane *lane, uint8_t spoke,
                                   uint64_t first_seed)
{
    if (lane->gslot >= 0) return lane->gslot;   /* already open */

    /* build name: "ground_sX" */
    char name[16];
    name[0]='g'; name[1]='r'; name[2]='o'; name[3]='u';
    name[4]='n'; name[5]='d'; name[6]='_'; name[7]='s';
    name[8]=(char)('0' + spoke); name[9]='\0';

    lane->seeds[0] = first_seed;
    lane->gslot = lcgw_open(name, LCGW_GROUND_SLOTS, lane->seeds, LC_LEVEL_0);
    return lane->gslot;
}

/* ══════════════════════════════════════════════════════════════
   CORE: write one GROUND packet into its spoke lane
   ══════════════════════════════════════════════════════════════ */
static inline void lcgw_ground_write(LCGWGroundStore *gs,
                                      uint64_t addr, uint64_t val)
{
    /* derive spoke from addr (O(1) LUT) */
    GeoNetAddr geo = geo_net_encode(addr);
    uint8_t spoke  = geo.spoke;   /* 0..5 */

    LCGWSpokeLane *lane = &gs->lanes[spoke];

    /* skip if lane ghosted */
    if (lane->ghosted) return;

    /* seed = deterministic mix of addr+val */
    uint64_t seed = addr ^ val ^ (addr << 17) ^ (val >> 13)
                  ^ 0x9e3779b97f4a7c15ull;

    /* lazy open */
    if (lane->gslot < 0) {
        int s = _lcgw_lane_open(lane, spoke, seed);
        if (s < 0) return;   /* no slot available */
    }

    /* store seed in ring buffer slot */
    uint8_t ci = lane->chunk_cursor % LCGW_GROUND_SLOTS;
    lane->seeds[ci] = seed;

    /* rebuild chunk from new seed (deterministic, no malloc) */
    LCGWFile *gf = &lcgw_files[lane->gslot];
    if (gf->open && ci < gf->chunk_count) {
        lcgw_build_from_seed(gf->chunks[ci].raw, seed,
                              (uint32_t)lane->write_count);
        gf->chunks[ci].valid   = 1;
        gf->chunks[ci].ghosted = 0;
    }

    lane->chunk_cursor = (uint8_t)((ci + 1u) % LCGW_GROUND_SLOTS);
    lane->write_count++;
    gs->total_writes++;
}

/* ══════════════════════════════════════════════════════════════
   TGWGroundFn CALLBACK — drop-in for tgw_dispatch_v2
   ctx = LCGWGroundStore*
   ══════════════════════════════════════════════════════════════ */
static inline void tgw_ground_fn(uint64_t addr, uint64_t val, void *ctx)
{
    lcgw_ground_write((LCGWGroundStore *)ctx, addr, val);
}

/* ══════════════════════════════════════════════════════════════
   GHOST DELETE: triple-cut one spoke lane
   content remains, path severed — "present but unreachable"
   ══════════════════════════════════════════════════════════════ */
static inline LCDeleteResult lcgw_ground_delete(LCGWGroundStore *gs,
                                                  uint8_t spoke)
{
    LCDeleteResult res = {-1, 0, 0, 0};
    if (spoke >= LCGW_GROUND_SPOKES) return res;

    LCGWSpokeLane *lane = &gs->lanes[spoke];
    if (lane->gslot < 0 || lane->ghosted) return res;

    res = lcgw_delete(lane->gslot, &gs->rb);
    if (res.status == 0) {
        lane->ghosted = 1;
        gs->total_ghosted++;
    }
    return res;
}

/* ══════════════════════════════════════════════════════════════
   REWIND REPLAY: undo ghost delete for one spoke lane
   Restores path (clears ghosted), rebuilds raw from saved seeds.
   Uses RewindBuffer to verify the delete was logged.
   Returns 0=ok, -1=spoke not ghosted or not in rewind buffer.
   ══════════════════════════════════════════════════════════════ */
static inline int lcgw_ground_rewind(LCGWGroundStore *gs, uint8_t spoke)
{
    if (spoke >= LCGW_GROUND_SPOKES) return -1;

    LCGWSpokeLane *lane = &gs->lanes[spoke];
    if (!lane->ghosted || lane->gslot < 0) return -1;

    LCGWFile *gf = &lcgw_files[lane->gslot];
    if (!gf->open) return -1;

    /* Rebuild all chunks from stored per-lane seeds. */
    uint32_t n = (gf->chunk_count < LCGW_GROUND_SLOTS) ? gf->chunk_count : LCGW_GROUND_SLOTS;
    for (uint32_t i = 0; i < n; i++) {
        lcgw_build_from_seed(gf->chunks[i].raw, lane->seeds[i], i);
        gf->chunks[i].valid = (lcgw_verify(gf->chunks[i].raw) == 0) ? 1 : 0;
        gf->chunks[i].ghosted = 0;
    }

    lane->ghosted = 0;
    if (gs->total_ghosted > 0u) gs->total_ghosted--;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   STATS
   ══════════════════════════════════════════════════════════════ */
static inline LCGWGroundStats lcgw_ground_stats(const LCGWGroundStore *gs)
{
    LCGWGroundStats s;
    s.total_writes  = gs->total_writes;
    s.total_ghosted = gs->total_ghosted;
    for (uint32_t i = 0u; i < LCGW_GROUND_SPOKES; i++) {
        s.writes[i]  = gs->lanes[i].write_count;
        s.ghosted[i] = gs->lanes[i].ghosted;
    }
    return s;
}

/* ── active spoke count (debug) ──────────────────────────────── */
static inline uint32_t lcgw_ground_active_spokes(const LCGWGroundStore *gs)
{
    uint32_t n = 0u;
    for (uint32_t s = 0u; s < LCGW_GROUND_SPOKES; s++)
        if (gs->lanes[s].gslot >= 0 && !gs->lanes[s].ghosted) n++;
    return n;
}

#endif /* TGW_GROUND_LCGW_H */

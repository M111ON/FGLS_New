/*
 * tgw_fgls_connector.h — TGW Dispatch → FGLS Twin Store Bridge
 * ════════════════════════════════════════════════════════════
 * Connects bond-layer TGW dispatch (shape-driven routing) to
 * FGLS geometric storage (GiantArray → CubeFileStore).
 *
 * Flow:
 *   PoglsPiece → tgw_dispatch() → TgwDispatchResult
 *     ├── ROUTE (I/O/T/J): shape → DodecaEntry → fts_write()
 *     │     geo_key → merkle_root, bond_key → sha256_hi/lo
 *     │     shape → offset, reroute_count → hop_count
 *     │     polarity → segment
 *     │     → GiantArray[coset].faces[face].core[level]
 *     │     → CubeFileStore 4896B flat file
 *     │
 *     └── GROUND (S/Z/L):   counted, slot marked, no storage yet
 *           (LC-GCFS C implementation pending — vaulted Python only)
 *
 * Design: stateless dispatch + stateful FGLS store.
 *   TgwFglsCtx owns both the dispatch ring and the FGLS storage array.
 *   Caller calls tgw_fgls_tick() each cycle for O-latch release.
 *   Call tgw_fgls_serialize() to freeze → 4896B buffer for disk write.
 *
 * No malloc, no float, O(1) per op.
 * ════════════════════════════════════════════════════════════
 */

#ifndef TGW_FGLS_CONNECTOR_H
#define TGW_FGLS_CONNECTOR_H

#include <stdint.h>
#include <string.h>
/* ── Fix Mingw _rotl64 conflict ──────────────────────────────
 * Mingw's <stdlib.h> declares _rotl64 as extern; geo_primitives.h
 * defines it static inline. Rename to avoid redefinition.
 * Must come before ALL includes (including bond → config → stdlib). */
#ifdef __MINGW32__
#define _rotl64 _POGLS_rotl64
#endif

/* Core engine types (must precede fgls_twin_store.h) */
#include "core/core/geo_dodeca.h"
#include "core/core/geo_apex_wire.h"
#include "core/core/geo_primitives.h"
#include "core/core/pogls_fibo_addr.h"
#include "core/core/geo_giant_array.h"
#include "core/core/geo_cube_file_store.h"

/* Undo rename before bond layer includes <stdlib.h> */
#ifdef __MINGW32__
#undef _rotl64
#endif

/* Undefine to avoid POGLS_PATH_SEP redefinition conflict
   (pogls_platform.h defines as string, pogls_config.h as char) */
#undef POGLS_PATH_SEP

/* Bond layer + TGW dispatch */
#include "tgw_bond_dispatch.h"

/* FGLS Twin Store */
#include "core/pogls_engine/fgls_twin_store.h"

/* ── Connector result ───────────────────────────────────────── */
typedef struct {
    TgwDispatchResult dispatch;
    int              fts_rc;     /* fts_write return: 0=ok -1=deleted -2=null */
    uint64_t         addr;       /* geo_key used as address                    */
    uint64_t         value;      /* bond_key used as value                     */
    FtsTritAddr      trit_addr;  /* resolved geometric position                */
} TgwFglsResult;

/* ── Connector context ──────────────────────────────────────── */
typedef struct {
    TgwDispatchCtx dispatch;     /* 720-slot TRing dispatch                    */
    FtsTwinStore   store;        /* FGLS GiantArray + CubeFileStore            */
    uint32_t       routed_count; /* lifetime ROUTE dispatches persisted        */
    uint32_t       grounded_count; /* GROUND dispatches (LC-GCFS pending)      */
    uint32_t       serialized_count;
} TgwFglsCtx;

/* ── Stats ───────────────────────────────────────────────────── */
typedef struct {
    TgwStats       dispatch;
    FtsTwinStats   storage;
    uint32_t       routed;
    uint32_t       grounded;
    uint32_t       serialized;
} TgwFglsStats;

/* ════════════════════════════════════════════════════════════════
   INIT
   ════════════════════════════════════════════════════════════════ */
static inline void tgw_fgls_init(TgwFglsCtx *ctx,
                                  uint64_t    session_nonce,
                                  uint64_t    root_seed)
{
    memset(ctx, 0, sizeof(*ctx));
    tgw_dispatch_init(&ctx->dispatch, session_nonce);
    fts_init(&ctx->store, root_seed);
}

/* ════════════════════════════════════════════════════════════════
   PIECE → DODECA ENTRY (canonical mapping)
   ════════════════════════════════════════════════════════════════ */
static inline DodecaEntry tgw_fgls_entry_from_piece(
    const PoglsPiece *piece,
    const PoglsSlot  *slot,
    uint8_t           polarity)
{
    uint64_t bk = pogls_bond_key(piece);
    DodecaEntry e;
    memset(&e, 0, sizeof(e));
    e.merkle_root = piece->geo_key;          /* geometry fingerprint     */
    e.sha256_hi   = bk;                      /* high = bond_key          */
    e.sha256_lo   = bk;                      /* low  = bond_key          */
    e.offset      = piece->shape;            /* shape byte = offset      */
    e.hop_count   = slot ? slot->rerouted : 0; /* reroute count = depth  */
    e.segment     = polarity;                /* 0=ROUTE 1=GROUND         */
    e.ref_count   = 1;
    return e;
}

/* ════════════════════════════════════════════════════════════════
   PIECE → FGLS WRITE (through TGW dispatch)
   ════════════════════════════════════════════════════════════════
   Full pipeline:
     1. bond_verify(slot_a, slot_b) via tgw_dispatch()
     2. shape → polarity → tring_pos
     3. For ROUTE: build DodecaEntry → fts_write(addr=geo_key, value=bond_key)
     4. For GROUND: count, no storage (LC-GCFS pending)
   ════════════════════════════════════════════════════════════════ */
static inline TgwFglsResult tgw_fgls_write(
    TgwFglsCtx      *ctx,
    PoglsSlot       *slot_a,
    PoglsSlot       *slot_b)
{
    TgwFglsResult r;
    memset(&r, 0, sizeof(r));

    /* Step 1: TGW dispatch — shape routing + bond verify */
    r.dispatch = tgw_dispatch(&ctx->dispatch, slot_a, slot_b, NULL, 0);

    /* Step 2: extract geometric key pair */
    PoglsPiece *p  = &slot_a->piece;
    r.addr  = p->geo_key;
    r.value = pogls_bond_key(p);

    /* Step 3: FGLS storage for ROUTE polarity */
    const TgwTRingSlot *ts = tgw_find_slot(&ctx->dispatch, r.value,
                        tgw_shape_polarity(p->shape));
    if (!ts) return r; /* slot not yet committed */

    if (ts->polarity == TGW_POLARITY_ROUTE) {
        DodecaEntry e = tgw_fgls_entry_from_piece(p, slot_a, ts->polarity);
        r.fts_rc  = fts_write(&ctx->store, r.addr, r.value, &e);
        r.trit_addr = fts_trit_addr(r.addr, r.value);
        ctx->routed_count++;
    } else {
        /* GROUND — LC-GCFS not yet implemented in C */
        r.fts_rc = 0;
        ctx->grounded_count++;
    }

    return r;
}

/* ════════════════════════════════════════════════════════════════
   BATCH WRITE via TGW dispatch + FGLS
   ════════════════════════════════════════════════════════════════ */
static inline void tgw_fgls_batch(
    TgwFglsCtx      *ctx,
    PoglsSlot       *slots_a,
    PoglsSlot       *slots_b,
    uint32_t         n)
{
    for (uint32_t i = 0; i < n; i++)
        tgw_fgls_write(ctx, &slots_a[i], &slots_b[i]);
}

/* ════════════════════════════════════════════════════════════════
   WRITE FROM RAW ADDR + VALUE (skip dispatch, direct FGLS)
   ════════════════════════════════════════════════════════════════
   Convenience: directly store (addr, value) → FGLS without
   going through full bond + TGW dispatch. Uses shape='I' (pipe).
   Useful for bulk loading or replay. Writes polarity=ROUTE.
   ════════════════════════════════════════════════════════════════ */
static inline int tgw_fgls_store_raw(
    TgwFglsCtx      *ctx,
    uint64_t          addr,
    uint64_t          value,
    uint8_t           shape)
{
    DodecaEntry e;
    memset(&e, 0, sizeof(e));
    e.merkle_root = addr;
    e.sha256_hi   = value;
    e.sha256_lo   = value;
    e.offset      = shape;
    e.hop_count   = 0;
    e.segment     = TGW_POLARITY_ROUTE;
    e.ref_count   = 1;

    int rc = fts_write(&ctx->store, addr, value, &e);
    if (rc == 0) ctx->routed_count++;
    return rc;
}

/* ════════════════════════════════════════════════════════════════
   O-LATCH TICK
   ════════════════════════════════════════════════════════════════
   Call once per dispatch cycle to advance O-shape hold counters.
   Released slots are forwarded to FGLS on next write.
   ════════════════════════════════════════════════════════════════ */
static inline uint32_t tgw_fgls_tick(TgwFglsCtx *ctx)
{
    return tgw_tick(&ctx->dispatch);
}

/* ════════════════════════════════════════════════════════════════
   SERIALIZE → 4896B CubeFileStore buffer
   ════════════════════════════════════════════════════════════════ */
static inline void tgw_fgls_serialize(
    TgwFglsCtx *ctx,
    uint8_t     out[GCFS_TOTAL_BYTES])
{
    fts_serialize(&ctx->store);
    fts_write_buf(&ctx->store, out);
    ctx->serialized_count++;
}

/* ════════════════════════════════════════════════════════════════
   GET STATS
   ════════════════════════════════════════════════════════════════ */
static inline TgwFglsStats tgw_fgls_stats(const TgwFglsCtx *ctx)
{
    TgwFglsStats s;
    s.dispatch    = tgw_get_stats(&ctx->dispatch);
    s.storage     = fts_stats(&ctx->store);
    s.routed      = ctx->routed_count;
    s.grounded    = ctx->grounded_count;
    s.serialized  = ctx->serialized_count;
    return s;
}

/* ════════════════════════════════════════════════════════════════
   STATUS PRINT
   ════════════════════════════════════════════════════════════════ */
#include <stdio.h>
static inline void tgw_fgls_status(const TgwFglsCtx *ctx)
{
    TgwStats ds = tgw_get_stats(&ctx->dispatch);
    FtsTwinStats fs = fts_stats(&ctx->store);
    printf("[TGW↔FGLS]\n");
    printf("  dispatch: active=%u  dispatched=%llu  grounded=%llu  quarantined=%llu\n",
           ds.active_count,
           (unsigned long long)ds.total_dispatched,
           (unsigned long long)ds.total_grounded,
           (unsigned long long)ds.total_quarantined);
    printf("  fgls: writes=%u  deletes=%u  overflows=%u  active_cosets=%u\n",
           fs.writes, fs.deletes, fs.overflows, fs.active_cosets);
    printf("  bridge: routed=%u  grounded=%u  serialized=%u\n",
           ctx->routed_count, ctx->grounded_count, ctx->serialized_count);
}

#endif /* TGW_FGLS_CONNECTOR_H */

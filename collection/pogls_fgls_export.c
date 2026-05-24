/*
 * pogls_fgls_export.c — DLL export wrapper for tgw_fgls_connector.h
 * ════════════════════════════════════════════════════════════════════
 *
 * Exports:
 *   pogls_fgls_alloc()          → allocate TgwFglsCtx on heap
 *   pogls_fgls_free(ctx)        → free
 *   pogls_fgls_init(ctx, nonce, root_seed)
 *   pogls_fgls_store_raw(ctx, addr, value, shape) → int
 *   pogls_fgls_tick(ctx)        → uint32 (released O-latches)
 *   pogls_fgls_serialize(ctx, out_4896B)
 *   pogls_fgls_routed_count(ctx)   → uint32
 *   pogls_fgls_grounded_count(ctx) → uint32
 *   pogls_fgls_stats(ctx, out_stats*)
 *   pogls_fgls_version()        → const char*
 *
 * Compile (Windows MinGW):
 *   gcc -O2 -shared -DPOGLS_FGLS_EXPORT_DLL
 *       -I. -Icore -Icore/core -Icore/pogls_engine
 *       -o pogls_fgls.dll pogls_fgls_export.c
 *       -Wl,--out-implib,libpogls_fgls.a
 *
 * Compile (Linux .so for testing):
 *   gcc -O2 -shared -fPIC -DPOGLS_FGLS_EXPORT_DLL
 *       -I. -Icore -Icore/core -Icore/pogls_engine
 *       -o pogls_fgls.so pogls_fgls_export.c
 *
 * NOTE: TgwFglsCtx includes FtsTwinStore (large struct).
 *       Heap-allocated via pogls_fgls_alloc() — caller must free.
 * ════════════════════════════════════════════════════════════════════
 */

/* Mingw _rotl64 conflict — must be first */
#ifdef __MINGW32__
#define _rotl64 _POGLS_rotl64
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* Pull in full connector (includes all deps) */
#include "tgw_fgls_connector.h"

#ifdef __MINGW32__
#undef _rotl64
#endif

/* ── DLL export macro ──────────────────────────────────────── */
#ifdef POGLS_FGLS_EXPORT_DLL
  #ifdef _WIN32
    #define FGLS_API __declspec(dllexport)
  #else
    #define FGLS_API __attribute__((visibility("default")))
  #endif
#else
  #define FGLS_API
#endif

/* ── Exported stats struct (flat, no internal deps) ─────────── */
typedef struct {
    uint32_t routed;
    uint32_t grounded;
    uint32_t serialized;
    uint32_t dispatched;
    uint32_t grounded_dispatch;
    uint32_t quarantined;
    uint32_t fgls_writes;
    uint32_t fgls_deletes;
    uint32_t fgls_overflows;
    uint32_t active_cosets;
} PoglsFglsStats;

/* ════════════════════════════════════════════════════════════
   LIFECYCLE
   ════════════════════════════════════════════════════════════ */

FGLS_API void *pogls_fgls_alloc(void)
{
    TgwFglsCtx *ctx = (TgwFglsCtx *)calloc(1, sizeof(TgwFglsCtx));
    return ctx;
}

FGLS_API void pogls_fgls_free(void *ctx)
{
    free(ctx);
}

FGLS_API void pogls_fgls_init(void    *ctx,
                                uint64_t session_nonce,
                                uint64_t root_seed)
{
    if (!ctx) return;
    tgw_fgls_init((TgwFglsCtx *)ctx, session_nonce, root_seed);
}

/* ── Read entry struct ──────────────────────────────────── */
typedef struct {
    uint64_t merkle_root;
    uint64_t sha256_hi;
    uint32_t offset;
    uint32_t hop_count;
    uint8_t  segment;
    uint8_t  found;      /* 1=found, 0=no data at this trit address */
    uint8_t  coset;
    uint8_t  face;
    uint8_t  level;
    uint8_t  pad[3];
} PoglsFglsReadEntry;

/* ════════════════════════════════════════════════════════════
   CORE: store_raw — primary Python-facing write
   ════════════════════════════════════════════════════════════ */

FGLS_API int pogls_fgls_store_raw(void    *ctx,
                                   uint64_t addr,
                                   uint64_t value,
                                   uint8_t  shape)
{
    if (!ctx || addr == 0) return -2;
    return tgw_fgls_store_raw((TgwFglsCtx *)ctx, addr, value, shape);
}

FGLS_API void pogls_fgls_read(void *ctx, uint64_t addr, uint64_t value,
                               PoglsFglsReadEntry *out)
{
    if (!ctx || !out) return;
    memset(out, 0, sizeof(*out));
    TgwFglsCtx *c = (TgwFglsCtx *)ctx;
    FtsTritAddr ta = fts_trit_addr(addr, value);
    out->coset = ta.coset;
    out->face  = ta.face;
    out->level = ta.level;
    if (ta.coset >= 12u || ta.face >= 6u) return;
    FrustumSlot64 *slot = &c->store.ga.cubes[ta.coset].faces[ta.face];
    out->merkle_root = slot->core[ta.level];
    out->sha256_hi   = slot->core[(ta.level + 1u) % 4u];
    out->offset      = slot->addr[ta.level];
    out->hop_count   = slot->addr[(ta.level + 1u) % 4u];
    out->segment     = slot->world[ta.level];
    out->found       = (slot->core[ta.level] == addr &&
                        slot->core[(ta.level + 1u) % 4u] == value) ? 1 : 0;
}

/* ════════════════════════════════════════════════════════════
   TICK — advance O-latch counters
   ════════════════════════════════════════════════════════════ */

FGLS_API uint32_t pogls_fgls_tick(void *ctx)
{
    if (!ctx) return 0;
    return tgw_fgls_tick((TgwFglsCtx *)ctx);
}

/* ════════════════════════════════════════════════════════════
   SERIALIZE → 4896B flat buffer
   ════════════════════════════════════════════════════════════ */

FGLS_API void pogls_fgls_serialize(void *ctx, uint8_t *out_buf)
{
    if (!ctx || !out_buf) return;
    tgw_fgls_serialize((TgwFglsCtx *)ctx, out_buf);
}

FGLS_API uint32_t pogls_fgls_serialized_size(void)
{
    return (uint32_t)GCFS_TOTAL_BYTES;   /* 4896 */
}

/* ════════════════════════════════════════════════════════════
   STATS
   ════════════════════════════════════════════════════════════ */

FGLS_API uint32_t pogls_fgls_routed_count(void *ctx)
{
    if (!ctx) return 0;
    return ((TgwFglsCtx *)ctx)->routed_count;
}

FGLS_API uint32_t pogls_fgls_grounded_count(void *ctx)
{
    if (!ctx) return 0;
    return ((TgwFglsCtx *)ctx)->grounded_count;
}

FGLS_API void pogls_fgls_stats(void *ctx, PoglsFglsStats *out)
{
    if (!ctx || !out) return;
    memset(out, 0, sizeof(*out));
    TgwFglsStats s = tgw_fgls_stats((const TgwFglsCtx *)ctx);
    out->routed           = s.routed;
    out->grounded         = s.grounded;
    out->serialized       = s.serialized;
    out->dispatched       = (uint32_t)s.dispatch.total_dispatched;
    out->grounded_dispatch= (uint32_t)s.dispatch.total_grounded;
    out->quarantined      = (uint32_t)s.dispatch.total_quarantined;
    out->fgls_writes      = s.storage.writes;
    out->fgls_deletes     = s.storage.deletes;
    out->fgls_overflows   = s.storage.overflows;
    out->active_cosets    = s.storage.active_cosets;
}

/* ════════════════════════════════════════════════════════════
   STATUS PRINT (debug)
   ════════════════════════════════════════════════════════════ */

FGLS_API void pogls_fgls_print_status(void *ctx)
{
    if (!ctx) return;
    tgw_fgls_status((const TgwFglsCtx *)ctx);
}

/* ════════════════════════════════════════════════════════════
   VERSION
   ════════════════════════════════════════════════════════════ */

FGLS_API const char *pogls_fgls_version(void)
{
    return "pogls_fgls v1.0 — tgw_fgls_connector.h export";
}


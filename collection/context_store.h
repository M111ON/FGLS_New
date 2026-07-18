/*
 * context_store.h — Residual Space as Temporary Context Store
 * ═══════════════════════════════════════════════════════════════
 *
 * Concept:
 *   Data ที่กำลังจะ ingest ถ้า fit geometry → live processing
 *   Data ที่ไม่ fit → freeze เข้า Residual Space (ghost layer)
 *   ตอน context building → summon กลับมาด้วย bond_key
 *
 * Pattern:
 *   freeze_context()  →  data enters residual space, returns bond_key
 *   thaw_context()    →  retrieve data by bond_key for processing
 *   verify_context()  →  check bond_key integrity
 *   evict_context()   →  remove old entries (LRU)
 *
 * Usage:
 *   ContextStore cs;
 *   ctx_store_init(&cs, 4096);
 *
 *   // Freeze: data that doesn't fit geometry
 *   uint64_t bk = ctx_store_freeze(&cs, &piece, data, size, is_high_entropy);
 *
 *   // Thaw: retrieve for context building
 *   const void *ctx = ctx_store_thaw(&cs, bk, &ctx_size);
 *
 *   // Process with context
 *   process_with_context(ctx, ctx_size);
 *
 * All header-only, static inline. No float. O(1) average.
 */

#ifndef CONTEXT_STORE_H
#define CONTEXT_STORE_H

#include "residual_space.h"
#include "pogls_bond.h"

/* ── Entry flags for context store ──────────────────────── */
#define CS_FLAG_INGEST_READY  0x40   /* data ready for ingest  */
#define CS_FLAG_PROCESSED     0x80   /* data already processed  */

/* ── Context Store (wraps ResidualSpace) ────────────────── */
typedef struct {
    ResidualSpace  rs;          /* underlying residual space    */
    uint32_t       freezes;     /* total freeze operations      */
    uint32_t       thaws;       /* total thaw operations        */
    uint32_t       hits;        /* thaw found data              */
    uint32_t       misses;      /* thaw found nothing           */
} ContextStore;

/* ════════════════════════════════════════════════════════════
   INIT / FREE
   ════════════════════════════════════════════════════════════ */

static inline int ctx_store_init(ContextStore *cs, uint32_t capacity) {
    if (!cs) return -1;
    if (rs_init(&cs->rs, capacity) != 0) return -1;
    cs->freezes = 0;
    cs->thaws   = 0;
    cs->hits    = 0;
    cs->misses  = 0;
    return 0;
}

static inline void ctx_store_free(ContextStore *cs) {
    if (!cs) return;
    rs_free(&cs->rs);
    cs->freezes = cs->thaws = cs->hits = cs->misses = 0;
}

/* ════════════════════════════════════════════════════════════
   FREEZE: store data in context (ghost layer)
   ════════════════════════════════════════════════════════════ */

/*
 * ctx_store_freeze() — store data that doesn't fit geometry
 *
 * piece: PoglsPiece for bond_key derivation
 * data:  payload bytes
 * size:  payload size (> 0, <= RS_MAX_DATA_SIZE)
 * is_high_entropy: flag for high-entropy marker
 *
 * Returns bond_key on success, 0 on failure.
 */
static inline uint64_t ctx_store_freeze(ContextStore *cs,
                                         const PoglsPiece *piece,
                                         const void *data,
                                         uint32_t size,
                                         uint8_t is_high_entropy)
{
    if (!cs) return RS_BOND_KEY_RESERVED;

    uint64_t bk = rs_freeze(&cs->rs, piece, data, size, is_high_entropy);
    if (bk != RS_BOND_KEY_RESERVED) {
        cs->freezes++;

        /* Mark as ingest-ready */
        uint32_t mask = cs->rs.capacity - 1;
        uint32_t slot = _rs_hash(bk, mask);
        while (cs->rs.entries[slot]) {
            ResidualEntry *e = cs->rs.entries[slot];
            if ((e->flags & RS_ENTRY_VALID) && e->bond_key == bk) {
                e->flags |= CS_FLAG_INGEST_READY;
                break;
            }
            slot = (slot + 1) & mask;
        }
    }

    return bk;
}

/* ════════════════════════════════════════════════════════════
   THAW: retrieve data for context building
   ════════════════════════════════════════════════════════════ */

/*
 * ctx_store_thaw() — retrieve frozen data by bond_key
 *
 * bond_key: key from ctx_store_freeze()
 * out_size: (output) size of retrieved data
 *
 * Returns pointer to data, or NULL if not found.
 * Data is valid until evicted or overwritten.
 */
static inline const void *ctx_store_thaw(ContextStore *cs,
                                           uint64_t bond_key,
                                           uint32_t *out_size)
{
    if (!cs) return NULL;

    const void *data = rs_thaw(&cs->rs, bond_key, out_size);
    if (data) {
        cs->thaws++;
        cs->hits++;
    } else {
        cs->misses++;
    }
    return data;
}

/* ════════════════════════════════════════════════════════════
   VERIFY: check bond_key integrity
   ════════════════════════════════════════════════════════════ */

static inline uint8_t ctx_store_verify(const ContextStore *cs,
                                         const PoglsPiece *piece)
{
    return cs ? rs_verify(&cs->rs, piece) : 0;
}

/* ════════════════════════════════════════════════════════════
   EVICT: remove old entries
   ════════════════════════════════════════════════════════════ */

static inline int ctx_store_evict(ContextStore *cs) {
    return cs ? rs_evict_one(&cs->rs) : 0;
}

static inline uint32_t ctx_store_evict_all(ContextStore *cs) {
    return cs ? rs_evict_all(&cs->rs) : 0;
}

/* ════════════════════════════════════════════════════════════
   QUERY
   ════════════════════════════════════════════════════════════ */

static inline uint8_t ctx_store_contains(const ContextStore *cs,
                                           uint64_t bond_key)
{
    return cs ? rs_contains(&cs->rs, bond_key) : 0;
}

static inline uint32_t ctx_store_count(const ContextStore *cs) {
    return cs ? cs->rs.count : 0;
}

static inline uint64_t ctx_store_bytes(const ContextStore *cs) {
    return cs ? cs->rs.total_bytes : 0;
}

/* ════════════════════════════════════════════════════════════
   STATS
   ════════════════════════════════════════════════════════════ */

static inline void ctx_store_stats(const ContextStore *cs) {
    if (!cs) return;
    printf("ContextStore: %u entries, %llu bytes\n", cs->rs.count,
           (unsigned long long)cs->rs.total_bytes);
    printf("  freezes=%u thaws=%u hits=%u misses=%u evictions=%u\n",
           cs->freezes, cs->thaws, cs->hits, cs->misses, cs->rs.evictions);
}

#endif /* CONTEXT_STORE_H */

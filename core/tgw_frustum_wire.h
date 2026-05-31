/*
 * tgw_frustum_wire.h — Parallel Frustum Hook + Metatron Option B
 * ════════════════════════════════════════════════════════════════
 *
 * Two drop-ins, one include:
 *
 *  1. FRUSTUM_PARALLEL_HOOK(addr, val, fs, fibo_seed)
 *     Parallel path — every packet, every addr, no gating.
 *     trit_decompose() → frustum_write() → side-effect only.
 *     Zero interference with geo/cardioid/tri5 paths.
 *     merkle_root = (uint32_t)(addr ^ val) — caller can override.
 *
 *  2. metatron_target_face_b(addr) — Option B
 *     addr ∈ [0    ..3455] → tetra zone → face from tring walk
 *     addr ∈ [3456 ..6911] → octa zone  → cross/chiral via compound
 *     addr ≥ 6912          → wrap mod 6912, re-classify
 *     Returns face_id 0..11 for metatron_route_to().
 *
 * Usage in tgw_dispatch_v2_tri5.h — add after geo_net_encode():
 *
 *   FRUSTUM_PARALLEL_HOOK(addr, val, &d->fs_store, FIBO_SEED_DEFAULT);
 *
 * For Metatron full routing (replaces lazy 0xFF):
 *
 *   uint8_t  target_face = metatron_target_face_b(addr);
 *   uint32_t dest_enc;
 *   MetaRouteType mr = meta_route(tri_enc, target_face).type;
 *
 * Add to TGWDispatchV2 struct:
 *   FrustumStore fs_store;       // parallel frustum pipeline
 *   uint64_t     fs_fibo_seed;   // set at init, default FIBO_SEED_DEFAULT
 *
 * Sacred: 3456=GEO_FULL_N, 6912=JUNCTION — used as read-only boundaries.
 * No malloc. No float. No heap.
 * ════════════════════════════════════════════════════════════════
 */

#ifndef TGW_FRUSTUM_WIRE_H
#define TGW_FRUSTUM_WIRE_H

#include <stdint.h>
#include "frustum_gcfs.h"          /* → frustum_slot64.h → frustum_trit.h */
#include "geo_metatron_route.h"    /* meta_route, METATRON_CROSS            */
#include "geo_temporal_lut.h"      /* GEO_WALK, TRING_COMP                 */

/* ── address zone boundaries (read-only, never modified) ───── */
#define FRUSTUM_TETRA_CEILING  3456u   /* GEO_FULL_N = 2⁷×3³              */
#define FRUSTUM_JUNCTION       6912u   /* 2⁸×3³, tetra+octa ceiling        */

/* ════════════════════════════════════════════════════════════════
   PART 1 — FRUSTUM PARALLEL HOOK
   Every packet → trit_decompose → frustum_write.
   Orthogonal to all geo/cardioid/tri5 logic.
   ════════════════════════════════════════════════════════════════ */

/*
 * _frustum_merkle_of: simple merkle_root for a packet.
 * addr XOR val — deterministic, no hash table, O(1).
 * Caller may replace with stronger hash if needed.
 */
static inline uint32_t _frustum_merkle_of(uint64_t addr, uint64_t val)
{
    return (uint32_t)((addr ^ val) & 0xFFFFFFFFu);
}

/*
 * frustum_parallel_write: core of the parallel hook.
 * Silenced cosets are dropped silently — no error, no stall.
 */
static inline void frustum_parallel_write(FrustumStore *fs,
                                           uint64_t      addr,
                                           uint64_t      val,
                                           uint64_t      fibo_seed)
{
    TritAddr  ta   = trit_decompose(addr, (uint32_t)val, fibo_seed);
    uint32_t  merk = _frustum_merkle_of(addr, val);
    frustum_write(fs, &ta, merk);
}

/*
 * FRUSTUM_PARALLEL_HOOK — paste once at top of tgw_dispatch_v2() body,
 * immediately after geo_net_encode(). No return, no branch.
 */
#define FRUSTUM_PARALLEL_HOOK(addr, val, fs_ptr, fibo_seed)            \
    frustum_parallel_write((fs_ptr), (addr), (uint64_t)(val), (fibo_seed))

/* ════════════════════════════════════════════════════════════════
   PART 2 — METATRON OPTION B: target_face derivation
   addr ∈ tetra zone [0..3455]    → face from tring walk position
   addr ∈ octa zone  [3456..6911] → cross-ring face (METATRON_CROSS)
   addr ≥ 6912                    → wrap to junction, re-classify
   ════════════════════════════════════════════════════════════════ */

/*
 * _meta_face_from_walk: tring walk position → compound face_id (0..11)
 * walk_pos 0..719 → compound = walk_pos / 60 → face 0..11
 */
static inline uint8_t _meta_face_from_walk(uint32_t enc)
{
    return (uint8_t)TRING_COMP(enc);   /* comp field = face_id 0..11 */
}

/*
 * metatron_target_face_b — Option B face derivation
 *
 * Tetra zone (addr < 3456):
 *   walk_pos = addr % 720
 *   enc      = GEO_WALK[walk_pos]
 *   face     = TRING_COMP(enc)         ← geometry drives face
 *
 * Octa zone (3456 ≤ addr < 6912):
 *   octa_pos  = (addr - 3456) % 720    ← mirror the 720-slot cycle
 *   enc       = GEO_WALK[octa_pos]
 *   src_face  = TRING_COMP(enc)
 *   face      = METATRON_CROSS[src_face]  ← cross-ring: octa bridges ring
 *
 * Above junction (addr ≥ 6912):
 *   wrap to junction, re-classify
 */
static inline uint8_t metatron_target_face_b(uint64_t addr)
{
    if (addr < FRUSTUM_TETRA_CEILING) {
        /* tetra zone */
        uint32_t enc = GEO_WALK[addr % TEMPORAL_WALK_LEN];
        return _meta_face_from_walk(enc);
    }

    if (addr < FRUSTUM_JUNCTION) {
        /* octa zone — cross-ring face */
        uint32_t octa_pos = (uint32_t)((addr - FRUSTUM_TETRA_CEILING)
                             % TEMPORAL_WALK_LEN);
        uint32_t enc      = GEO_WALK[octa_pos];
        uint8_t  src_face = _meta_face_from_walk(enc);
        return METATRON_CROSS[src_face];
    }

    /* above junction: wrap mod JUNCTION, re-classify */
    return metatron_target_face_b(addr % FRUSTUM_JUNCTION);
}

/* ════════════════════════════════════════════════════════════════
   COMBINED HOOK — full Metatron + Frustum per packet
   Replaces tgw_tri5_decide lazy 0xFF target_face with Option B.
   Use this when both Metatron routing and frustum parallel are active.
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t       target_face;  /* metatron_target_face_b result   */
    MetaRouteType route;        /* route type to target_face        */
    uint16_t      dest_pos;     /* walk position of destination     */
    uint32_t      dest_enc;     /* enc at destination               */
} MetatronB;

static inline MetatronB metatron_b_decide(uint64_t addr)
{
    MetatronB mb;
    uint32_t  src_enc   = GEO_WALK[addr % TEMPORAL_WALK_LEN];
    mb.target_face      = metatron_target_face_b(addr);
    MetaDecision md     = meta_route((uint16_t)(src_enc % TRING_WALK_CYCLE), mb.target_face);
    mb.route            = md.type;
    mb.dest_enc         = md.next_enc;
    mb.dest_pos         = (mb.dest_enc < 0x800u)
                          ? GEO_WALK_IDX[mb.dest_enc & 0x7FFu]
                          : 0u;
    return mb;
}

/*
 * TGW_FULL_HOOK — paste at top of tgw_dispatch_v2() after geo_net_encode().
 * Runs frustum parallel write + Metatron Option B derivation together.
 * mb_var must be declared MetatronB before calling.
 */
#define TGW_FULL_HOOK(addr, val, fs_ptr, fibo_seed, mb_var)            \
    do {                                                                 \
        FRUSTUM_PARALLEL_HOOK((addr), (val), (fs_ptr), (fibo_seed));   \
        (mb_var) = metatron_b_decide((uint64_t)(addr));                 \
    } while(0)

#endif /* TGW_FRUSTUM_WIRE_H */

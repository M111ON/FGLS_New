/*
 * geo_gp_frustum_bridge.h — GpSphere ↔ FrustumBlock Bridge
 * ══════════════════════════════════════════════════════════
 *
 * Closes the loop:
 *   chunk_idx → GpAddr → FrustumBlock slot → Diamond data[64B]
 *
 * Mapping logic (all O(1), stateless, no malloc):
 *
 *   GpAddr.tile_id  → pentagon anchor (0..11) = drain index
 *   GpAddr.tile_id  → diamond_slot (0..53)   = data[slot*64]
 *   GpAddr.dim      → clock_tick  (0..143)   = meta.letter_map[tick]
 *
 *   diamond_slot = tile_id % DGLS_DIAMOND_COUNT   (54 slots)
 *   drain_idx    = tile_id % DGLS_DRAIN_COUNT      (12 drains = pentagons)
 *   clock_tick   = (tile_id * 144 / face_max) & 0x7F  scaled to 0..143
 *
 * Pentagon rule preserved:
 *   tile_id 0..11 → drain_idx == tile_id (direct anchor, no mod ambiguity)
 *   tile_id 12+   → drain_idx = gp_tile_to_pent() → drain_state[drain_idx]
 *
 * Zone reset rule:
 *   gp_is_zone_boundary(tile_id) → skel_enc_zone_reset()
 *                                 + drain_state[drain_idx] flush check
 *
 * Metatron route entry:
 *   enc = gp_addr_to_tick(addr) % 720  → meta_face(enc), meta_slot(enc)
 *   → ORBITAL / CHIRAL / CROSS / HUB dispatch
 *
 * Include order:
 *   frustum_layout_v2.h
 *   geo_goldberg_sphere.h
 *   geo_metatron_route.h   (for meta_face / meta_slot)
 *
 * No float. No heap. No global state.
 * ══════════════════════════════════════════════════════════
 */

#pragma once
#include <stdint.h>
#include <string.h>
#include "frustum_layout_v2.h"
#include "geo_goldberg_sphere.h"

/* ── Slot mapping result ───────────────────────────────────── */
typedef struct {
    uint8_t  diamond_slot;   /* 0..53  → data[slot*64 .. slot*64+63] */
    uint8_t  drain_idx;      /* 0..11  → meta.drain_state[drain_idx] */
    uint8_t  clock_tick;     /* 0..143 → meta.letter_map[tick]       */
    uint8_t  meta_enc_face;  /* 0..11  → Metatron face               */
    uint8_t  meta_enc_slot;  /* 0..59  → Metatron slot within face   */
} GpFrustumSlot;

/*
 * gp_frustum_map — GpAddr → GpFrustumSlot
 * All divisions use constants → compiler reduces to shifts/mods.
 */
static inline GpFrustumSlot gp_frustum_map(uint8_t gp_level, GpAddr a)
{
    GpFrustumSlot s;

    /* diamond slot: tile_id wraps over 54 DiamondBlocks */
    s.diamond_slot = (uint8_t)(a.tile_id % FGLS_DIAMOND_COUNT);   /* %54 */

    /* drain: pentagon anchor of this tile */
    s.drain_idx = gp_is_pentagon(a.tile_id)
                ? (uint8_t)a.tile_id
                : gp_tile_to_pent(gp_level, a.tile_id);

    /* clock tick: scale tile_id into 0..143 range */
    uint32_t face_max = gp_face_count(gp_level);
    s.clock_tick = (uint8_t)(((uint64_t)a.tile_id * FGLS_CLOCK_TICKS)
                              / face_max);                          /* 0..143 */

    /* Metatron enc: tick % 720 → face/slot decomposition */
    uint16_t enc = (uint16_t)(gp_addr_to_tick(a) % 720u);
    s.meta_enc_face = (uint8_t)(enc / 60u);   /* META_FACE_SZ=60 */
    s.meta_enc_slot = (uint8_t)(enc % 60u);

    return s;
}

/* ── Diamond data pointer — zero copy ─────────────────────── */
static inline uint8_t *gp_diamond_ptr(FrustumBlock *blk,
                                       GpFrustumSlot s)
{
    return blk->data + (size_t)s.diamond_slot * FGLS_DIAMOND_BYTES;
}

static inline const uint8_t *gp_diamond_ptr_c(const FrustumBlock *blk,
                                                GpFrustumSlot s)
{
    return blk->data + (size_t)s.diamond_slot * FGLS_DIAMOND_BYTES;
}

/* ── Lens read/write through FrustumBlock ──────────────────── */

/*
 * gp_blk_write — write 64B chunk into FrustumBlock via GpAddr
 * Marks shadow_state[drain] as occupied.
 * Returns diamond_slot written (0..53).
 */
static inline uint8_t gp_blk_write(FrustumBlock *blk,
                                    uint8_t       gp_level,
                                    GpAddr        a,
                                    const uint8_t chunk[64])
{
    GpFrustumSlot s = gp_frustum_map(gp_level, a);
    memcpy(gp_diamond_ptr(blk, s), chunk, FGLS_DIAMOND_BYTES);

    /* mark shadow occupied (bit0) for drain tracking */
    uint8_t drain = s.drain_idx;
    blk->meta.drain_state[drain] |= 0x01u;   /* active drain open */

    /* mark shadow zone occupied — use dim as shadow index (0..7 → 0..27) */
    uint8_t shadow_idx = (uint8_t)((s.drain_idx * 2u + a.dim) & 0x1Fu); /* %28 */
    blk->meta.shadow_state[shadow_idx] |= 0x01u;  /* occupied */

    /* update letter_map slot */
    blk->meta.letter_map[s.clock_tick] = s.diamond_slot;

    return s.diamond_slot;
}

/*
 * gp_blk_read — read 64B chunk from FrustumBlock via GpAddr
 * Returns pointer to data (no copy). NULL if drain is not active.
 */
static inline const uint8_t *gp_blk_read(const FrustumBlock *blk,
                                           uint8_t             gp_level,
                                           GpAddr              a)
{
    GpFrustumSlot s = gp_frustum_map(gp_level, a);
    /* check drain active */
    if (!(blk->meta.drain_state[s.drain_idx] & 0x01u)) return NULL;
    return gp_diamond_ptr_c(blk, s);
}

/* ── Zone boundary → drain flush check ────────────────────── */
/*
 * gp_blk_zone_check — call when tile_id crosses pentagon boundary.
 * Returns 1 if drain needs flush (flush_pending bit set),
 * 0 if clean. Caller triggers skel_enc_zone_reset() + drain flush.
 */
static inline int gp_blk_zone_check(const FrustumBlock *blk,
                                     uint8_t drain_idx)
{
    return (blk->meta.drain_state[drain_idx] & 0x02u) ? 1 : 0;
}

/* ── Reshape threshold check (54 = POGLS_SACRED_NEXUS) ──────── */
/*
 * gp_blk_reshape_ready — count occupied shadow slots.
 * Returns 1 if >= 54 slots occupied → atomic reshape should fire.
 * O(28) = O(1).
 */
static inline int gp_blk_reshape_ready(const FrustumBlock *blk)
{
    int count = 0;
    for (int i = 0; i < FGLS_SHADOW_COUNT; i++)
        count += (blk->meta.shadow_state[i] & 0x01u) ? 1 : 0;
    /* also count active drain slots */
    for (int i = 0; i < FGLS_DRAIN_COUNT; i++)
        count += (blk->meta.drain_state[i] & 0x01u) ? 1 : 0;
    return (count >= FGLS_DIAMOND_COUNT) ? 1 : 0;  /* 54 threshold */
}

/* ── Complete loop: chunk_idx → write → zone → reshape ─────── */
/*
 * gp_pipeline_write — full pipeline entry point.
 *
 * chunk_idx  : from encode pipeline (ci * CHUNK_SZ offset)
 * gp_level   : fixed per file
 * blk        : active FrustumBlock
 * chunk      : 64B data
 * out_zone_reset : set to 1 if caller should call skel_enc_zone_reset()
 * out_reshape    : set to 1 if caller should trigger atomic reshape
 *
 * Returns diamond_slot written.
 */
static inline uint8_t gp_pipeline_write(uint64_t       chunk_idx,
                                          uint8_t        gp_level,
                                          FrustumBlock  *blk,
                                          const uint8_t  chunk[64],
                                          int           *out_zone_reset,
                                          int           *out_reshape)
{
    GpAddr a = gp_chunk_to_addr(gp_level, chunk_idx);

    *out_zone_reset = gp_is_zone_boundary(a.tile_id);
    uint8_t slot    = gp_blk_write(blk, gp_level, a, chunk);
    *out_reshape    = gp_blk_reshape_ready(blk);

    return slot;
}

/*
 * tw_rewind_bridge.h — SID TW Face Rewind ↔ geo_rewind.h Bridge
 * ═══════════════════════════════════════════════════════════════
 *
 * Connects the SID coordinate system (1440-slot TWFaceRewind with hex+tri
 * centroids) to the legacy temporal rewind buffer (972-slot RewindBuffer
 * with hex-only GEO_WALK enc).
 *
 *   SID world          → TWFaceRewind (1440 slots, 8-byte keys, hex+tri)
 *   TStream world      → RewindBuffer (972 slots, 4104B chunks, hex-only enc)
 *
 * Hex captures (is_tri=0): tring_pos (0..719) = GEO_WALK walk position.
 *   → enc = GEO_WALK[tring_pos] for geo_rewind.h.
 *
 * Tri captures (is_tri=1): tring_pos (60..779) sits between hex centroids.
 *   → Not stored in geo_rewind.h (hex-only buffer). TWFaceRewind only.
 *
 * Unified lookup flow:
 *   1. Look up TWFaceRewind by tring_pos (O(1), hex+tri, 1440 slots)
 *   2. If hex capture, also check RewindBuffer by enc (O(1), 972 slots)
 *   3. Return combined result
 *
 * Include this AFTER both tw_face_bridge.h and core/core/geo_rewind.h.
 *
 * No malloc. No float. O(1).
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef TW_REWIND_BRIDGE_H
#define TW_REWIND_BRIDGE_H

#include <stdint.h>
#include <string.h>
#include "tw_face_bridge.h"
#include "core/core/geo_rewind.h"

/* ══════════════════════════════════════════════════════════════
   SID tring_pos ↔ packed enc conversion
   ══════════════════════════════════════════════════════════════ */

/*
 * Convert SID tring_pos (0..1439) to packed GEO_WALK enc value.
 *
 * For hex captures (is_tri=0):  walk_pos = tring_pos (0..719)
 * For tri captures (is_tri=1):  walk_pos = (tring_pos - 60) % 720
 *                                (maps to hex counterpart position)
 *
 * Returns packed enc for use with rewind_store/rewind_find in geo_rewind.h.
 * Returns 0xFFFFFFFF if the position is out of range or unmappable.
 */
static inline uint32_t tw_sid_tring_to_enc(uint16_t sid_tring, uint8_t is_tri) {
    if (sid_tring >= TW_TRING_1440) return 0xFFFFFFFFu;

    uint16_t walk_pos;
    if (is_tri) {
        if (sid_tring < 60) return 0xFFFFFFFFu;
        walk_pos = (sid_tring - 60u) % 720u;
    } else {
        walk_pos = sid_tring % 720u;
    }

    return GEO_WALK[walk_pos];
}

/*
 * Convert packed GEO_WALK enc to SID tring_pos.
 *
 * Uses tring_pos() from geo_temporal_ring.h to get the walk position (0..719).
 * Returns the hex-only tring_pos (tri position is not recoverable from enc alone).
 */
static inline uint16_t tw_enc_to_sid_tring(uint32_t enc) {
    return tring_pos(enc);
}

/*
 * Check whether a given enc has a valid mapping (walk position within 0..719).
 * Returns 1 if valid, 0 if the enc doesn't correspond to any walk position.
 */
static inline int tw_enc_is_valid(uint32_t enc) {
    return (enc & 0x7FFu) < 2048u && GEO_WALK_IDX[enc & 0x7FFu] != 0xFFFFu;
}

/* ══════════════════════════════════════════════════════════════
   TWFaceCapture ↔ TStreamChunk packing
   ══════════════════════════════════════════════════════════════ */

/*
 * Pack a TWFaceCapture into a TStreamChunk for geo_rewind.h storage.
 * The chunk carries the 8-byte packed key as metadata.
 * size = 8, remaining 4088 bytes are zeroed.
 */
static inline TStreamChunk tw_cap_pack_chunk(const TWFaceCapture *cap) {
    TStreamChunk ch;
    memset(&ch, 0, sizeof(ch));
    uint64_t key = tw_face_pack_key(cap);
    memcpy(ch.data, &key, 8);
    ch.size = 8;
    return ch;
}

/*
 * Unpack a TStreamChunk back to TWFaceCapture + packed key.
 * Returns 1 on success, 0 if chunk data is too small or all-zero.
 */
static inline int tw_chunk_unpack_cap(const TStreamChunk *chunk, TWFaceCapture *cap, uint64_t *key_out) {
    if (!chunk || chunk->size < 8) return 0;

    uint64_t key;
    memcpy(&key, chunk->data, 8);
    if (key == 0) return 0;

    if (cap) tw_face_unpack_key(key, cap);
    if (key_out) *key_out = key;
    return 1;
}

/* ══════════════════════════════════════════════════════════════
   Unified store/find
   ══════════════════════════════════════════════════════════════ */

/*
 * Unified rewind buffer lookup result.
 *
 * Fields:
 *   tw_key    — packed key from TWFaceRewind (0 = not found)
 *   cap       — TWFaceCapture unpacked from tw_key
 *   has_geo   — geo_rewind.h also had this tensor
 *   geo_chunk — TStreamChunk from geo_rewind.h (valid only if has_geo=1)
 */
typedef struct {
    uint64_t        tw_key;
    TWFaceCapture   cap;
    int             has_geo;
    TStreamChunk    geo_chunk;
} TWBRewindResult;

/*
 * Store a TWFaceCapture in both TWFaceRewind and RewindBuffer.
 *
 * Hex captures (is_tri=0): stored in both systems.
 * Tri captures (is_tri=1): stored in TWFaceRewind only.
 *
 * Returns the enc used for geo_rewind.h (0xFFFFFFFF if skipped).
 */
static inline uint32_t tw_bridge_rewind_store(TWFaceRewind *tw_rb,
                                               RewindBuffer *geo_rb,
                                               const TWFaceCapture *cap)
{
    if (!tw_rb || !cap) return 0xFFFFFFFFu;

    tw_face_rewind_store(tw_rb, cap);

    if (!cap->is_tri && geo_rb) {
        uint32_t enc = tw_sid_tring_to_enc(cap->tring_pos, 0);
        if (enc != 0xFFFFFFFFu) {
            TStreamChunk ch = tw_cap_pack_chunk(cap);
            rewind_store(geo_rb, enc, &ch);
            return enc;
        }
    }

    return 0xFFFFFFFFu;
}

/*
 * Unified lookup by SID tring_pos.
 *
 * First checks TWFaceRewind (fast O(1), supports hex+tri, 1440 slots).
 * Then checks RewindBuffer by derived enc (hex-only, 972 slots).
 *
 * Returns a TWBRewindResult struct with combined results.
 */
static inline TWBRewindResult tw_bridge_rewind_find(TWFaceRewind *tw_rb,
                                                     RewindBuffer *geo_rb,
                                                     uint16_t sid_tring)
{
    TWBRewindResult r;
    memset(&r, 0, sizeof(r));

    r.tw_key = tw_rewind_find(tw_rb, sid_tring);
    if (r.tw_key) {
        tw_face_unpack_key(r.tw_key, &r.cap);

        if (geo_rb && !r.cap.is_tri) {
            uint32_t enc = tw_sid_tring_to_enc(sid_tring, 0);
            if (enc != 0xFFFFFFFFu) {
                const TStreamChunk *p = rewind_find(geo_rb, enc);
                if (p) {
                    r.geo_chunk = *p;
                    r.has_geo = 1;
                }
            }
        }
    }

    return r;
}

/*
 * Unified lookup by TWFaceCapture (extracts tring_pos internally).
 * Convenience wrapper around tw_bridge_rewind_find.
 */
static inline TWBRewindResult tw_bridge_rewind_find_cap(TWFaceRewind *tw_rb,
                                                         RewindBuffer *geo_rb,
                                                         const TWFaceCapture *cap)
{
    return tw_bridge_rewind_find(tw_rb, geo_rb, cap ? cap->tring_pos : 0xFFFFu);
}

/*
 * Check if a SID position is present in either store.
 * Returns 1 if found in TWFaceRewind OR RewindBuffer, 0 if neither.
 */
static inline int tw_bridge_rewind_has(TWFaceRewind *tw_rb,
                                        RewindBuffer *geo_rb,
                                        uint16_t sid_tring,
                                        uint8_t is_tri)
{
    if (tw_rewind_has(tw_rb, sid_tring)) return 1;

    if (!is_tri && geo_rb) {
        uint32_t enc = tw_sid_tring_to_enc(sid_tring, 0);
        if (enc != 0xFFFFFFFFu)
            return rewind_has(geo_rb, enc);
    }

    return 0;
}

/*
 * Evict a SID position from the RewindBuffer.
 * TWFaceRewind is write-once (key is overwritten on next store at same pos).
 * Returns the enc that was evicted (0xFFFFFFFF if not found/skipped).
 */
static inline uint32_t tw_bridge_rewind_evict_geo(TWFaceRewind *tw_rb,
                                                    RewindBuffer *geo_rb,
                                                    uint16_t sid_tring,
                                                    uint8_t is_tri)
{
    (void)tw_rb;
    if (is_tri || !geo_rb) return 0xFFFFFFFFu;

    uint32_t enc = tw_sid_tring_to_enc(sid_tring, 0);
    if (enc == 0xFFFFFFFFu) return 0xFFFFFFFFu;

    uint16_t slot = tring_pos(enc) % REWIND_SLOTS;
    if (geo_rb->slots[slot].valid && geo_rb->slots[slot].enc == enc) {
        geo_rb->slots[slot].valid = false;
        geo_rb->slots[slot].enc = 0;
    }

    return enc;
}

/* ══════════════════════════════════════════════════════════════
   Stats
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t tw_occupied;    /* valid slots in TWFaceRewind */
    uint32_t tw_stored;      /* total stores in TWFaceRewind */
    uint32_t geo_occupied;   /* valid slots in RewindBuffer */
    uint32_t geo_stored;     /* total chunks in RewindBuffer */
    uint32_t geo_pinned;     /* pinned slots in RewindBuffer */
} TWBridgeStats;

/*
 * Collect stats from both buffers.
 */
static inline void tw_bridge_stats(TWFaceRewind *tw_rb,
                                    RewindBuffer *geo_rb,
                                    TWBridgeStats *st)
{
    memset(st, 0, sizeof(*st));

    if (tw_rb) {
        st->tw_stored   = tw_rb->stored;
        st->tw_occupied = tw_rewind_occupied(tw_rb);
    }

    if (geo_rb) {
        RewindStats rs;
        rewind_stats(geo_rb, &rs);
        st->geo_occupied = rs.occupied;
        st->geo_stored   = rs.stored;
        st->geo_pinned   = rs.pinned;
    }
}

#endif /* TW_REWIND_BRIDGE_H */

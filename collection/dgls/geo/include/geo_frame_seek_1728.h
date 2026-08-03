/*
 * geo_frame_seek_1728.h — Frame Seek on 1728-cycle (288-cell extended)
 * ══════════════════════════════════════════════════════════════════
 *
 * Extension of geo_frame_seek.h for 288-cell architecture.
 * 1728 = 288 × 6 = 12³ (vs 1440 = 288 × 5)
 *
 * Functions (frame_1728_enc/next/prev/verify) live in rdh_288_bridge.h
 * This header provides the Frame1728 struct and frame_1728_at()
 *
 * No malloc. No float. Stateless O(1).
 * ══════════════════════════════════════════════════════════════════
 */

#ifndef GEO_FRAME_SEEK_1728_H
#define GEO_FRAME_SEEK_1728_H

#include <stdint.h>

/* ══════════════════════════════════════════════════════════════
   CONSTANTS (mirrored from rdh_288_bridge.h for independence)
   ══════════════════════════════════════════════════════════════ */

#define CYCLE_1728       1728u
#define FACE_SLOTS_1728   144u

/* ══════════════════════════════════════════════════════════════
   FRAME STRUCT — 1728-cycle decomposition
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t enc;           /* source enc 0..1727                */
    uint8_t  face;          /* 0..11  dodecahedron face           */
    uint8_t  direction;     /* 0..5   6-fold symmetry direction   */
    uint16_t cell_pos;      /* 0..287  position within 288-cell   */
    uint16_t ico_idx;       /* 0..161 icosphere address           */
    uint8_t  phase;         /* iteration phase (enc/12 % 12)      */
    uint8_t  slot_in_face;  /* 0..143 slot within face (12×12)    */
} Frame1728;

/* ══════════════════════════════════════════════════════════════
   CORE: frame_1728_at(enc) — O(1) seek
   ══════════════════════════════════════════════════════════════ */

static inline Frame1728 frame_1728_at(uint16_t enc)
{
    Frame1728 f;
    f.enc          = enc;
    f.face         = (uint8_t)(enc / FACE_SLOTS_1728);           /* 0..11  */
    f.slot_in_face = (uint8_t)(enc % FACE_SLOTS_1728);           /* 0..143 */
    f.direction    = (uint8_t)((enc / 288) % 6);                 /* 0..5   */
    f.cell_pos     = (uint16_t)(enc % 288);                      /* 0..287 */
    f.ico_idx      = (uint16_t)(enc % 162);                      /* 0..161 */
    f.phase        = (uint8_t)((enc / 12) % 12);                 /* 0..11  */
    return f;
}

/* ══════════════════════════════════════════════════════════════
   BRIDGE: flat_key → Frame1728
   ══════════════════════════════════════════════════════════════ */

static inline Frame1728 frame_1728_from_key(int64_t flat_key)
{
    uint16_t enc_1728 = (uint16_t)(flat_key % CYCLE_1728);
    return frame_1728_at(enc_1728);
}

static inline int64_t frame_1728_to_key(Frame1728 f)
{
    return (int64_t)f.face * CYCLE_1728 + (int64_t)f.enc;
}

#endif /* GEO_FRAME_SEEK_1728_H */

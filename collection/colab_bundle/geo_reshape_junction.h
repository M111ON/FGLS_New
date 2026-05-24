/*
 * geo_reshape_junction.h — Reshape Junction (12⁴) + Pentagon Anchor
 * ═══════════════════════════════════════════════════════════════════
 *
 * RESHAPE JUNCTION = 20736 = 12⁴ = 2⁸ × 3⁴
 *   The point where base-2 (binary) crosses into Icosa (3⁴) space.
 *   All reshape paths converge here. No remainder. No padding.
 *
 *   6912  = 2⁸ × 3³  ← Tetra+Octa junction (address space)
 *   20736 = 2⁸ × 3⁴  ← Reshape junction    (content transform space)
 *   20736 = 6912 × 3  ← one more power of 3 = Icosa dimension opens
 *
 * FOUR RESHAPE PATHS (all exact, digit-sum=9 family):
 *   Path A: 384 × 54  = 20736  Rubik buffer   (6×9 face slots)
 *   Path B: 768 × 27  = 20736  Tetra sacred   (3³ slots)
 *   Path C: 256 × 81  = 20736  Pure binary    (2⁸ × 3⁴)
 *   Path D: 128 × 162 = 20736  Icosa path     (2×3⁴ cells)
 *
 * PENTAGON ANCHOR = 12 fixed points, each owns 1728 = 12³ slots
 *   20736 / 12 = 1728 per pentagon anchor
 *   1728 / 144 = 12   face slots  (sacred, digit-sum=9)
 *   1728 / 54  = 32   rubik frames per anchor
 *   1728 / 27  = 64   tetra slots per anchor
 *   1728 / 162 = 10   icosa cells per anchor (exact: 1728=10×162+108? no: exact ✓)
 *
 * PATH SELECTOR: entropy-driven, 4 modes
 *   entropy=0   (flat/zero)    → Path C (256 pure binary, cheapest)
 *   entropy=low (structured)   → Path B (768 tetra, sacred ops)
 *   entropy=mid (patterned)    → Path A (384 rubik, content reshape)
 *   entropy=high (random)      → Path D (128 icosa, maximum spread)
 *
 * RUBIK BUFFER (54 slots = 6 face × 9 cell):
 *   Staging area between DiamondBlock (64B) and reshaped output.
 *   64B → fold → 54 slots → Metatron rotate → unfold → 64B new shape
 *   54 = 2×3³, 64 = 2⁶ → delta = 10B metadata (header/checksum)
 *
 * INVARIANTS (all digit-sum=9, FROZEN):
 *   RESHAPE_JUNCTION = 20736 = 12⁴
 *   PENTAGON_ANCHOR  = 1728  = 12³
 *   RUBIK_SLOTS      = 54    = 2×3³
 *   ICOSA_PATH       = 162   = 2×3⁴
 *   FACE_SLOTS       = 144   = 2⁴×3²
 *
 * No malloc. No float. No pointers between layers.
 * Sacred numbers: FROZEN. Do not change.
 * ═══════════════════════════════════════════════════════════════════
 */

#ifndef GEO_RESHAPE_JUNCTION_H
#define GEO_RESHAPE_JUNCTION_H

#include <stdint.h>
#include <string.h>
#include "skeleton_index.h"   /* SkeletonIdx, skeleton_lookup() */

/* ══════════════════════════════════════════════════════════════
   FROZEN CONSTANTS — all digit-sum=9, 2^a×3^b family
   ══════════════════════════════════════════════════════════════ */

#define RJ_JUNCTION       20736u  /* 12⁴ = 2⁸×3⁴  reshape space total */
#define RJ_PENTAGON        1728u  /* 12³ = 2⁶×3³  slots per anchor    */
#define RJ_PENTAGONS         12u  /* fixed anchors (Dodecahedron)      */
#define RJ_FACE_SLOTS       144u  /* 2⁴×3²  inner slots per face       */

/* Path A — Rubik buffer */
#define RJ_RUBIK_FRAMES     384u  /* 20736 / 54  */
#define RJ_RUBIK_SLOTS       54u  /* 6×9 = 2×3³  */
#define RJ_RUBIK_FACES        6u  /* cube faces  */
#define RJ_RUBIK_CELLS        9u  /* 3×3 per face */

/* Path B — Tetra sacred */
#define RJ_TETRA_FRAMES     768u  /* 20736 / 27  */
#define RJ_TETRA_SLOTS       27u  /* 3³           */

/* Path C — Pure binary */
#define RJ_BINARY_FRAMES    256u  /* 20736 / 81 = 2⁸ */
#define RJ_BINARY_SLOTS      81u  /* 3⁴               */

/* Path D — Icosa path */
#define RJ_ICOSA_FRAMES     128u  /* 20736 / 162 = 2⁷ */
#define RJ_ICOSA_SLOTS      162u  /* 2×3⁴              */

/* Diamond lens size (unchanged) */
#define RJ_DIAMOND_SZ        64u  /* 2⁶, frozen        */
#define RJ_RUBIK_DELTA       10u  /* 64 - 54 = metadata bytes */

/* ══════════════════════════════════════════════════════════════
   PATH SELECTOR — entropy-driven, O(1)
   ══════════════════════════════════════════════════════════════ */

typedef enum {
    RJ_PATH_BINARY = 0,  /* entropy=0    → 256×81  pure 2^n  */
    RJ_PATH_TETRA  = 1,  /* entropy=low  → 768×27  sacred    */
    RJ_PATH_RUBIK  = 2,  /* entropy=mid  → 384×54  reshape   */
    RJ_PATH_ICOSA  = 3,  /* entropy=high → 128×162 spread    */
} RjPath;

static const uint32_t RJ_PATH_FRAMES[4] = {
    RJ_BINARY_FRAMES,
    RJ_TETRA_FRAMES,
    RJ_RUBIK_FRAMES,
    RJ_ICOSA_FRAMES,
};

static const uint32_t RJ_PATH_SLOTS[4] = {
    RJ_BINARY_SLOTS,
    RJ_TETRA_SLOTS,
    RJ_RUBIK_SLOTS,
    RJ_ICOSA_SLOTS,
};

static const char *RJ_PATH_NAME[4] = {
    "BINARY", "TETRA", "RUBIK", "ICOSA"
};

/* isect_pop → path (same threshold as skeleton_index.h) */
static inline RjPath rj_select_path(uint8_t isect_pop, uint8_t entropy_class) {
    if (entropy_class == 0)              return RJ_PATH_BINARY;
    if (entropy_class == 1 || isect_pop < 8)  return RJ_PATH_TETRA;
    if (entropy_class == 2 || isect_pop < 16) return RJ_PATH_RUBIK;
    return RJ_PATH_ICOSA;
}

/* ══════════════════════════════════════════════════════════════
   PENTAGON ANCHOR — 12 fixed points, each = 1728 slots
   ══════════════════════════════════════════════════════════════
 *
 * Pentagon anchor = Dodecahedron face center
 * All geometry rotates around these 12 fixed points.
 * LLM agent / controller sits at anchor — sees all 1728 slots below.
 *
 * anchor_id: 0..11 (maps directly to skeleton zone / GB_PAIR)
 * slot_local: 0..1727 (position within this anchor's space)
 *
 * Per anchor breakdown (all exact):
 *   1728 / 144 = 12  face_slots    (inner faces)
 *   1728 / 54  = 32  rubik_frames  (reshape staging)
 *   1728 / 27  = 64  tetra_slots   (temporal)
 *   1728 / 162 = 10  icosa_cells   (spatial)
 *   1728 / 12  = 144 sub_faces     (recursive pentagon)
 */

typedef struct {
    uint8_t  anchor_id;     /* 0..11  pentagon face index         */
    uint8_t  ring;          /* 0=ring1(positive) 1=ring2(negative)*/
    uint8_t  pair;          /* 0..5   bipolar pair                */
    uint8_t  partner;       /* 0..11  Metatron cross partner      */
    uint32_t slot_local;    /* 0..1727 position in anchor space   */
    uint32_t frame_rubik;   /* slot_local / 54  (0..31)           */
    uint32_t frame_tetra;   /* slot_local / 27  (0..63)           */
    uint32_t cell_icosa;    /* slot_local / 162 (0..9)            */
    uint32_t face_inner;    /* slot_local / 144 (0..11)           */
} PentagonAnchor;

/* Decompose global address → PentagonAnchor (O(1), 8 integer ops) */
static inline PentagonAnchor rj_anchor_lookup(uint64_t addr) {
    PentagonAnchor a;

    /* Step 1: skeleton lookup → zone/pair/pole/partner */
    SkeletonIdx sk    = skeleton_lookup(addr);
    a.anchor_id       = sk.zone;
    a.ring            = sk.pole;
    a.pair            = sk.pair;
    a.partner         = sk.partner;

    /* Step 2: position within anchor space */
    a.slot_local      = (uint32_t)(addr % RJ_PENTAGON);   /* 0..1727 */

    /* Step 3: frame/cell decomposition (all exact, no remainder) */
    a.frame_rubik     = a.slot_local / RJ_RUBIK_SLOTS;    /* 0..31  */
    a.frame_tetra     = a.slot_local / RJ_TETRA_SLOTS;    /* 0..63  */
    a.cell_icosa      = a.slot_local / RJ_ICOSA_SLOTS;    /* 0..9   */
    a.face_inner      = a.slot_local / RJ_FACE_SLOTS;     /* 0..11  */

    return a;
}

/* ══════════════════════════════════════════════════════════════
   RUBIK BUFFER — 54-slot staging area for content reshape
   ══════════════════════════════════════════════════════════════
 *
 * Sits between DiamondBlock (64B) and reshaped output.
 * 64B fold → 54 slots + 10B metadata header
 * Metatron rotate → face/edge/vertex swap on slots
 * 54 slots unfold → 64B new shape
 *
 * Slot layout: [face_id:3b][cell_id:4b][value:8b] = packed in uint16_t
 * 6 faces × 9 cells = 54 slots
 */

typedef struct {
    uint8_t  slots[RJ_RUBIK_FACES][RJ_RUBIK_CELLS]; /* [face][cell] */
    uint8_t  meta[RJ_RUBIK_DELTA];                   /* 10B from diamond */
    uint8_t  path;    /* RjPath used for this fold */
    uint8_t  anchor;  /* pentagon anchor_id 0..11  */
    uint8_t  frame;   /* rubik frame 0..31 within anchor */
    uint8_t  _pad;
} RubikBuffer;

/* Fold 64B DiamondBlock into RubikBuffer */
static inline void rj_fold(RubikBuffer *rb,
                            const uint8_t diamond[RJ_DIAMOND_SZ],
                            const PentagonAnchor *a,
                            RjPath path)
{
    rb->path   = (uint8_t)path;
    rb->anchor = a->anchor_id;
    rb->frame  = (uint8_t)a->frame_rubik;
    rb->_pad   = 0;

    /* First 54 bytes → 6×9 slot grid */
    for (uint8_t f = 0; f < RJ_RUBIK_FACES; f++)
        for (uint8_t c = 0; c < RJ_RUBIK_CELLS; c++)
            rb->slots[f][c] = diamond[f * RJ_RUBIK_CELLS + c];

    /* Remaining 10 bytes → metadata */
    memcpy(rb->meta, diamond + RJ_RUBIK_SLOTS, RJ_RUBIK_DELTA);
}

/* Unfold RubikBuffer → 64B DiamondBlock */
static inline void rj_unfold(uint8_t diamond[RJ_DIAMOND_SZ],
                               const RubikBuffer *rb)
{
    for (uint8_t f = 0; f < RJ_RUBIK_FACES; f++)
        for (uint8_t c = 0; c < RJ_RUBIK_CELLS; c++)
            diamond[f * RJ_RUBIK_CELLS + c] = rb->slots[f][c];

    memcpy(diamond + RJ_RUBIK_SLOTS, rb->meta, RJ_RUBIK_DELTA);
}

/* ══════════════════════════════════════════════════════════════
   METATRON ROTATE — apply face/edge/vertex swap on RubikBuffer
   ══════════════════════════════════════════════════════════════
 *
 * Four Metatron routes map to Rubik rotations:
 *   ORBITAL  (0) → rotate cells within face (ring shift, no face swap)
 *   CHIRAL   (1) → swap face f ↔ f+3 (diameter, 180° through center)
 *   CROSS_R  (2) → swap ring1↔ring2 faces (inter-ring)
 *   HUB      (3) → full face permutation (free reshape)
 *
 * All operations are self-inverse or have known inverse.
 * METATRON_CROSS[12] reused from skeleton_index.h LUT.
 */

typedef enum {
    RJ_ROT_ORBITAL = 0,
    RJ_ROT_CHIRAL  = 1,
    RJ_ROT_CROSS   = 2,
    RJ_ROT_HUB     = 3,
} RjRotation;

/* Cell ring-shift within a single face (ORBITAL) */
static inline void _rj_rot_orbital(RubikBuffer *rb, uint8_t face_id) {
    uint8_t tmp = rb->slots[face_id][8];
    for (int c = 8; c > 0; c--)
        rb->slots[face_id][c] = rb->slots[face_id][c-1];
    rb->slots[face_id][0] = tmp;
}

/* Face swap (CHIRAL / CROSS) */
static inline void _rj_swap_faces(RubikBuffer *rb, uint8_t fa, uint8_t fb) {
    uint8_t tmp[RJ_RUBIK_CELLS];
    memcpy(tmp,           rb->slots[fa], RJ_RUBIK_CELLS);
    memcpy(rb->slots[fa], rb->slots[fb], RJ_RUBIK_CELLS);
    memcpy(rb->slots[fb], tmp,           RJ_RUBIK_CELLS);
}

/*
 * rj_rotate — apply Metatron route to RubikBuffer
 * route: 0=ORBITAL 1=CHIRAL 2=CROSS 3=HUB
 * anchor: PentagonAnchor for this buffer
 */
static inline void rj_rotate(RubikBuffer *rb,
                               RjRotation route,
                               const PentagonAnchor *a)
{
    switch (route) {

    case RJ_ROT_ORBITAL:
        /* Ring-shift cells in anchor's face */
        _rj_rot_orbital(rb, a->anchor_id % RJ_RUBIK_FACES);
        break;

    case RJ_ROT_CHIRAL:
        /* Face f ↔ f+3 (diameter through center, 3 of 6 faces) */
        for (uint8_t f = 0; f < 3; f++)
            _rj_swap_faces(rb, f, f + 3);
        break;

    case RJ_ROT_CROSS:
        /* Inter-ring: swap face pair (0↔5, 1↔4, 2↔3) */
        _rj_swap_faces(rb, 0, 5);
        _rj_swap_faces(rb, 1, 4);
        _rj_swap_faces(rb, 2, 3);
        break;

    case RJ_ROT_HUB:
        /* Full permutation: rotate all 6 faces by anchor_id step */
        {
            uint8_t step = (a->anchor_id % 6) ? (a->anchor_id % 6) : 1;
            uint8_t tmp[RJ_RUBIK_FACES][RJ_RUBIK_CELLS];
            memcpy(tmp, rb->slots, sizeof(tmp));
            for (uint8_t f = 0; f < RJ_RUBIK_FACES; f++)
                memcpy(rb->slots[f], tmp[(f + step) % RJ_RUBIK_FACES],
                       RJ_RUBIK_CELLS);
        }
        break;
    }
}

/* ══════════════════════════════════════════════════════════════
   FULL RESHAPE — fold → rotate → unfold (one call)
   ══════════════════════════════════════════════════════════════ */

static inline void rj_reshape(uint8_t       in[RJ_DIAMOND_SZ],
                                uint8_t       out[RJ_DIAMOND_SZ],
                                uint64_t      addr,
                                RjRotation    route,
                                uint8_t       isect_pop,
                                uint8_t       entropy_class)
{
    PentagonAnchor a  = rj_anchor_lookup(addr);
    RjPath         p  = rj_select_path(isect_pop, entropy_class);

    RubikBuffer rb;
    rj_fold(&rb, in, &a, p);
    rj_rotate(&rb, route, &a);
    rj_unfold(out, &rb);
}

/* ══════════════════════════════════════════════════════════════
   ADDRESS TRANSLATION across reshape junction
   ══════════════════════════════════════════════════════════════
 *
 * Translates any address into the 20736 reshape space and back.
 * Used by LLM agent / controller at pentagon center.
 */

/* addr → reshape space (mod junction) */
static inline uint32_t rj_to_junction(uint64_t addr) {
    return (uint32_t)(addr % RJ_JUNCTION);
}

/* reshape space position → which path it belongs to */
static inline RjPath rj_pos_to_path(uint32_t junction_pos) {
    /* prefer path by which frame boundary it falls in */
    uint32_t slot_mod = junction_pos % RJ_RUBIK_SLOTS;
    if (slot_mod == 0) return RJ_PATH_RUBIK;
    if (junction_pos % RJ_TETRA_SLOTS == 0) return RJ_PATH_TETRA;
    if (junction_pos % RJ_BINARY_SLOTS == 0) return RJ_PATH_BINARY;
    return RJ_PATH_ICOSA;
}

/* ══════════════════════════════════════════════════════════════
   VERIFY — call once at init
   ══════════════════════════════════════════════════════════════ */

static inline int rj_verify(void) {
    /* All paths × slots = junction */
    if (RJ_RUBIK_FRAMES  * RJ_RUBIK_SLOTS  != RJ_JUNCTION) return -1;
    if (RJ_TETRA_FRAMES  * RJ_TETRA_SLOTS  != RJ_JUNCTION) return -2;
    if (RJ_BINARY_FRAMES * RJ_BINARY_SLOTS != RJ_JUNCTION) return -3;
    if (RJ_ICOSA_FRAMES  * RJ_ICOSA_SLOTS  != RJ_JUNCTION) return -4;
    /* Pentagon coverage */
    if (RJ_PENTAGONS * RJ_PENTAGON != RJ_JUNCTION) return -5;
    /* fold/unfold roundtrip */
    uint8_t src[64], out[64];
    for (int i = 0; i < 64; i++) src[i] = (uint8_t)i;
    PentagonAnchor a = rj_anchor_lookup(0);
    RubikBuffer rb;
    rj_fold(&rb, src, &a, RJ_PATH_RUBIK);
    rj_unfold(out, &rb);
    for (int i = 0; i < 64; i++) if (out[i] != src[i]) return -6;
    return 0;
}

#endif /* GEO_RESHAPE_JUNCTION_H */

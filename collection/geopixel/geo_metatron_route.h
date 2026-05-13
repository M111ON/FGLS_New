/*
 * geo_metatron_route.h — Metatron's Cube × TRing Routing Layer
 * ═════════════════════════════════════════════════════════════
 *
 * Additive layer บน geo_tring_walk.h — ไม่แตะ 5-tetra core
 * sacred numbers ยังอยู่ครบ (1440, 3456, 6912)
 *
 * Metatron's Cube → tring mapping:
 *   13 circles   → 12 pentagon faces + 1 center hub
 *   diameter line → TRING_CPAIR (face f ↔ face f+6)
 *   inter-ring    → METATRON_CROSS (non-chiral, self-inverse)
 *   missing circle → tring_first_gap() self-heal
 *
 * Route hierarchy (all O(1), zero search, zero pointer):
 *   Orbital  → same face, slot wrap (1 mod op)
 *   Chiral   → TRING_CPAIR = (enc+720)%1440  (1 add op)
 *   Cross    → METATRON_CROSS LUT  (1 LUT op)
 *   Hub      → any face via center  (2 LUT ops)
 *
 * Circuit Switch integration (tri[5] FLOW condition):
 *   tri[5] = metatron_cond(enc)  = face*3 + enc%3
 *            range 0..35, 36=2²×3² ✅ sacred family (unchanged)
 *   key    = metatron_key(enc)   = (face+6)%12 * 3 + enc%3
 *            chiral complement → WARP gate condition
 *   match  : key == (tri5 + 18) % 36  (half-ring shift, unchanged)
 *
 * Sacred constants: FROZEN
 *   TRING_FACES     = 12   (dodecahedron pentagons)
 *   TRING_FACE_SZ   = 120  (slots per face, 1440/12)
 *   TRING_CYCLE     = 1440 (12×120)
 *   TRING_HALF      = 720  (chiral half = 6 faces × 120)
 *   META_COND_MOD   = 36   (2²×3², trit×face hybrid — UNCHANGED)
 *   META_COND_HALF  = 18   (36/2 = chiral offset in cond space — UNCHANGED)
 *
 * NOTE: 120 and 1440 contain factor 5 → NOT in 2^a×3^b family
 *       Known tension — same resolution as before:
 *       only use %27, %36, %144 for sacred conditions.
 *       120-based slot indexing stays as geometry coordinate only.
 *
 * CPAIR self-inverse at 1440: verified ✓
 *   cpair(cpair(enc)) == enc for all enc 0..1439
 *
 * No malloc. No float. No pointers. Stateless O(1).
 * ═════════════════════════════════════════════════════════════
 */

#ifndef GEO_METATRON_ROUTE_H
#define GEO_METATRON_ROUTE_H

#include <stdint.h>
#include "geo_tring_walk.h"   /* TRING_WALK_CYCLE=1440, tring_walk_enc() */

/* ═══════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════ */

#define META_FACES        12u    /* dodecahedron compound faces        */
#define META_FACE_SZ     120u    /* slots per face (1440/12)           */
#define META_HALF        720u    /* chiral offset = 6×120              */
#define META_COND_MOD     36u    /* 2²×3² — sacred condition space     */
#define META_COND_HALF    18u    /* half of condition space            */

/* ═══════════════════════════════════════════════════════════
   PRIMITIVE DECOMPOSITION
   ═══════════════════════════════════════════════════════════ */

/* face index: 0..11 */
static inline uint8_t meta_face(uint16_t enc)
{
    return (uint8_t)(enc / META_FACE_SZ);
}

/* slot within face: 0..119 */
static inline uint8_t meta_slot(uint16_t enc)
{
    return (uint8_t)(enc % META_FACE_SZ);
}

/* ring index: 0 = face 0..5, 1 = face 6..11 */
static inline uint8_t meta_ring(uint16_t enc)
{
    return (uint8_t)(meta_face(enc) / 6u);
}

/* ═══════════════════════════════════════════════════════════
   LAYER 1 — ORBITAL (same face, circular)
   wrap within current face: slot→slot+1 mod 120
   ═══════════════════════════════════════════════════════════ */

static inline uint16_t meta_orbital(uint16_t enc)
{
    uint16_t base = (uint16_t)(meta_face(enc) * META_FACE_SZ);
    uint16_t next = (uint16_t)((meta_slot(enc) + 1u) % META_FACE_SZ);
    return base + next;
}

/* ═══════════════════════════════════════════════════════════
   LAYER 2 — CHIRAL (TRING_CPAIR)
   face f ↔ face f+6  |  enc → (enc + 720) % 1440
   = diameter line in Metatron's Cube
   Self-inverse: cpair(cpair(enc)) == enc ✓ (verified)
   ═══════════════════════════════════════════════════════════ */

static inline uint16_t meta_cpair(uint16_t enc)
{
    return (uint16_t)((enc + META_HALF) % TRING_WALK_CYCLE);
}

/* ═══════════════════════════════════════════════════════════
   LAYER 3 — CROSS (METATRON_CROSS)
   Non-chiral inter-ring bijection: ring1↔ring2, offset +3 within ring
   face f → (f+3)%6 + 6  (ring1→ring2)
   face f → (f-6+3)%6    (ring2→ring1)
   Self-inverse: cross(cross(enc)) == enc ✓
   Mathematically: 90° rotation in 6-face ring (3 steps × 60° = 180°)
   ═══════════════════════════════════════════════════════════ */

/*
 * METATRON_CROSS[12]: face → partner face (unchanged, face-space only)
 *   ring1 (face 0..5) → ring2 (face 6..11): face+3 in ring, +6 offset
 *   ring2 (face 6..11) → ring1 (face 0..5): face-6+3 in ring
 *   Result: {9,10,11,6,7,8, 3,4,5,0,1,2}
 */
static const uint8_t METATRON_CROSS[META_FACES] = {
    9, 10, 11, 6, 7, 8,   /* ring1 → ring2 */
    3,  4,  5, 0, 1, 2,   /* ring2 → ring1 */
};

static inline uint16_t meta_cross(uint16_t enc)
{
    uint8_t face    = meta_face(enc);
    uint8_t slot    = meta_slot(enc);
    uint8_t partner = METATRON_CROSS[face];
    return (uint16_t)(partner * META_FACE_SZ + slot);
}

/* ═══════════════════════════════════════════════════════════
   LAYER 4 — HUB (center node routing)
   Any face → center → any face
   Hub enc = virtual slot 1440 (outside ring, special address)
   In practice: hub routes to canonical face entry (slot 0 of target)
   Cost: 2 LUT ops (face→hub→face)
   ═══════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  active_faces;   /* bitmask: which of 12 faces are reachable */
    uint16_t hub_enc;        /* current hub position (canonical) */
} MetatronHub;

/* route enc through hub to target_face — returns entry enc of target */
static inline uint16_t meta_hub_route(uint16_t enc, uint8_t target_face,
                                       const MetatronHub *hub)
{
    (void)enc;  /* hub is face-agnostic — any source reaches any target */
    if (!((hub->active_faces >> (target_face % META_FACES)) & 1u))
        return hub->hub_enc;  /* unreachable — return hub itself */
    return (uint16_t)((target_face % META_FACES) * META_FACE_SZ);
}

/* ═══════════════════════════════════════════════════════════
   FULL METATRON WALK DECISION
   Priority: Orbital < Cross < Chiral < Hub
   ═══════════════════════════════════════════════════════════ */

typedef enum {
    META_ROUTE_ORBITAL = 0,  /* same face */
    META_ROUTE_CHIRAL  = 1,  /* opposite face (diameter) */
    META_ROUTE_CROSS   = 2,  /* non-chiral inter-ring */
    META_ROUTE_HUB     = 3,  /* any face via center */
} MetaRouteType;

typedef struct {
    uint16_t      next_enc;
    MetaRouteType type;
} MetaDecision;

/*
 * meta_route(src, dst_face):
 *   Chooses cheapest route from src enc to dst_face
 *   dst_face = UINT8_MAX → stay orbital (no specific target)
 */
static inline MetaDecision meta_route(uint16_t src, uint8_t dst_face)
{
    MetaDecision d;
    uint8_t src_face = meta_face(src);

    if (dst_face == 0xFFu || dst_face == src_face) {
        d.next_enc = meta_orbital(src);
        d.type     = META_ROUTE_ORBITAL;
    } else if (dst_face == (src_face + 6u) % META_FACES) {
        d.next_enc = meta_cpair(src);
        d.type     = META_ROUTE_CHIRAL;
    } else if (METATRON_CROSS[src_face] == dst_face) {
        d.next_enc = meta_cross(src);
        d.type     = META_ROUTE_CROSS;
    } else {
        /* Hub: jump to entry slot of target face */
        d.next_enc = (uint16_t)(dst_face * META_FACE_SZ);
        d.type     = META_ROUTE_HUB;
    }
    return d;
}

/* ═══════════════════════════════════════════════════════════
   CIRCUIT SWITCH INTEGRATION — tri[5] FLOW condition
   ═══════════════════════════════════════════════════════════
 *
 * Condition space: 36 = 2²×3² ✅ sacred family (UNCHANGED)
 * cond = face * 3 + enc % 3
 *   face: 0..11 (geometric zone)
 *   enc%3: 0..2 (trit sub-position)
 *   combined: 0..35, 36 distinct zones
 *
 * WARP gate: key must be chiral complement in cond space
 *   key  = meta_key(enc)  = (face+6)%12 * 3 + enc%3
 *   match: (cond + META_COND_HALF) % META_COND_MOD == key
 *        = key is cond shifted by half-ring (18 steps)
 *
 * enc%3 is cycle-independent → cond logic unchanged at 1440 ✓
 */

static inline uint8_t metatron_cond(uint16_t enc)
{
    return (uint8_t)(meta_face(enc) * 3u + enc % 3u);
}

static inline uint8_t metatron_key(uint16_t enc)
{
    uint8_t pair_face = (meta_face(enc) + 6u) % META_FACES;
    return (uint8_t)(pair_face * 3u + enc % 3u);
}

/* circuit_trip: does requester_enc hold the WARP key for folded_cond?
 * Property: metatron_cond(folder) - metatron_cond(cpair_requester) == 18 (mod 36)
 * → trip when cond(requester) == (folded_cond + 18) % 36
 */
static inline int metatron_trip(uint8_t folded_cond, uint16_t requester_enc)
{
    uint8_t req_cond = metatron_cond(requester_enc);
    return req_cond == (uint8_t)((folded_cond + META_COND_HALF) % META_COND_MOD);
}

/* ═══════════════════════════════════════════════════════════
   CIRCUIT SWITCH API  (wraps tri[2]/tri[5] concept)
   Minimal cell struct — tri[6] array
   ═══════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t tri[6];   /* tri[2]=dest, tri[5]=cond, others caller-defined */
} MetatronCell;

#define META_TRI_DEST  2u
#define META_TRI_COND  5u
#define META_OPEN      UINT32_MAX   /* open circuit sentinel */

/* arm: set relay condition from geometric enc */
static inline void circuit_arm(MetatronCell *cell,
                                 uint32_t dest_hid,
                                 uint16_t enc)
{
    cell->tri[META_TRI_DEST] = dest_hid;
    cell->tri[META_TRI_COND] = (uint32_t)metatron_cond(enc);
}

/* resolve: check if requester_enc trips the relay */
static inline uint32_t circuit_resolve(const MetatronCell *cell,
                                        uint16_t requester_enc)
{
    uint8_t cond = (uint8_t)(cell->tri[META_TRI_COND] & 0xFFu);
    if (cell->tri[META_TRI_DEST] == META_OPEN) return META_OPEN;
    return metatron_trip(cond, requester_enc)
           ? cell->tri[META_TRI_DEST]
           : META_OPEN;
}

/* ═══════════════════════════════════════════════════════════
   GEOFACE ROUTER — inter-geometry face translation
   Uses shared invariant: 12 compounds in both TETRA and OCTA
   Wires into geo_compound_cfg.h geo_face_route()
   ═══════════════════════════════════════════════════════════ */

/*
 * meta_geoface_enc(src_enc, src_face_sz, dst_face_sz):
 *   Translate enc across geometry boundary
 *   Preserves: face_id % 12, slot proportional scaling
 *
 * Example (1440→other geometry):
 *   src_face_sz = 120 (1440/12)
 *   dst_face_sz = 128 (1536/12)
 *   slot scaled: src_slot × 128/120
 */
static inline uint32_t meta_geoface_enc(uint16_t src_enc,
                                         uint32_t src_face_sz,
                                         uint32_t dst_face_sz)
{
    uint32_t face = src_enc / src_face_sz;
    uint32_t slot = src_enc % src_face_sz;
    /* proportional scale: slot × dst / src */
    uint32_t dst_slot = (slot * dst_face_sz) / src_face_sz;
    return face * dst_face_sz + dst_slot;
}

/* ═══════════════════════════════════════════════════════════
   VERIFY — call once at init
   ═══════════════════════════════════════════════════════════ */

static inline int geo_metatron_verify(void)
{
    /* CROSS self-inverse */
    for (uint8_t f = 0; f < META_FACES; f++) {
        if (METATRON_CROSS[METATRON_CROSS[f]] != f) return -1;
    }
    /* CPAIR self-inverse — verified at 1440 ✓ */
    for (uint16_t e = 0; e < TRING_WALK_CYCLE; e++) {
        if (meta_cpair(meta_cpair(e)) != e) return -2;
    }
    /* cond range */
    for (uint16_t e = 0; e < TRING_WALK_CYCLE; e++) {
        if (metatron_cond(e) >= META_COND_MOD) return -3;
        if (metatron_key(e)  >= META_COND_MOD) return -4;
    }
    /* META_COND_MOD in 2^a*3^b family: 36=2^2*3^2 ✓ */
    return 0;
}

#endif /* GEO_METATRON_ROUTE_H */

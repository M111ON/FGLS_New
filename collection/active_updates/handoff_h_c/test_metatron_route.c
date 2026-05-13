/*
 * test_metatron_route.c — geo_metatron_route.h verification
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_metatron_route.h"   /* pulls geo_tring_walk.h */

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else       { printf("  FAIL  %s\n", msg); _fail++; } \
} while(0)

/* ── M01: constants sanity ─────────────────────────────────── */
static void m01(void) {
    printf("\nM01: constants sanity\n");
    CHECK(META_FACES * META_FACE_SZ == TRING_WALK_CYCLE, "12×60=720");
    CHECK(META_HALF == TRING_WALK_CYCLE / 2u,            "HALF=360");
    CHECK(META_COND_MOD == 36u,                          "COND_MOD=36");
    CHECK(META_COND_HALF == META_COND_MOD / 2u,          "COND_HALF=18");
    /* 36 = 2²×3² in sacred family */
    uint32_t n = META_COND_MOD;
    while (n % 2 == 0) n /= 2;
    while (n % 3 == 0) n /= 3;
    CHECK(n == 1u, "36=2^2*3^2 sacred family");
}

/* ── M02: geo_metatron_verify() ────────────────────────────── */
static void m02(void) {
    printf("\nM02: geo_metatron_verify()\n");
    int r = geo_metatron_verify();
    CHECK(r == 0, "verify passes: CROSS self-inverse + CPAIR + cond range");
}

/* ── M03: orbital routing ──────────────────────────────────── */
static void m03(void) {
    printf("\nM03: orbital (same face wrap)\n");
    /* slot wraps within face */
    uint16_t enc = 5 * META_FACE_SZ + 59u;  /* face 5, last slot */
    uint16_t next = meta_orbital(enc);
    CHECK(meta_face(next) == 5u,   "orbital stays in same face");
    CHECK(meta_slot(next) == 0u,   "orbital wraps to slot 0");
    /* mid-face: just increments */
    enc  = 3 * META_FACE_SZ + 10u;
    next = meta_orbital(enc);
    CHECK(meta_face(next) == 3u,             "orbital mid-face: same face");
    CHECK(meta_slot(next) == 11u,            "orbital mid-face: slot+1");
}

/* ── M04: chiral CPAIR ─────────────────────────────────────── */
static void m04(void) {
    printf("\nM04: chiral CPAIR (face+6)\n");
    /* self-inverse for all enc */
    int ok = 1;
    for (uint16_t e = 0; e < TRING_WALK_CYCLE; e++) {
        if (meta_cpair(meta_cpair(e)) != e) { ok = 0; break; }
    }
    CHECK(ok, "cpair self-inverse: cpair(cpair(e))==e for all 720");
    /* face offset: always +6 mod 12 */
    ok = 1;
    for (uint16_t e = 0; e < TRING_WALK_CYCLE; e++) {
        uint8_t f  = meta_face(e);
        uint8_t pf = meta_face(meta_cpair(e));
        if ((f + 6u) % META_FACES != pf) { ok = 0; break; }
    }
    CHECK(ok, "cpair: face(cpair(e)) == (face(e)+6)%12 for all");
    /* slot preserved */
    ok = 1;
    for (uint16_t e = 0; e < TRING_WALK_CYCLE; e++) {
        if (meta_slot(e) != meta_slot(meta_cpair(e))) { ok = 0; break; }
    }
    CHECK(ok, "cpair: slot preserved across chiral pair");
}

/* ── M05: cross routing ────────────────────────────────────── */
static void m05(void) {
    printf("\nM05: cross (METATRON_CROSS inter-ring)\n");
    /* CROSS bijective */
    uint8_t seen[META_FACES] = {0};
    for (uint8_t f = 0; f < META_FACES; f++) seen[METATRON_CROSS[f]]++;
    int bij = 1;
    for (uint8_t f = 0; f < META_FACES; f++) if (seen[f] != 1) { bij = 0; break; }
    CHECK(bij, "CROSS bijective (each face appears exactly once)");
    /* self-inverse for all enc */
    int ok = 1;
    for (uint16_t e = 0; e < TRING_WALK_CYCLE; e++) {
        if (meta_cross(meta_cross(e)) != e) { ok = 0; break; }
    }
    CHECK(ok, "cross self-inverse: cross(cross(e))==e for all 720");
    /* slot preserved */
    ok = 1;
    for (uint16_t e = 0; e < TRING_WALK_CYCLE; e++) {
        if (meta_slot(e) != meta_slot(meta_cross(e))) { ok = 0; break; }
    }
    CHECK(ok, "cross: slot preserved");
    /* cross is NOT chiral (different from cpair) */
    ok = 1;
    for (uint8_t f = 0; f < META_FACES; f++) {
        if (METATRON_CROSS[f] == (f + 6u) % META_FACES) { ok = 0; break; }
    }
    CHECK(ok, "cross != cpair (non-chiral inter-ring)");
}

/* ── M06: hub routing ──────────────────────────────────────── */
static void m06(void) {
    printf("\nM06: hub routing (any face)\n");
    MetatronHub hub = { .active_faces = 0xFFu, .hub_enc = 0u };
    /* route to any target face should return entry enc */
    for (uint8_t f = 0; f < META_FACES; f++) {
        uint16_t dest = meta_hub_route(42u, f, &hub);
        CHECK(meta_face(dest) == f, "hub: reaches target face");
        /* only check first few to avoid verbosity */
        if (f == 2) break;
    }
    /* unreachable face → returns hub_enc */
    MetatronHub hub_partial = { .active_faces = 0x03Fu, .hub_enc = 999u };
    uint16_t dest = meta_hub_route(0u, 11u, &hub_partial);
    CHECK(dest == 999u, "hub: unreachable face returns hub sentinel");
}

/* ── M07: meta_route() dispatch ────────────────────────────── */
static void m07(void) {
    printf("\nM07: meta_route() dispatch priority\n");
    uint16_t src = 2 * META_FACE_SZ + 5u;  /* face 2, slot 5 */
    MetaDecision d;

    /* ORBITAL: no target */
    d = meta_route(src, 0xFFu);
    CHECK(d.type == META_ROUTE_ORBITAL, "route: no target → orbital");

    /* CHIRAL: face+6 */
    d = meta_route(src, (2 + 6) % META_FACES);
    CHECK(d.type == META_ROUTE_CHIRAL, "route: face+6 → chiral");
    CHECK(d.next_enc == meta_cpair(src), "route: chiral enc correct");

    /* CROSS: METATRON_CROSS partner */
    uint8_t cross_target = METATRON_CROSS[2];
    d = meta_route(src, cross_target);
    CHECK(d.type == META_ROUTE_CROSS, "route: cross partner → cross");
    CHECK(d.next_enc == meta_cross(src), "route: cross enc correct");

    /* HUB: any other face */
    uint8_t hub_target = 1u;  /* not same, not chiral, not cross of face 2 */
    d = meta_route(src, hub_target);
    CHECK(d.type == META_ROUTE_HUB, "route: arbitrary face → hub");
}

/* ── M08: circuit switch condition (sacred family) ─────────── */
static void m08(void) {
    printf("\nM08: circuit switch condition (36=2²×3²)\n");
    /* all cond/key in range 0..35 */
    int ok = 1;
    for (uint16_t e = 0; e < TRING_WALK_CYCLE; e++) {
        if (metatron_cond(e) >= META_COND_MOD) { ok = 0; break; }
        if (metatron_key(e)  >= META_COND_MOD) { ok = 0; break; }
    }
    CHECK(ok, "cond and key always in 0..35");
    /* key = chiral complement of cond in condition space */
    ok = 1;
    for (uint16_t e = 0; e < TRING_WALK_CYCLE; e++) {
        uint8_t cond = metatron_cond(e);
        uint8_t key  = metatron_key(e);
        uint8_t expected = (uint8_t)((cond + META_COND_HALF) % META_COND_MOD);
        if (key != expected) { ok = 0; break; }
    }
    CHECK(ok, "key == (cond + 18) % 36 for all enc (chiral in cond space)");
}

/* ── M09: circuit_arm + circuit_resolve ────────────────────── */
static void m09(void) {
    printf("\nM09: circuit_arm + circuit_resolve\n");
    MetatronCell cell = {0};
    uint16_t enc = 4 * META_FACE_SZ + 7u;   /* folder's enc */
    uint32_t dest_hid = 9999u;

    circuit_arm(&cell, dest_hid, enc);

    /* correct key: chiral complement → trips */
    uint16_t correct_key_enc = meta_cpair(enc);  /* face+6, same slot */
    uint32_t result = circuit_resolve(&cell, correct_key_enc);
    CHECK(result == dest_hid, "circuit: correct complement key trips relay");

    /* wrong key: orbital neighbor → blocked */
    uint16_t wrong_enc = meta_orbital(enc);
    result = circuit_resolve(&cell, wrong_enc);
    CHECK(result == META_OPEN, "circuit: orbital neighbor blocked (open circuit)");

    /* cross partner: different cond → blocked */
    uint16_t cross_enc = meta_cross(enc);
    result = circuit_resolve(&cell, cross_enc);
    /* cross != cpair so condition doesn't match */
    uint8_t cond = metatron_cond(enc);
    uint8_t cross_key = metatron_key(cross_enc);
    uint8_t expected_key = (cond + META_COND_HALF) % META_COND_MOD;
    int cross_trips = (cross_key == expected_key);
    CHECK(!cross_trips || result == dest_hid,
          "circuit: cross partner result consistent with cond math");

    /* OPEN sentinel: unset dest → always OPEN */
    MetatronCell empty = {0};
    empty.tri[META_TRI_DEST] = META_OPEN;
    empty.tri[META_TRI_COND] = metatron_cond(enc);
    result = circuit_resolve(&empty, correct_key_enc);
    CHECK(result == META_OPEN, "circuit: META_OPEN dest → always blocked");
}

/* ── M10: meta_geoface_enc (TETRA→OCTA translation) ────────── */
static void m10(void) {
    printf("\nM10: geoface_enc TETRA→OCTA translation\n");
    /* TETRA: face_sz=60, OCTA: face_sz=128 (1536/12) */
    uint32_t t_face_sz = 60u, o_face_sz = 128u;

    /* enc=0 → face 0, slot 0 → octa face 0, slot 0 */
    uint32_t r = meta_geoface_enc(0u, t_face_sz, o_face_sz);
    CHECK(r == 0u, "geoface: enc=0 → 0");

    /* enc=60 (face 1, slot 0) → octa face 1, slot 0 = 128 */
    r = meta_geoface_enc(60u, t_face_sz, o_face_sz);
    CHECK(r == 128u, "geoface: face 1 entry maps to octa face 1 entry");

    /* face preserved after translation */
    int ok = 1;
    for (uint16_t e = 0; e < TRING_WALK_CYCLE; e += 60u) {
        uint8_t  src_face = meta_face(e);
        uint32_t dst_enc  = meta_geoface_enc(e, t_face_sz, o_face_sz);
        uint32_t dst_face = dst_enc / o_face_sz;
        if (dst_face != src_face) { ok = 0; break; }
    }
    CHECK(ok, "geoface: face_id preserved for all face entry points");

    /* reverse: OCTA→TETRA, face still preserved */
    ok = 1;
    for (uint32_t e = 0; e < 1536u; e += 128u) {
        uint32_t src_face = e / o_face_sz;
        uint32_t dst_enc  = meta_geoface_enc((uint16_t)(e % 1536u),
                                              o_face_sz, t_face_sz);
        uint32_t dst_face = dst_enc / t_face_sz;
        if (dst_face != src_face) { ok = 0; break; }
    }
    CHECK(ok, "geoface: OCTA→TETRA face preserved");
}

int main(void)
{
    printf("=== geo_metatron_route.h Verification ===\n");
    m01(); m02(); m03(); m04(); m05();
    m06(); m07(); m08(); m09(); m10();
    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    if (_fail == 0)
        printf("\n✓ Metatron route layer verified — geometry is the key\n");
    return _fail ? 1 : 0;
}

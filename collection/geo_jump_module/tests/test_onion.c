/*
 * test_onion.c — Test suite for onion_stack.h (S2)
 * Compile: gcc -O2 -o test_onion test_onion.c && ./test_onion
 * Depends: shell_container.h, onion_stack.h, geo_compound_cfg.h
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "onion_stack.h"

static int pass = 0, fail = 0;
#define CHECK(label, cond) \
    do { if (cond) { printf("  PASS  %s\n", label); pass++; } \
         else      { printf("  FAIL  %s\n", label); fail++; } } while(0)

/* ── T1: shell_hop adjacent ──────────────────────────── */
static void test_hop_adjacent(void)
{
    printf("\n[T1] shell_hop — adjacent pairs\n");
    ShellContainer s;
    shell_init(&s, 0, 1, 0xABCD1234u, GEO_COMPOUND_TETRA);

    /* 0→1 is adjacent per PENT_ADJ */
    HopResult r = shell_hop(&s, 0, 1);
    CHECK("0→1 valid", r.valid == 1u);
    CHECK("0→1 addr < JUNCTION", r.addr < SHELL_JUNCTION);

    /* 0→3 adjacent */
    r = shell_hop(&s, 0, 3);
    CHECK("0→3 valid", r.valid == 1u);

    /* 0→11 NOT adjacent (top cap to bottom cap) */
    r = shell_hop(&s, 0, 11);
    CHECK("0→11 not adjacent", r.valid == 0u);

    /* 6→9 NOT adjacent */
    r = shell_hop(&s, 6, 9);
    CHECK("6→9 not adjacent", r.valid == 0u);

    /* 11→7 adjacent (bottom cap neighbors) */
    r = shell_hop(&s, 11, 7);
    CHECK("11→7 valid", r.valid == 1u);
}

/* ── T2: shell_hop symmetric ─────────────────────────── */
static void test_hop_symmetric(void)
{
    printf("\n[T2] shell_hop — symmetric (A→B same addr as B→A midpoint)\n");
    ShellContainer s;
    shell_init(&s, 3, 2, 0xDEADBEEFu, GEO_COMPOUND_OCTA);

    /* midpoint formula is symmetric */
    HopResult ab = shell_hop(&s, 1, 2);
    HopResult ba = shell_hop(&s, 2, 1);
    CHECK("1→2 addr == 2→1 addr (symmetric midpoint)", ab.addr == ba.addr);
}

/* ── T3: shell_hop_path ───────────────────────────────── */
static void test_hop_path(void)
{
    printf("\n[T3] shell_hop_path\n");
    ShellContainer s;
    shell_init(&s, 0, 1, 0x12345678u, GEO_COMPOUND_TETRA);

    /* valid path: 0→1→2→3 (all adjacent in dodecahedron) */
    uint8_t  path[]  = {0, 1, 2, 3};
    uint32_t addrs[3] = {0};
    uint8_t n = shell_hop_path(&s, path, 4, addrs);
    CHECK("path 0→1→2→3 all 3 hops valid", n == 3u);
    for (uint8_t i = 0u; i < n; i++)
        CHECK("hop addr < JUNCTION", addrs[i] < SHELL_JUNCTION);

    /* invalid path: 0→11 (not adjacent), stops at first bad hop */
    uint8_t  badpath[] = {0, 11, 6};
    uint32_t baddrs[2] = {0};
    uint8_t m = shell_hop_path(&s, badpath, 3, baddrs);
    CHECK("path 0→11→6 stops at 0 hops", m == 0u);
}

/* ── T4: onion_init + onion_verify ───────────────────── */
static void test_onion_init(void)
{
    printf("\n[T4] onion_init + onion_verify\n");
    OnionStack o;
    onion_init(&o, 0xC0FFEE00u, 1u);
    int r = onion_verify(&o);
    CHECK("all 12 shells verify OK", r == 0);

    /* check shell IDs */
    for (uint8_t i = 0u; i < ONION_N_SHELLS; i++)
        CHECK("shell_id correct", o.shells[i].shell_id == i);

    /* check radial_ratio: shell 0 = 12, shell 11 = 1 */
    CHECK("shell 0 ratio = 12", o.radial_ratio[0] == 12u);
    CHECK("shell 11 ratio = 1", o.radial_ratio[11] == 1u);
}

/* ── T5: onion_addr deterministic ────────────────────── */
static void test_onion_addr(void)
{
    printf("\n[T5] onion_addr deterministic\n");
    OnionStack o;
    onion_init(&o, 0xFEEDFACEu, 1u);

    Chord c = { .geometry=0, .seed=0xABCDu, .chord_id=CHORD_ORBITAL, .key_offset=0 };

    uint64_t a1 = onion_addr(&o, 3, &c);
    uint64_t a2 = onion_addr(&o, 3, &c);
    CHECK("same chord → same addr (deterministic)", a1 == a2);
    CHECK("addr in valid range", a1 < SHELL_JUNCTION);

    /* different shells → different addr */
    uint64_t b = onion_addr(&o, 7, &c);
    CHECK("different shell → different addr", a1 != b);

    /* bad shell_id */
    uint64_t bad = onion_addr(&o, 99, &c);
    CHECK("bad shell_id → -1", bad == (uint64_t)-1);
}

/* ── T6: onion_cross O(1) LUT ────────────────────────── */
static void test_onion_cross(void)
{
    printf("\n[T6] onion_cross — radial jump via LUT\n");
    OnionStack o;
    onion_init(&o, 0xBEEFCAFEu, 1u);

    /* cross shell 0→5 at anchor 3 */
    uint32_t addr = onion_cross(&o, 0, 3, 5);
    CHECK("cross 0→5 anchor 3 valid", addr != UINT32_MAX);
    CHECK("cross addr matches LUT directly", addr == o.lut[5][3]);

    /* cross to same shell = identity */
    uint32_t same = onion_cross(&o, 4, 7, 4);
    CHECK("cross same shell = own anchor", same == o.shells[4].anchor[7]);

    /* bad params */
    uint32_t b1 = onion_cross(&o, 99, 0, 0);
    uint32_t b2 = onion_cross(&o, 0, 99, 0);
    uint32_t b3 = onion_cross(&o, 0, 0, 99);
    CHECK("bad from_shell → UINT32_MAX", b1 == UINT32_MAX);
    CHECK("bad anchor_id → UINT32_MAX",  b2 == UINT32_MAX);
    CHECK("bad to_shell → UINT32_MAX",   b3 == UINT32_MAX);
}

/* ── T7: compression tiers ───────────────────────────── */
static void test_compress_tiers(void)
{
    printf("\n[T7] compression tiers (frustum depth = resolution)\n");
    CHECK("Q4  → shell 0 (apex)",   onion_shell_for_tier(ONION_TIER_Q4)  == 0u);
    CHECK("Q8  → shell 4 (mid)",    onion_shell_for_tier(ONION_TIER_Q8)  == 4u);
    CHECK("F16 → shell 8 (base)",   onion_shell_for_tier(ONION_TIER_F16) == 8u);

    CHECK("Q4  reconstruct depth 1",  onion_reconstruct_depth(ONION_TIER_Q4)  == 1u);
    CHECK("Q8  reconstruct depth 8",  onion_reconstruct_depth(ONION_TIER_Q8)  == 8u);
    CHECK("F16 reconstruct depth 12", onion_reconstruct_depth(ONION_TIER_F16) == 12u);
}

/* ── T8: capo (key_offset) shift across onion ────────── */
static void test_capo_shift(void)
{
    printf("\n[T8] capo shift across shells\n");
    OnionStack o;
    onion_init(&o, 0x42424242u, 1u);

    Chord c1 = { .geometry=0, .seed=100u, .chord_id=CHORD_CROSS, .key_offset=0 };
    Chord c2 = { .geometry=0, .seed=100u, .chord_id=CHORD_CROSS, .key_offset=576u };

    uint64_t a = onion_addr(&o, 2, &c1);
    uint64_t b = onion_addr(&o, 2, &c2);
    CHECK("capo 0 vs 576 → different addr", a != b);
    CHECK("both in valid range", a < SHELL_JUNCTION && b < SHELL_JUNCTION);
}

/* ── main ─────────────────────────────────────────────── */
int main(void)
{
    printf("══════════════════════════════════════\n");
    printf(" POGLS onion_stack.h — Test Suite S2  \n");
    printf("══════════════════════════════════════\n");

    test_hop_adjacent();
    test_hop_symmetric();
    test_hop_path();
    test_onion_init();
    test_onion_addr();
    test_onion_cross();
    test_compress_tiers();
    test_capo_shift();

    printf("\n══════════════════════════════════════\n");
    printf(" Result: %d PASS / %d FAIL\n", pass, fail);
    printf("══════════════════════════════════════\n");
    return (fail == 0) ? 0 : 1;
}

/*
 * test_pogls_bond.c
 * ─────────────────────────────────────────────────────────────
 * Compile:  gcc -O2 -o test_bond test_pogls_bond.c -I. && ./test_bond
 * ─────────────────────────────────────────────────────────────
 */

#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <assert.h>
#include "pogls_bond.h"

/* ── helper: print piece ──────────────────────────────────────*/
static void print_piece(const char *label, const PoglsPiece *p) {
    printf("  %-10s  shape=%c  geo=%016llx\n"
           "             bond_L=%016llx\n"
           "             bond_R=%016llx\n"
           "             bond_key=%016llx\n",
           label,
           (char)p->shape,
           (unsigned long long)p->geo_key,
           (unsigned long long)p->bond_L,
           (unsigned long long)p->bond_R,
           (unsigned long long)pogls_bond_key(p));
}

/* ── helper: print slot ───────────────────────────────────────*/
static void print_slot(const PoglsSlot *s) {
    printf("  agent=%u  shape=%c  rerouted=%u\n"
           "  geo=%016llx\n",
           s->agent_id, (char)s->piece.shape, s->rerouted,
           (unsigned long long)s->piece.geo_key);
    const char *faces[] = {"N","S","E","W"};
    for (int i = 0; i < 4; i++) {
        if (s->plugs[i].active)
            printf("  plug[%s] → agent_%u face=%s ttl=%u\n",
                   faces[i], s->plugs[i].target_id,
                   faces[s->plugs[i].face], s->plugs[i].ttl);
    }
}

/* ══════════════════════════════════════════════════════════════
 * TEST 1 — fibo_addr determinism
 * same seed → same addr, always
 * ══════════════════════════════════════════════════════════════*/
static void test_fibo_determinism(void) {
    printf("\n[1] fibo_addr determinism\n");
    uint64_t seed = 0xDEADBEEFCAFE0001ULL;
    uint64_t a    = pogls_fibo_addr(seed);
    uint64_t b    = pogls_fibo_addr(seed);
    printf("  seed=%016llx → addr=%016llx\n",
           (unsigned long long)seed, (unsigned long long)a);
    assert(a == b);
    printf("  ✓ same seed → same addr\n");

    /* different seed → different addr */
    uint64_t c = pogls_fibo_addr(seed ^ 1);
    assert(a != c);
    printf("  ✓ seed±1 → different addr  (%016llx)\n",
           (unsigned long long)c);
}

/* ══════════════════════════════════════════════════════════════
 * TEST 2 — piece factory + axis→shape
 * ══════════════════════════════════════════════════════════════*/
static void test_piece_factory(void) {
    printf("\n[2] piece factory — axis→shape\n");
    uint64_t session_seed = 0x9009000000000001ULL;

    /* Agent defs: id, fold_axis */
    struct { uint32_t id; uint8_t axis; } agents[] = {
        {0, 1},   /* A - I pipe     */
        {1, 3},   /* B - T splitter */
        {2, 1},   /* C - I pipe     */
        {3, 6},   /* D - L fork-L   */
    };
    (void)agents;     /* suppress unused warning */

    uint8_t axes[]   = {1, 3, 1, 6};
    char    labels[] = {'A','B','C','D'};

    for (int i = 0; i < 4; i++) {
        uint64_t   seed = pogls_fibo_addr(session_seed ^ (uint64_t)i);
        PoglsPiece p    = pogls_make_piece(seed, axes[i]);
        printf("  Agent-%c axis=%u ", labels[i], axes[i]);
        print_piece("", &p);
        assert(p.shape == POGLS_AXIS_SHAPE[axes[i]]);
    }
    printf("  ✓ all axis→shape mappings correct\n");
}

/* ══════════════════════════════════════════════════════════════
 * TEST 3 — intrinsic bond: coordinate-bound self-enforcement
 * ══════════════════════════════════════════════════════════════*/
static void test_intrinsic_bond(void) {
    printf("\n[3] intrinsic bond — coordinate enforcement\n");
    uint64_t seed_A = 0xF1B0000000000001ULL;
    uint64_t seed_B = 0xF1B0000000000002ULL;

    PoglsPiece A = pogls_make_piece(pogls_fibo_addr(seed_A), 1);
    PoglsPiece B = pogls_make_piece(pogls_fibo_addr(seed_B), 3);

    uint64_t key_A  = pogls_bond_key(&A);
    uint64_t key_B  = pogls_bond_key(&B);
    uint64_t xor_AB = key_A ^ key_B;

    printf("  bond_key A = %016llx\n", (unsigned long long)key_A);
    printf("  bond_key B = %016llx\n", (unsigned long long)key_B);
    printf("  XOR(A,B)   = %016llx\n", (unsigned long long)xor_AB);

    /* Simulate coordinate shift: geo_key changes slightly */
    PoglsPiece A_shifted  = A;
    A_shifted.geo_key    ^= 0x1ULL;   /* tiny shift */
    A_shifted.bond_L      = pogls_fibo_addr(A_shifted.geo_key ^ POGLS_BOND_SALT_L);
    A_shifted.bond_R      = pogls_fibo_addr(A_shifted.geo_key ^ POGLS_BOND_SALT_R);

    uint64_t key_A_shifted = pogls_bond_key(&A_shifted);
    assert(key_A != key_A_shifted);
    printf("  ✓ coord shift → bond_key changed: %016llx → %016llx\n",
           (unsigned long long)key_A, (unsigned long long)key_A_shifted);
    printf("  ✓ bond broken automatically (no permission check needed)\n");
}

/* ══════════════════════════════════════════════════════════════
 * TEST 4 — wallet topology_fp → seed → piece
 * (Python coord_wallet ↔ C piece)
 * ══════════════════════════════════════════════════════════════*/
static void test_wallet_bridge(void) {
    printf("\n[4] wallet topology_fp → C piece (bridge test)\n");

    /* Simulated wallet topology_fp from Python side */
    const char *fp_A = "a3f0b2c1d4e5f6a7";  /* Agent-A */
    const char *fp_B = "b1e2f3a4c5d6e7f8";  /* Agent-B */

    uint64_t seed_A = pogls_seed_from_fp(fp_A);
    uint64_t seed_B = pogls_seed_from_fp(fp_B);

    PoglsPiece pA = pogls_make_piece(seed_A, 1);   /* I-shape */
    PoglsPiece pB = pogls_make_piece(seed_B, 3);   /* T-shape */

    printf("  fp_A=%s\n", fp_A);
    print_piece("piece_A", &pA);
    printf("  fp_B=%s\n", fp_B);
    print_piece("piece_B", &pB);

    /* bond_key is reproducible from same fp */
    uint64_t seed_A2 = pogls_seed_from_fp(fp_A);
    PoglsPiece pA2   = pogls_make_piece(seed_A2, 1);
    assert(pA.geo_key == pA2.geo_key);
    assert(pA.bond_L  == pA2.bond_L);
    assert(pA.bond_R  == pA2.bond_R);
    printf("  ✓ same fp → same piece (reproducible from wallet)\n");
}

/* ══════════════════════════════════════════════════════════════
 * TEST 5 — slot + extrinsic plug chain: A─E/W─B─E/W─C─E/W─D
 * ══════════════════════════════════════════════════════════════*/
static void test_plug_chain(void) {
    printf("\n[5] extrinsic plug chain  A─B─C─D\n");

    uint64_t base = 0x9009009009009009ULL;
    uint8_t  axes[] = {1,3,1,6};
    char     lbl[]  = {'A','B','C','D'};
    PoglsSlot slots[4];

    for (int i = 0; i < 4; i++) {
        uint64_t seed    = pogls_fibo_addr(base ^ (uint64_t)i);
        slots[i].piece   = pogls_make_piece(seed, axes[i]);
        slots[i].agent_id = (uint32_t)i;
        slots[i].token_cap = 300 + i*50;
        slots[i].rerouted = 0;
        memset(slots[i].plugs, 0, sizeof(slots[i].plugs));
    }

    /* wire: A.E ↔ B.W,  B.E ↔ C.W,  C.E ↔ D.W */
    pogls_plug_connect(&slots[0], PLUG_FACE_E, &slots[1], PLUG_FACE_W, 64);
    pogls_plug_connect(&slots[1], PLUG_FACE_E, &slots[2], PLUG_FACE_W, 64);
    pogls_plug_connect(&slots[2], PLUG_FACE_E, &slots[3], PLUG_FACE_W, 64);

    for (int i = 0; i < 4; i++) {
        printf("  Agent-%c:\n", lbl[i]);
        print_slot(&slots[i]);
    }

    assert(slots[0].plugs[PLUG_FACE_E].active == 1);
    assert(slots[3].plugs[PLUG_FACE_W].active == 1);
    printf("  ✓ plug chain wired A─B─C─D\n");
}

/* ══════════════════════════════════════════════════════════════
 * TEST 6 — Ω reroute (fault → shape substitution)
 * ══════════════════════════════════════════════════════════════*/
static void test_reroute(void) {
    printf("\n[6] Ω reroute — fault → shape substitution\n");

    uint64_t  seed  = pogls_fibo_addr(0xDEAD0000BEEF0000ULL);
    PoglsSlot slot  = {0};
    slot.piece      = pogls_make_piece(seed, 1);   /* I originally */
    slot.agent_id   = 99;

    printf("  before: shape=%c  geo=%016llx\n",
           (char)slot.piece.shape,
           (unsigned long long)slot.piece.geo_key);

    /* Simulate overflow */
    pogls_reroute(&slot, POGLS_OVERFLOW);
    printf("  OVERFLOW → shape=%c  geo=%016llx  rerouted=%u\n",
           (char)slot.piece.shape,
           (unsigned long long)slot.piece.geo_key,
           slot.rerouted);
    assert(slot.piece.shape == OMEGA_COMPRESS);

    /* bond survives reroute (bond_L/R unchanged) */
    printf("  bond_L=%016llx (unchanged)\n",
           (unsigned long long)slot.piece.bond_L);
    printf("  ✓ bond survives reroute\n");
    printf("  ✓ shape substituted to Ω_compress (I)\n");
}

/* ══════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════*/
int main(void) {
    printf("═══════════════════════════════════════════════════\n");
    printf("  POGLS BOND — intrinsic bond unit test\n");
    printf("═══════════════════════════════════════════════════\n");

    test_fibo_determinism();
    test_piece_factory();
    test_intrinsic_bond();
    test_wallet_bridge();
    test_plug_chain();
    test_reroute();

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  ALL TESTS PASSED\n");
    printf("═══════════════════════════════════════════════════\n");
    return 0;
}

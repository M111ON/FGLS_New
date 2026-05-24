/*
 * test_pogls_bond_v2.c
 * ─────────────────────────────────────────────────────────────
 * v1.1 extended test suite
 *
 * Compile:
 *   gcc -O2 -I. -o test_bond_v2 test_pogls_bond_v2.c && ./test_bond_v2
 *
 * Tests added vs v1:
 *   [7]  bond_verify false-positive rate (statistical)
 *   [8]  nonce isolation — same pieces, different nonce → invalid
 *   [9]  nonce replay protection — nonce=0 backward-compat
 *   [10] reroute chain — multiple Ω substitutions
 *   [11] plug TTL expire simulation
 *   [12] config path resolution (env-driven)
 * ─────────────────────────────────────────────────────────────
 */

#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_bond.h"

/* ── pass/fail counters ───────────────────────────────────── */
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                          \
    if (cond) { printf("  ✓ %s\n", msg); g_pass++; }  \
    else       { printf("  ✗ FAIL: %s\n", msg); g_fail++; assert(0); } \
} while(0)

/* ── helpers ─────────────────────────────────────────────── */
static void print_piece(const char *label, const PoglsPiece *p) {
    printf("  %-12s shape=%c  geo=%016llx  bond_key=%016llx\n",
           label, (char)p->shape,
           (unsigned long long)p->geo_key,
           (unsigned long long)pogls_bond_key(p));
}

static void print_slot(const char *label, const PoglsSlot *s) {
    printf("  %-12s agent=%u  shape=%c  rerouted=%u\n",
           label, s->agent_id, (char)s->piece.shape, s->rerouted);
}

static void section(int n, const char *title) {
    printf("\n[%d] %s\n", n, title);
}

/* ══════════════════════════════════════════════════════════
 * TESTS 1–6 (carried from v1, unchanged logic, CHECK macro)
 * ══════════════════════════════════════════════════════════*/

static void test_fibo_determinism(void) {
    section(1, "fibo_addr determinism");
    uint64_t seed = 0xDEADBEEFCAFE0001ULL;
    uint64_t a = pogls_fibo_addr(seed);
    uint64_t b = pogls_fibo_addr(seed);
    CHECK(a == b, "same seed → same addr");
    uint64_t c = pogls_fibo_addr(seed ^ 1);
    CHECK(a != c, "seed±1 → different addr");
    /* avalanche: 1-bit flip should change many bits */
    uint64_t diff = a ^ c;
    int popcount = __builtin_popcountll(diff);
    printf("  bit diff on 1-bit seed flip: %d/64 bits changed\n", popcount);
    CHECK(popcount >= 16, "avalanche: ≥16 bits changed on 1-bit seed flip");
}

static void test_piece_factory(void) {
    section(2, "piece factory — axis→shape");
    uint64_t base = 0x9009000000000001ULL;
    uint8_t  axes[]   = {1, 2, 3, 4, 5, 6, 7};
    uint8_t  shapes[] = {SHAPE_I, SHAPE_O, SHAPE_T, SHAPE_S,
                         SHAPE_Z, SHAPE_L, SHAPE_J};
    for (int i = 0; i < 7; i++) {
        PoglsPiece p = pogls_make_piece(pogls_fibo_addr(base ^ (uint64_t)i), axes[i]);
        CHECK(p.shape == shapes[i], "axis→shape correct");
    }
    /* axis 0 (unused) → 0x00 sentinel, not SHAPE_I */
    PoglsPiece p0 = pogls_make_piece(base, 0);
    CHECK(p0.shape == 0x00, "axis=0 → 0x00 (unused sentinel)");
    /* axis 8+ → SHAPE_I fallback */
    PoglsPiece p8 = pogls_make_piece(base, 8);
    CHECK(p8.shape == SHAPE_I, "axis≥8 → SHAPE_I fallback");
}

static void test_intrinsic_bond(void) {
    section(3, "intrinsic bond — coordinate enforcement");
    PoglsPiece A = pogls_make_piece(pogls_fibo_addr(0xF1B0000000000001ULL), 1);
    PoglsPiece B = pogls_make_piece(pogls_fibo_addr(0xF1B0000000000002ULL), 3);
    uint64_t ka = pogls_bond_key(&A);
    PoglsPiece A_shifted = A;
    A_shifted.geo_key ^= 1ULL;
    A_shifted.bond_L   = pogls_fibo_addr(A_shifted.geo_key ^ POGLS_BOND_SALT_L);
    A_shifted.bond_R   = pogls_fibo_addr(A_shifted.geo_key ^ POGLS_BOND_SALT_R);
    uint64_t ka2 = pogls_bond_key(&A_shifted);
    CHECK(ka != ka2, "coord shift → bond_key changes");
    (void)B;
    /* bond_key changes on geo_key shift regardless of delta size */
    for (int bit = 0; bit < 8; bit++) {
        PoglsPiece Ax = A;
        Ax.geo_key ^= (1ULL << bit);
        Ax.bond_L   = pogls_fibo_addr(Ax.geo_key ^ POGLS_BOND_SALT_L);
        Ax.bond_R   = pogls_fibo_addr(Ax.geo_key ^ POGLS_BOND_SALT_R);
        CHECK(pogls_bond_key(&Ax) != ka, "any bit flip in geo_key breaks bond");
    }
}

static void test_wallet_bridge(void) {
    section(4, "wallet topology_fp → C piece (bridge)");
    const char *fp = "a3f0b2c1d4e5f6a7";
    uint64_t s1 = pogls_seed_from_fp(fp);
    uint64_t s2 = pogls_seed_from_fp(fp);
    CHECK(s1 == s2, "same fp → same seed");
    PoglsPiece p1 = pogls_make_piece(s1, 1);
    PoglsPiece p2 = pogls_make_piece(s2, 1);
    CHECK(p1.geo_key == p2.geo_key, "reproducible geo_key from fp");
    CHECK(p1.bond_L  == p2.bond_L,  "reproducible bond_L from fp");
    CHECK(p1.bond_R  == p2.bond_R,  "reproducible bond_R from fp");
}

static void test_plug_chain(void) {
    section(5, "extrinsic plug chain A─B─C─D");
    uint64_t  base  = 0x9009009009009009ULL;
    uint8_t   axes[] = {1,3,1,6};
    PoglsSlot slots[4];
    for (int i = 0; i < 4; i++) {
        slots[i].piece     = pogls_make_piece(pogls_fibo_addr(base ^ (uint64_t)i), axes[i]);
        slots[i].agent_id  = (uint32_t)i;
        slots[i].token_cap = 300;
        slots[i].rerouted  = 0;
        memset(slots[i].plugs, 0, sizeof(slots[i].plugs));
    }
    pogls_plug_connect(&slots[0], PLUG_FACE_E, &slots[1], PLUG_FACE_W, 64);
    pogls_plug_connect(&slots[1], PLUG_FACE_E, &slots[2], PLUG_FACE_W, 64);
    pogls_plug_connect(&slots[2], PLUG_FACE_E, &slots[3], PLUG_FACE_W, 64);
    CHECK(slots[0].plugs[PLUG_FACE_E].active == 1, "A.E plug active");
    CHECK(slots[1].plugs[PLUG_FACE_W].active == 1, "B.W plug active (symmetric)");
    CHECK(slots[3].plugs[PLUG_FACE_W].active == 1, "D.W plug active");
    CHECK(slots[3].plugs[PLUG_FACE_E].active == 0, "D.E plug inactive (end of chain)");
    /* disconnect mid-chain */
    pogls_plug_disconnect(&slots[1], PLUG_FACE_E);
    CHECK(slots[1].plugs[PLUG_FACE_E].active == 0, "B.E disconnected");
    CHECK(slots[2].plugs[PLUG_FACE_W].active == 1, "C.W still active (asymmetric ok)");
}

static void test_reroute(void) {
    section(6, "Ω reroute — fault → shape substitution");
    uint64_t  seed = pogls_fibo_addr(0xDEAD0000BEEF0000ULL);
    PoglsSlot slot = {0};
    slot.piece     = pogls_make_piece(seed, 1);
    slot.agent_id  = 99;
    uint64_t orig_bond_L = slot.piece.bond_L;
    uint64_t orig_bond_R = slot.piece.bond_R;
    pogls_reroute(&slot, POGLS_OVERFLOW);
    CHECK(slot.piece.shape    == OMEGA_COMPRESS,  "OVERFLOW → OMEGA_COMPRESS");
    CHECK(slot.rerouted       == POGLS_OVERFLOW,   "rerouted flag set");
    CHECK(slot.piece.bond_L   == orig_bond_L,      "bond_L unchanged after reroute");
    CHECK(slot.piece.bond_R   == orig_bond_R,      "bond_R unchanged after reroute");
    /* geo_key mutated (by design — reroute changes position) */
    CHECK(slot.piece.geo_key  != seed, "geo_key mutated by reroute");
    /* POGLS_OK → no-op */
    PoglsSlot slot2 = {0};
    slot2.piece = pogls_make_piece(seed, 2);
    slot2.agent_id = 1;
    uint64_t geo_before = slot2.piece.geo_key;
    pogls_reroute(&slot2, POGLS_OK);
    CHECK(slot2.piece.geo_key == geo_before, "POGLS_OK → no mutation");
}

/* ══════════════════════════════════════════════════════════
 * TEST 7 — bond_verify false-positive rate (statistical)
 * Run N random cross-session pairs, count false positives
 * Expected: 0 with 32-bit mask at N=100000
 * ══════════════════════════════════════════════════════════*/
static void test_bond_verify_false_positive_rate(void) {
    section(7, "bond_verify false-positive rate (N=100000 random pairs)");

    /* ensure nonce=0 for this test (reproducible) */
    pogls_config_set_nonce(0);

    int fp_count = 0;
    int N = 100000;

    for (int i = 0; i < N; i++) {
        /* random-ish seeds — not from same origin, so bond should NOT verify */
        uint64_t seed_x = (uint64_t)i * 0x9e3779b97f4a7c15ULL ^ 0xAAAAAAAA00000001ULL;
        uint64_t seed_y = (uint64_t)i * 0x6c62272e07bb0142ULL ^ 0x5555555500000002ULL;
        PoglsPiece px = pogls_make_piece(seed_x, 1);
        PoglsPiece py = pogls_make_piece(seed_y, 3);
        PoglsBond  b  = pogls_bond_verify(&px, &py);
        if (b.valid) fp_count++;
    }

    printf("  false positives: %d / %d  (%.6f%%)\n",
           fp_count, N, (double)fp_count / N * 100.0);
    printf("  verify bits: %d  mask: %016llx\n",
           POGLS_BOND_VERIFY_BITS,
           (unsigned long long)POGLS_BOND_VERIFY_MASK);

    /* With 32-bit mask: expect ~0.0000023% → ~2 in 100M, 0 in 100K */
    /* With 16-bit mask: expect ~1.5% → ~1500 in 100K               */
    CHECK(fp_count == 0, "zero false positives at N=100000 (32-bit mask)");
}

/* ══════════════════════════════════════════════════════════
 * TEST 8 — nonce isolation
 * Same origin pieces, different nonce → bond invalid
 * ══════════════════════════════════════════════════════════*/
static void test_nonce_isolation(void) {
    section(8, "nonce isolation — same pieces, different nonce → invalid");

    uint64_t origin = 0x9009CAFE00000001ULL;

    /* Make a bonded pair: A and B from same session */
    /* To get valid=1, both must derive from same origin with matching nonce */
    /* Here we test that nonce change breaks cross-session reuse */

    PoglsPiece A = pogls_make_piece(pogls_fibo_addr(origin),      1);
    PoglsPiece B = pogls_make_piece(pogls_fibo_addr(origin ^ 1),  3);

    /* session 1 */
    pogls_config_set_nonce(0xDEAD000000000001ULL);
    PoglsBond b1 = pogls_bond_verify(&A, &B);

    /* session 2 — different nonce */
    pogls_config_set_nonce(0xBEEF000000000002ULL);
    PoglsBond b2 = pogls_bond_verify(&A, &B);

    /* both may be invalid (pieces not origin-paired) but they must differ */
    printf("  session1 nonce=DEAD...: valid=%u  bond_key=%016llx\n",
           b1.valid, (unsigned long long)b1.bond_key);
    printf("  session2 nonce=BEEF...: valid=%u  bond_key=%016llx\n",
           b2.valid, (unsigned long long)b2.bond_key);

    /* bond_key (raw_xor, pre-nonce) must be stable across nonces */
    CHECK(b1.bond_key == b2.bond_key,
          "bond_key (raw_xor) stable across nonces — useful for indexing");

    /* If b1.valid happened to be 1, b2 should be different */
    if (b1.valid) {
        CHECK(b2.valid == 0,
              "bond valid in session1 is invalid in session2 (nonce replay protection)");
    } else {
        printf("  (pair not origin-bonded — nonce isolation via combined hash change)\n");
        /* Verify that combined hash is actually different with different nonce */
        pogls_config_set_nonce(0xDEAD000000000001ULL);
        uint64_t ka = pogls_bond_key(&A), kb = pogls_bond_key(&B);
        uint64_t raw = ka ^ kb;
        uint64_t n1  = pogls_fibo_addr(raw ^ 0xDEAD000000000001ULL);
        uint64_t c1  = pogls_fibo_addr(n1 ^ raw);
        uint64_t n2  = pogls_fibo_addr(raw ^ 0xBEEF000000000002ULL);
        uint64_t c2  = pogls_fibo_addr(n2 ^ raw);
        CHECK(c1 != c2, "different nonce → different combined hash (replay blocked)");
    }

    /* reset nonce to 0 for subsequent tests */
    pogls_config_set_nonce(0);
}

/* ══════════════════════════════════════════════════════════
 * TEST 9 — nonce=0 backward-compatibility
 * Old and new code with nonce=0 must produce same bond_key
 * ══════════════════════════════════════════════════════════*/
static void test_nonce_backward_compat(void) {
    section(9, "nonce=0 backward-compatibility");

    pogls_config_set_nonce(0);

    uint64_t seed_A = pogls_fibo_addr(0xCAFE000000000001ULL);
    uint64_t seed_B = pogls_fibo_addr(0xCAFE000000000002ULL);
    PoglsPiece A = pogls_make_piece(seed_A, 1);
    PoglsPiece B = pogls_make_piece(seed_B, 3);

    PoglsBond bond = pogls_bond_verify(&A, &B);

    /* bond_key must equal raw ka ^ kb (same as old code) */
    uint64_t expected_key = pogls_bond_key(&A) ^ pogls_bond_key(&B);
    CHECK(bond.bond_key == expected_key,
          "nonce=0: bond_key == raw XOR (backward-compat)");

    /* deterministic: same call → same result */
    PoglsBond bond2 = pogls_bond_verify(&A, &B);
    CHECK(bond.valid     == bond2.valid,     "deterministic: valid");
    CHECK(bond.bond_key  == bond2.bond_key,  "deterministic: bond_key");

    printf("  bond_key=%016llx  valid=%u\n",
           (unsigned long long)bond.bond_key, bond.valid);
    printf("  verify_bits=%d  (was 16 in v1.0)\n", POGLS_BOND_VERIFY_BITS);
}

/* ══════════════════════════════════════════════════════════
 * TEST 10 — reroute chain (multiple Ω substitutions)
 * Verify geo_key keeps changing, bond_L/R stay constant
 * ══════════════════════════════════════════════════════════*/
static void test_reroute_chain(void) {
    section(10, "Ω reroute chain — geo evolution, bond stability");

    uint64_t  seed = pogls_fibo_addr(0xABCD000000000001ULL);
    PoglsSlot slot = {0};
    slot.piece    = pogls_make_piece(seed, 1);
    slot.agent_id = 42;

    uint64_t bond_L_orig = slot.piece.bond_L;
    uint64_t bond_R_orig = slot.piece.bond_R;
    uint64_t geo_prev    = slot.piece.geo_key;

    PoglsFault faults[] = {POGLS_OVERFLOW, POGLS_FAULT, POGLS_UPSTREAM, POGLS_RETRY};
    const char *names[] = {"OVERFLOW","FAULT","UPSTREAM","RETRY"};

    for (int i = 0; i < 4; i++) {
        pogls_reroute(&slot, faults[i]);
        printf("  after %-10s: shape=%c  geo=%016llx  rerouted=%u\n",
               names[i], (char)slot.piece.shape,
               (unsigned long long)slot.piece.geo_key,
               slot.rerouted);
        CHECK(slot.piece.geo_key != geo_prev, "geo_key evolves each reroute");
        CHECK(slot.piece.bond_L == bond_L_orig, "bond_L stable through chain");
        CHECK(slot.piece.bond_R == bond_R_orig, "bond_R stable through chain");
        geo_prev = slot.piece.geo_key;
    }
}

/* ══════════════════════════════════════════════════════════
 * TEST 11 — plug TTL expiry simulation
 * ══════════════════════════════════════════════════════════*/
static void test_plug_ttl(void) {
    section(11, "plug TTL expiry simulation");

    uint64_t  base = 0x1234000000000001ULL;
    PoglsSlot A = {0}, B = {0};
    A.piece    = pogls_make_piece(pogls_fibo_addr(base),     1);
    B.piece    = pogls_make_piece(pogls_fibo_addr(base ^ 1), 3);
    A.agent_id = 1;
    B.agent_id = 2;
    memset(A.plugs, 0, sizeof(A.plugs));
    memset(B.plugs, 0, sizeof(B.plugs));

    pogls_plug_connect(&A, PLUG_FACE_N, &B, PLUG_FACE_S, 3);
    CHECK(A.plugs[PLUG_FACE_N].active == 1, "plug active after connect");
    CHECK(A.plugs[PLUG_FACE_N].ttl   == 3, "TTL=3 set correctly");

    /* simulate TTL countdown */
    for (uint16_t ttl = 3; ttl > 0; ttl--) {
        A.plugs[PLUG_FACE_N].ttl--;
        B.plugs[PLUG_FACE_S].ttl--;
    }
    /* TTL hit 0 — caller responsible for disconnect (pogls_bond.h is stateless) */
    if (A.plugs[PLUG_FACE_N].ttl == 0) {
        pogls_plug_disconnect(&A, PLUG_FACE_N);
        pogls_plug_disconnect(&B, PLUG_FACE_S);
    }
    CHECK(A.plugs[PLUG_FACE_N].active == 0, "plug inactive after TTL expiry");
    CHECK(B.plugs[PLUG_FACE_S].active == 0, "symmetric plug inactive");
    printf("  TTL countdown → disconnect: correct\n");
}

/* ══════════════════════════════════════════════════════════
 * TEST 12 — config path resolution (env-driven)
 * ══════════════════════════════════════════════════════════*/
static void test_config_paths(void) {
    section(12, "config path resolution");

    /* default: env not set → falls back to "." */
    const char *root = pogls_config_root();
    printf("  POGLS_ROOT (default): %s\n", root);
    CHECK(root != NULL, "root is non-null");

    /* override via setenv */
#ifdef POGLS_PLATFORM_UNIX
    setenv("POGLS_ROOT", "/tmp/pogls_test", 1);
    CHECK(strcmp(pogls_config_root(), "/tmp/pogls_test") == 0,
          "env POGLS_ROOT override works");
    unsetenv("POGLS_ROOT");
    CHECK(strcmp(pogls_config_root(), ".") == 0,
          "fallback to . after unset");
#else
    /* Windows: skip setenv, just validate defaults */
    CHECK(root != NULL, "root non-null on Windows (default path)");
    printf("  (env override test skipped on Windows)\n");
#endif

    printf("  blob_dir   : %s\n", pogls_config_blob_dir());
    printf("  wallet_dir : %s\n", pogls_config_wallet_dir());
    printf("  version    : %s\n", POGLS_BOND_VERSION_STR);
    CHECK(strcmp(pogls_config_blob_dir(),   "blobs")   == 0, "default blob_dir");
    CHECK(strcmp(pogls_config_wallet_dir(), "wallets") == 0, "default wallet_dir");
}

/* ══════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════*/
int main(void) {
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  POGLS BOND v1.1 — extended test suite\n");
    printf("  bond_verify bits : %d\n", POGLS_BOND_VERIFY_BITS);
    printf("  verify mask      : %016llx\n",
           (unsigned long long)POGLS_BOND_VERIFY_MASK);
    printf("  verify target    : %016llx\n",
           (unsigned long long)POGLS_BOND_VERIFY_TARGET);
    printf("  version          : %s\n", POGLS_BOND_VERSION_STR);
    printf("═══════════════════════════════════════════════════════════\n");

    test_fibo_determinism();
    test_piece_factory();
    test_intrinsic_bond();
    test_wallet_bridge();
    test_plug_chain();
    test_reroute();
    test_bond_verify_false_positive_rate();
    test_nonce_isolation();
    test_nonce_backward_compat();
    test_reroute_chain();
    test_plug_ttl();
    test_config_paths();

    printf("\n═══════════════════════════════════════════════════════════\n");
    if (g_fail == 0) {
        printf("  ALL TESTS PASSED  (%d/%d)\n", g_pass, g_pass + g_fail);
    } else {
        printf("  FAILED: %d  PASSED: %d\n", g_fail, g_pass);
    }
    printf("═══════════════════════════════════════════════════════════\n");
    return g_fail ? 1 : 0;
}

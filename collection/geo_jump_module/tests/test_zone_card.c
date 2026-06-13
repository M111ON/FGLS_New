/*
 * test_zone_card.c — Test suite for zone_card.h (S3 unified)
 * Compile: gcc -O2 -lm -o test_zone_card test_zone_card.c && ./test_zone_card
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "zone_card.h"

static int pass = 0, fail = 0;
#define CHECK(label, cond) \
    do { if (cond) { printf("  PASS  %s\n", label); pass++; } \
         else      { printf("  FAIL  %s\n", label); fail++; } } while(0)

/* ── T1: struct sizes ────────────────────────────────── */
static void test_sizes(void) {
    printf("\n[T1] struct sizes\n");
    CHECK("ZoneCard = 12 bytes",    sizeof(ZoneCard)    == 12u);
    CHECK("ZoneCardExt = 20 bytes", sizeof(ZoneCardExt) == 20u);
}

/* ── T2: local path ──────────────────────────────────── */
static void test_local(void) {
    printf("\n[T2] local path (float* weights)\n");

    /* sparse weight block: mostly zeros */
    float sparse[64] = {0};
    sparse[3] = 0.9f; sparse[17] = -0.1f;
    ZoneCard cs = zone_card_make(sparse, 64, 1, NO_NEIGHBOR, 2);
    CHECK("sparse weights → CARD_SPARSE", cs.card_type == CARD_SPARSE);
    CHECK("sparse entropy low (<100)",    cs.entropy < 100);

    /* dense random-ish weights */
    float dense[64];
    for (int i = 0; i < 64; i++) dense[i] = (float)(i % 7) * 0.13f - 0.45f;
    ZoneCard cd = zone_card_make(dense, 64, 2, 1, 3);
    CHECK("dense weights → BATCH or LZ",
          cd.card_type == CARD_BATCH || cd.card_type == CARD_LZ);
    CHECK("dense entropy > sparse", cd.entropy > cs.entropy);

    /* deterministic */
    ZoneCard cd2 = zone_card_make(dense, 64, 2, 1, 3);
    CHECK("local deterministic", cd.entropy == cd2.entropy &&
                                  cd.card_type == cd2.card_type);
}

/* ── T3: cloud path ──────────────────────────────────── */
static void test_cloud(void) {
    printf("\n[T3] cloud path (logprobs[])\n");

    /* certain model: top token dominates */
    float certain[5] = {
        logf(0.92f), logf(0.04f), logf(0.02f), logf(0.01f), logf(0.01f)
    };
    ZoneCard cc = zone_card_from_logprobs(certain, 5, 10, NO_NEIGHBOR, 11);
    CHECK("certain model → CARD_SPARSE",   cc.card_type == CARD_SPARSE);
    CHECK("certain model → low entropy",   cc.entropy < 80);
    CHECK("certain model → high stability", cc.stability > 180);

    /* uncertain model: spread across tokens */
    float uncertain[8];
    for (int i = 0; i < 8; i++) uncertain[i] = logf(1.0f / 8.0f);
    ZoneCard cu = zone_card_from_logprobs(uncertain, 8, 11, 10, 12);
    CHECK("uncertain model → CARD_LZ",       cu.card_type == CARD_LZ);
    CHECK("uncertain model → high entropy",  cu.entropy > 180);
    CHECK("uncertain model → low stability", cu.stability < 80);

    /* entropy ordering: certain < uncertain */
    CHECK("certain entropy < uncertain entropy", cc.entropy < cu.entropy);
}

/* ── T4: cloud deterministic ─────────────────────────── */
static void test_cloud_deterministic(void) {
    printf("\n[T4] cloud path deterministic\n");
    float lp[4] = { logf(0.6f), logf(0.2f), logf(0.15f), logf(0.05f) };
    ZoneCard a = zone_card_from_logprobs(lp, 4, 5, NO_NEIGHBOR, 6);
    ZoneCard b = zone_card_from_logprobs(lp, 4, 5, NO_NEIGHBOR, 6);
    CHECK("same logprobs → same entropy",   a.entropy   == b.entropy);
    CHECK("same logprobs → same card_type", a.card_type == b.card_type);
    CHECK("same logprobs → same stability", a.stability == b.stability);
    CHECK("same logprobs → same pattern",   a.pattern   == b.pattern);
}

/* ── T5: same shell check ────────────────────────────── */
static void test_same_shell(void) {
    printf("\n[T5] zone_card_same_shell\n");
    ZoneCard a = {.entropy = 50};
    ZoneCard b = {.entropy = 55};   /* same band */
    ZoneCard c = {.entropy = 200};  /* different band */
    CHECK("entropy 50 vs 55 → same shell",  zone_card_same_shell(&a, &b));
    CHECK("entropy 50 vs 200 → diff shell", !zone_card_same_shell(&a, &c));
}

/* ── T6: local vs cloud consistency ─────────────────── */
static void test_cross_path(void) {
    printf("\n[T6] local vs cloud cross-path consistency\n");

    /* local: sparse stable weights → should agree with cloud: certain model */
    float w[32] = {0}; w[0] = 1.0f;  /* very sparse */
    ZoneCard local = zone_card_make(w, 32, 20, NO_NEIGHBOR, 21);

    float lp[5] = { logf(0.93f), logf(0.03f), logf(0.02f),
                    logf(0.01f), logf(0.01f) };
    ZoneCard cloud = zone_card_from_logprobs(lp, 5, 20, NO_NEIGHBOR, 21);

    /* both should map to inner shells (low entropy) */
    CHECK("local sparse → low entropy",  local.entropy < 100);
    CHECK("cloud certain → low entropy", cloud.entropy < 100);
    CHECK("both consistent",             zone_card_src_consistent(&local, &cloud));
}

/* ── T7: diff bitmask ────────────────────────────────── */
static void test_diff(void) {
    printf("\n[T7] zone_card_diff\n");
    ZoneCard a = {.card_type=CARD_SPARSE, .entropy=10, .stability=250, .pattern=0x0100};
    ZoneCard b = {.card_type=CARD_SPARSE, .entropy=10, .stability=250, .pattern=0x0100};
    CHECK("identical cards → diff 0", zone_card_diff(&a, &b) == 0);

    ZoneCard c = {.card_type=CARD_LZ, .entropy=200, .stability=30, .pattern=0xFF00};
    uint8_t d = zone_card_diff(&a, &c);
    CHECK("different cards → diff != 0", d != 0);
    CHECK("card_type bit set",           d & 0x01);
    CHECK("entropy band bit set",        d & 0x02);
}

/* ── T8: ZoneCardExt hash ────────────────────────────── */
static void test_ext(void) {
    printf("\n[T8] ZoneCardExt (with hash)\n");

    float w[16]; for (int i=0;i<16;i++) w[i]=(float)i*0.1f;
    ZoneCardExt ex = zone_card_make_ext(w, 16, 99, 98, NO_NEIGHBOR);
    CHECK("ext hash != 0", ex.hash_val != 0u);
    CHECK("ext base fields match make()",
          zone_card_make(w, 16, 99, 98, NO_NEIGHBOR).entropy == ex.entropy);

    float lp[3] = { logf(0.5f), logf(0.3f), logf(0.2f) };
    ZoneCardExt ec = zone_card_from_logprobs_ext(lp, 3, 50, NO_NEIGHBOR, 51);
    CHECK("cloud ext hash != 0", ec.hash_val != 0u);
    CHECK("cloud ext base consistent",
          zone_card_from_logprobs(lp, 3, 50, NO_NEIGHBOR, 51).entropy == ec.entropy);
}

/* ── main ─────────────────────────────────────────────── */
int main(void) {
    printf("════════════════════════════════════════════\n");
    printf(" POGLS zone_card.h unified — Test Suite S3  \n");
    printf("════════════════════════════════════════════\n");

    test_sizes();
    test_local();
    test_cloud();
    test_cloud_deterministic();
    test_same_shell();
    test_cross_path();
    test_diff();
    test_ext();

    printf("\n════════════════════════════════════════════\n");
    printf(" Result: %d PASS / %d FAIL\n", pass, fail);
    printf("════════════════════════════════════════════\n");
    return (fail == 0) ? 0 : 1;
}

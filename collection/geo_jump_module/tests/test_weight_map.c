/*
 * test_weight_map.c — Test suite for shell_weight_map.h (S3)
 * Compile: gcc -O2 -lm -o test_weight_map test_weight_map.c && ./test_weight_map
 */

#include <stdio.h>
#include <stdint.h>
#include "shell_weight_map.h"

static int pass = 0, fail = 0;
#define CHECK(label, cond) \
    do { if (cond) { printf("  PASS  %s\n", label); pass++; } \
         else      { printf("  FAIL  %s\n", label); fail++; } } while(0)

static OnionStack G;

static void setup(void) {
    onion_init(&G, 0xC0FFEE42u, 1u);
}

/* ── T1: entropy → shell mapping ────────────────────── */
static void test_entropy_shell(void) {
    printf("\n[T1] entropy → shell_id\n");
    CHECK("entropy 0   → shell 0",  wmap_entropy_to_shell(0)   == 0u);
    CHECK("entropy 255 → shell 11", wmap_entropy_to_shell(255) == 11u);
    CHECK("entropy 128 → shell 6",  wmap_entropy_to_shell(128) == 6u);
    CHECK("entropy 22  → shell 1",  wmap_entropy_to_shell(22)  == 1u);
    /* monotonic */
    uint8_t prev = 0;
    int mono = 1;
    for (int e = 0; e <= 255; e++) {
        uint8_t s = wmap_entropy_to_shell((uint8_t)e);
        if (s < prev) { mono = 0; break; }
        prev = s;
    }
    CHECK("entropy→shell monotonic", mono);
}

/* ── T2: card_type → chord_id ───────────────────────── */
static void test_type_chord(void) {
    printf("\n[T2] card_type → chord_id\n");
    CHECK("SPARSE → HUB",     wmap_type_to_chord(CARD_SPARSE) == CHORD_HUB);
    CHECK("BATCH  → ORBITAL", wmap_type_to_chord(CARD_BATCH)  == CHORD_ORBITAL);
    CHECK("LZ     → CHIRAL",  wmap_type_to_chord(CARD_LZ)     == CHORD_CHIRAL);
}

/* ── T3: stability → capo ───────────────────────────── */
static void test_stability_capo(void) {
    printf("\n[T3] stability → capo\n");
    CHECK("stability 255 → capo 0",   wmap_stability_to_capo(255) == 0u);
    CHECK("stability 0   → capo max", wmap_stability_to_capo(0)   == 575u);
    CHECK("stability 128 → capo mid", wmap_stability_to_capo(128) > 0u &&
                                      wmap_stability_to_capo(128) < 576u);
}

/* ── T4: ZoneCard → WeightAddr deterministic ────────── */
static void test_deterministic(void) {
    printf("\n[T4] shell_weight_addr deterministic\n");
    ZoneCard card = {
        .id=1, .card_type=CARD_BATCH, .entropy=80,
        .pattern=0x1234, .locality=128, .stability=200,
        .neighbor_left=0, .neighbor_right=2
    };
    WeightAddr a1 = shell_weight_addr(&G, &card, 5u);
    WeightAddr a2 = shell_weight_addr(&G, &card, 5u);
    CHECK("same card+layer → same addr", a1.addr == a2.addr);
    CHECK("addr in valid range", a1.addr < SHELL_JUNCTION);
    CHECK("shell_id in 0..11", a1.shell_id < ONION_N_SHELLS);

    /* different layer → different addr */
    WeightAddr b = shell_weight_addr(&G, &card, 10u);
    CHECK("different layer → different addr", a1.addr != b.addr);
}

/* ── T5: card types → different shells/chords ───────── */
static void test_card_variety(void) {
    printf("\n[T5] card variety → different placements\n");

    /* sparse (low entropy, stable) = inner shell */
    ZoneCard sparse = {.card_type=CARD_SPARSE, .entropy=10,
                       .pattern=0x0100, .stability=250,
                       .neighbor_left=NO_NEIGHBOR, .neighbor_right=NO_NEIGHBOR};
    /* dense (high entropy, unstable) = outer shell */
    ZoneCard dense  = {.card_type=CARD_LZ, .entropy=240,
                       .pattern=0xFF00, .stability=10,
                       .neighbor_left=NO_NEIGHBOR, .neighbor_right=NO_NEIGHBOR};

    WeightAddr wa_sparse = shell_weight_addr(&G, &sparse, 0u);
    WeightAddr wa_dense  = shell_weight_addr(&G, &dense,  0u);

    CHECK("sparse → inner shell (0..2)",  wa_sparse.shell_id <= 2u);
    CHECK("dense  → outer shell (9..11)", wa_dense.shell_id  >= 9u);
    CHECK("sparse → CHORD_HUB",           wa_sparse.chord_id == CHORD_HUB);
    CHECK("dense  → CHORD_CHIRAL",        wa_dense.chord_id  == CHORD_CHIRAL);
    CHECK("sparse capo near 0",           wa_sparse.capo < 20u);
    CHECK("dense  capo near max",         wa_dense.capo  > 550u);
}

/* ── T6: round-trip verify ───────────────────────────── */
static void test_roundtrip(void) {
    printf("\n[T6] round-trip verify\n");
    ZoneCard cards[3] = {
        {.card_type=CARD_SPARSE, .entropy=5,   .pattern=0x0200,
         .stability=255, .neighbor_left=NO_NEIGHBOR, .neighbor_right=1},
        {.card_type=CARD_BATCH,  .entropy=128, .pattern=0x8800,
         .stability=128, .neighbor_left=0, .neighbor_right=2},
        {.card_type=CARD_LZ,     .entropy=250, .pattern=0xFF80,
         .stability=10,  .neighbor_left=1, .neighbor_right=NO_NEIGHBOR},
    };
    for (int i = 0; i < 3; i++) {
        int ok = shell_weight_verify(&G, &cards[i], (uint32_t)i * 3u);
        char label[64];
        snprintf(label, sizeof(label), "card[%d] round-trip shell+chord consistent", i);
        CHECK(label, ok);
    }
}

/* ── T7: batch layer map ─────────────────────────────── */
static void test_batch(void) {
    printf("\n[T7] shell_weight_map_layer (batch)\n");
    ZoneCard layer[4] = {
        {.card_type=CARD_SPARSE, .entropy=20,  .pattern=0x0100, .stability=240,
         .neighbor_left=NO_NEIGHBOR, .neighbor_right=1},
        {.card_type=CARD_BATCH,  .entropy=100, .pattern=0x5500, .stability=180,
         .neighbor_left=0, .neighbor_right=2},
        {.card_type=CARD_LZ,     .entropy=180, .pattern=0xAA00, .stability=80,
         .neighbor_left=1, .neighbor_right=3},
        {.card_type=CARD_BATCH,  .entropy=230, .pattern=0xFF00, .stability=30,
         .neighbor_left=2, .neighbor_right=NO_NEIGHBOR},
    };
    WeightAddr out[4];
    shell_weight_map_layer(&G, layer, 4u, 7u, out);

    int all_valid = 1;
    int all_unique = 1;
    for (int i = 0; i < 4; i++) {
        if (out[i].addr >= SHELL_JUNCTION) { all_valid = 0; }
        for (int j = i+1; j < 4; j++)
            if (out[i].addr == out[j].addr) all_unique = 0;
    }
    CHECK("all addrs in valid range", all_valid);
    CHECK("all addrs unique", all_unique);

    /* monotonic shell (entropy increases) */
    CHECK("shell[0] <= shell[1]", out[0].shell_id <= out[1].shell_id);
    CHECK("shell[1] <= shell[2]", out[1].shell_id <= out[2].shell_id);
    CHECK("shell[2] <= shell[3]", out[2].shell_id <= out[3].shell_id);
}

/* ── T8: ZoneCard 12-byte passport handoff ───────────── */
static void test_passport(void) {
    printf("\n[T8] ZoneCard as 12-byte passport\n");
    CHECK("ZoneCard size = 12 bytes", sizeof(ZoneCard) == 12u);

    /* simulate cross-session: session A makes card, session B resolves */
    ZoneCard passport = {
        .id=42, .card_type=CARD_LZ, .entropy=160,
        .pattern=0xBEEF, .locality=200, .stability=100,
        .neighbor_left=41, .neighbor_right=43
    };

    /* session A */
    WeightAddr wa_a = shell_weight_addr(&G, &passport, 12u);

    /* session B (fresh onion, same seed) */
    OnionStack o2;
    onion_init(&o2, 0xC0FFEE42u, 1u);
    WeightAddr wa_b = shell_weight_addr(&o2, &passport, 12u);

    CHECK("cross-session: same passport → same addr", wa_a.addr == wa_b.addr);
    CHECK("cross-session: same shell_id",             wa_a.shell_id == wa_b.shell_id);
}

/* ── main ─────────────────────────────────────────────── */
int main(void) {
    printf("══════════════════════════════════════════\n");
    printf(" POGLS shell_weight_map.h — Test Suite S3 \n");
    printf("══════════════════════════════════════════\n");

    setup();
    test_entropy_shell();
    test_type_chord();
    test_stability_capo();
    test_deterministic();
    test_card_variety();
    test_roundtrip();
    test_batch();
    test_passport();

    printf("\n══════════════════════════════════════════\n");
    printf(" Result: %d PASS / %d FAIL\n", pass, fail);
    printf("══════════════════════════════════════════\n");
    return (fail == 0) ? 0 : 1;
}

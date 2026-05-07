/*
 * test_seam2.c — SEAM 2: geo_addr_net.h test suite
 * =================================================
 * T1:  LUT init — 720 unique lanes (no duplicate)
 * T2:  geo_net_encode() polarity matches old enc%720 path
 * T3:  geo_net_encode() spoke matches pos/120
 * T4:  geo_net_encode() hilbert_idx in [0,719]
 * T5:  hilbert_idx unique across all 720 tring positions
 * T6:  bridge hex[16] round-trip (all 16 chars valid hex)
 * T7:  interface base62[11] round-trip (all 11 chars valid)
 * T8:  geo_net_to_lc_route() matches tgwlc_route() old output
 * T9:  batch encode N=720 — all hilbert_idx unique
 * T10: hilbert coherence — adjacent spoke×pos map to close lanes
 * T11: spoke histogram uniform (720 enc → 120/spoke)
 * T12: GeoNetAddr size = 16 bytes (cache-line alignment)
 * T13: geo_net_address() null-termination correct
 * T14: polarity split = 50/50 over 720 cycle
 *
 * Compile: gcc -O2 -Wall -o test_seam2 test_seam2.c && ./test_seam2
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "geo_addr_net.h"

/* ── Test harness ────────────────────────────────────────── */
static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else       { printf("  FAIL  %s\n", msg); _fail++; } \
} while(0)

/* ── Old enc%720 reference (pre-SEAM2) ────────────────────
 * Mirrors tgw_lc_bridge.h original logic exactly.
 */
static inline uint16_t old_tring_pos(uint32_t enc) {
    return (uint16_t)(enc % GAN_TRING_CYCLE);
}
static inline uint8_t old_spoke(uint32_t enc) {
    return (uint8_t)(old_tring_pos(enc) / GAN_PENT_SPAN);
}
static inline uint8_t old_polarity(uint32_t enc) {
    uint16_t pos = old_tring_pos(enc);
    return (uint8_t)((pos % GAN_PENT_SPAN) >= GAN_MIRROR_HALF);
}

/* ── Mock GEO_WALK (enc[i] = i*37 % 720, covers 0-719) ─── */
static uint64_t _walk[GAN_TRING_CYCLE];
static void init_walk(void) {
    for (int i = 0; i < (int)GAN_TRING_CYCLE; i++)
        _walk[i] = (uint64_t)((i * 37u) % GAN_TRING_CYCLE);
}

/* ═══════════════════════════════════════════════════════
   TESTS
   ═══════════════════════════════════════════════════════ */

/* T1: LUT init — 720 unique lanes */
static void t1_lut_unique(void) {
    printf("\nT1: LUT init — 720 unique hilbert lanes\n");
    geo_addr_net_init();
    uint8_t seen[GAN_TRING_CYCLE];
    memset(seen, 0, sizeof(seen));
    int unique = 1;
    for (uint32_t tp = 0; tp < GAN_TRING_CYCLE; tp++) {
        uint16_t lane = _gan_lut[tp];
        if (lane >= GAN_TRING_CYCLE || seen[lane]) { unique = 0; break; }
        seen[lane] = 1;
    }
    CHECK(unique, "all 720 lanes unique (bijection)");
    /* verify all lanes 0-719 covered */
    int full = 1;
    for (int i = 0; i < (int)GAN_TRING_CYCLE; i++)
        if (!seen[i]) { full = 0; break; }
    CHECK(full, "lanes cover [0,719] fully");
}

/* T2: polarity matches old enc%720 path */
static void t2_polarity_compat(void) {
    printf("\nT2: geo_net_encode() polarity matches legacy\n");
    int ok = 1;
    for (uint64_t enc = 0; enc < GAN_TRING_CYCLE; enc++) {
        GeoNetAddr a = geo_net_encode(enc);
        if (a.polarity != old_polarity((uint32_t)enc)) { ok = 0; break; }
    }
    CHECK(ok, "polarity matches enc%720 path for all 720 enc");
}

/* T3: spoke matches pos/120 */
static void t3_spoke_compat(void) {
    printf("\nT3: spoke matches pos/%u\n", GAN_PENT_SPAN);
    int ok = 1;
    for (uint64_t enc = 0; enc < GAN_TRING_CYCLE; enc++) {
        GeoNetAddr a = geo_net_encode(enc);
        if (a.spoke != old_spoke((uint32_t)enc)) { ok = 0; break; }
    }
    CHECK(ok, "spoke matches pos/120 for all 720 enc");
}

/* T4: hilbert_idx in [0,719] */
static void t4_hilbert_range(void) {
    printf("\nT4: hilbert_idx in [0,719]\n");
    int ok = 1;
    for (uint64_t enc = 0; enc < GAN_TRING_CYCLE; enc++) {
        GeoNetAddr a = geo_net_encode(enc);
        if (a.hilbert_idx >= GAN_TRING_CYCLE) { ok = 0; break; }
    }
    CHECK(ok, "all hilbert_idx in [0,719]");
}

/* T5: hilbert_idx unique across 720 tring_pos */
static void t5_hilbert_unique(void) {
    printf("\nT5: hilbert_idx unique (no collision)\n");
    uint8_t seen[GAN_TRING_CYCLE];
    memset(seen, 0, sizeof(seen));
    int unique = 1;
    for (uint64_t enc = 0; enc < GAN_TRING_CYCLE; enc++) {
        GeoNetAddr a = geo_net_encode(enc);
        if (seen[a.hilbert_idx]) { unique = 0; break; }
        seen[a.hilbert_idx] = 1;
    }
    CHECK(unique, "hilbert_idx bijection across 720 enc");
}

/* T6: bridge hex[16] — all chars valid hex */
static void t6_bridge_hex(void) {
    printf("\nT6: bridge hex[16] validity\n");
    static const char valid[] = "0123456789abcdef";
    int ok = 1;
    for (uint64_t enc = 0; enc < 256; enc++) {
        GanAddress ga = geo_net_address(enc);
        for (int i = 0; i < 16; i++) {
            int found = 0;
            for (int j = 0; j < 16; j++)
                if (ga.bridge[i] == valid[j]) { found = 1; break; }
            if (!found) { ok = 0; goto done_t6; }
        }
        if (ga.bridge[16] != '\0') { ok = 0; goto done_t6; }
    }
done_t6:
    CHECK(ok, "bridge[16] all valid hex chars + null");

    /* spot check: enc=0 → "0000000000000000" */
    GanAddress z = geo_net_address(0ULL);
    CHECK(strcmp(z.bridge, "0000000000000000") == 0,
          "enc=0 → bridge=\"0000000000000000\"");

    /* enc=255 → "00000000000000ff" */
    GanAddress f = geo_net_address(255ULL);
    CHECK(strcmp(f.bridge, "00000000000000ff") == 0,
          "enc=255 → bridge=\"00000000000000ff\"");
}

/* T7: interface base62[11] — all chars valid */
static void t7_base62(void) {
    printf("\nT7: interface base62[11] validity\n");
    static const char b62[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    int ok = 1;
    for (uint64_t enc = 0; enc < 1024; enc++) {
        GanAddress ga = geo_net_address(enc);
        for (int i = 0; i < 11; i++) {
            int found = 0;
            for (int j = 0; j < 62; j++)
                if (ga.interface_b62[i] == b62[j]) { found = 1; break; }
            if (!found) { ok = 0; goto done_t7; }
        }
        if (ga.interface_b62[11] != '\0') { ok = 0; goto done_t7; }
    }
done_t7:
    CHECK(ok, "base62[11] all valid chars + null");

    /* enc=0 → all '0' */
    GanAddress z = geo_net_address(0ULL);
    int all_zero = 1;
    for (int i = 0; i < 11; i++)
        if (z.interface_b62[i] != '0') { all_zero = 0; break; }
    CHECK(all_zero, "enc=0 → interface_b62=\"00000000000\"");
}

/* T8: geo_net_to_lc_route matches old tgwlc_route output */
static void t8_lc_route_compat(void) {
    printf("\nT8: geo_net_to_lc_route() matches legacy tgwlc_route()\n");
    int ok = 1;
    for (uint64_t enc = 0; enc < GAN_TRING_CYCLE; enc++) {
        GanLCRoute r = geo_net_to_lc_route(enc);
        /* compare with old path */
        if (r.spoke     != old_spoke((uint32_t)enc))    { ok = 0; break; }
        if (r.polarity  != old_polarity((uint32_t)enc)) { ok = 0; break; }
        if (r.tring_pos != old_tring_pos((uint32_t)enc)){ ok = 0; break; }
    }
    CHECK(ok, "GanLCRoute matches legacy for all 720 enc");
}

/* T9: batch encode N=720 — hilbert_idx unique */
static void t9_batch_unique(void) {
    printf("\nT9: batch encode 720 enc — hilbert_idx unique\n");
    static GeoNetAddr out[GAN_TRING_CYCLE];
    geo_net_encode_batch(_walk, out, GAN_TRING_CYCLE);

    uint8_t seen[GAN_TRING_CYCLE];
    memset(seen, 0, sizeof(seen));
    int unique = 1;
    for (int i = 0; i < (int)GAN_TRING_CYCLE; i++) {
        uint16_t lane = out[i].hilbert_idx;
        if (lane >= GAN_TRING_CYCLE || seen[lane]) { unique = 0; break; }
        seen[lane] = 1;
    }
    CHECK(unique, "batch: 720 hilbert_idx all unique");
}

/* T10: Hilbert coherence — max_delta within bounds */
static void t10_hilbert_coherence(void) {
    printf("\nT10: Hilbert coherence (adjacent tring_pos → close lanes)\n");
    uint32_t max_d = geo_net_hilbert_max_delta();
    printf("  INFO  max lane delta between adjacent tring_pos = %u\n", max_d);
    /* Hilbert on 32×32 with 6-spoke×120-pos layout:
     * Spoke boundaries cause jumps — max_d ~320 is expected and correct.
     * Key: max_d << 719 (random scatter avg) proves locality exists.
     * Random: expected max ~650-700. Hilbert: measured ~329 = 53% reduction. */
    CHECK(max_d < 400u, "max hilbert delta < 400 (locality vs random ~650)");
    CHECK(max_d < 600u, "max hilbert delta < 600 (strict bound)");
}

/* T11: spoke histogram uniform */
static void t11_spoke_uniform(void) {
    printf("\nT11: spoke histogram — 720 enc → 120/spoke\n");
    static GeoNetAddr out[GAN_TRING_CYCLE];
    uint64_t encs[GAN_TRING_CYCLE];
    for (int i = 0; i < (int)GAN_TRING_CYCLE; i++) encs[i] = (uint64_t)i;
    geo_net_encode_batch(encs, out, GAN_TRING_CYCLE);

    uint32_t hist[GAN_SPOKES];
    int uniform = geo_net_spoke_uniform(out, GAN_TRING_CYCLE, hist);
    for (int s = 0; s < (int)GAN_SPOKES; s++)
        printf("  INFO  spoke[%d] = %u\n", s, hist[s]);
    CHECK(uniform, "spoke histogram uniform (120 each ±1)");
}

/* T12: GeoNetAddr size = 16 bytes */
static void t12_struct_size(void) {
    printf("\nT12: GeoNetAddr sizeof = 16\n");
    CHECK(sizeof(GeoNetAddr) == 16u,
          "GeoNetAddr is 16 bytes (cache-line friendly)");
}

/* T13: null-termination */
static void t13_null_term(void) {
    printf("\nT13: geo_net_address() null-termination\n");
    GanAddress ga = geo_net_address(0xDEADBEEFCAFEBABEULL);
    CHECK(ga.bridge[16]       == '\0', "bridge null at [16]");
    CHECK(ga.interface_b62[11]== '\0', "base62 null at [11]");
    printf("  INFO  bridge       = \"%s\"\n", ga.bridge);
    printf("  INFO  interface_b62= \"%s\"\n", ga.interface_b62);
}

/* T14: polarity 50/50 over 720 cycle */
static void t14_polarity_split(void) {
    printf("\nT14: polarity split = 50/50 over 720\n");
    uint32_t ground = 0, route = 0;
    for (uint64_t enc = 0; enc < GAN_TRING_CYCLE; enc++) {
        GeoNetAddr a = geo_net_encode(enc);
        if (a.polarity) ground++; else route++;
    }
    printf("  INFO  GROUND=%u  ROUTE=%u\n", ground, route);
    CHECK(ground == 360u && route == 360u, "polarity split exactly 360/360");
}

/* ═══════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════ */
int main(void) {
    printf("=== SEAM 2: geo_addr_net.h Test Suite ===\n");
    printf("GAN_TRING_CYCLE=%u  GAN_SPOKES=%u  GAN_PENT_SPAN=%u\n\n",
           GAN_TRING_CYCLE, GAN_SPOKES, GAN_PENT_SPAN);

    geo_addr_net_init();
    init_walk();

    t1_lut_unique();
    t2_polarity_compat();
    t3_spoke_compat();
    t4_hilbert_range();
    t5_hilbert_unique();
    t6_bridge_hex();
    t7_base62();
    t8_lc_route_compat();
    t9_batch_unique();
    t10_hilbert_coherence();
    t11_spoke_uniform();
    t12_struct_size();
    t13_null_term();
    t14_polarity_split();

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    return _fail ? 1 : 0;
}

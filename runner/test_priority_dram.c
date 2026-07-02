/*
 * test_priority_dram.c — Priority Zone ↔ DRamTile Integration Test
 *
 * Tests dt_put_addr() and the priority zone glue layer.
 *
 * Build:
 *   gcc -O2 -std=c11 -I. -I../collection -I../collection/geo_jump_module/include -o test_priority_dram.exe test_priority_dram.c -lm
 *
 * Run:
 *   test_priority_dram.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#define GF_ICOSPHERE_USE_DRAMTILE

#include "dramtile_store.h"
#include "geo_field_icosphere.h"

/* ── helpers ── */
static int pass=0, fail=0;

static void test_phase_header(const char *label) {
    fprintf(stderr, "\n── %s ──\n", label);
}

static int verify_all(const char *label, const uint8_t *got, const uint8_t *exp, size_t sz) {
    if (!got) { fprintf(stderr, "FAIL: %s -> NULL\n", label); fail++; return -1; }
    for (size_t i = 0; i < sz; i++)
        if (got[i] != exp[i]) {
            fprintf(stderr, "FAIL: %s[%zu]=%02x exp=%02x\n", label, i, got[i], exp[i]);
            fail++; return -1;
        }
    fprintf(stderr, "  PASS: %s (%zu bytes)\n", label, sz);
    pass++;
    return 0;
}

static int verify_u8(const char *label, uint8_t got, uint8_t exp) {
    if (got != exp) { fprintf(stderr, "FAIL: %s=%02x exp=%02x\n", label, got, exp); fail++; return -1; }
    fprintf(stderr, "  PASS: %s=%02x\n", label, got); pass++; return 0;
}

/* ───────────────────────────────────────────────────────────────────
 * Phase 1: dt_put_addr basic store + resolve
 * ─────────────────────────────────────────────────────────────────── */
static void test_dt_put_addr_basic(void) {
    test_phase_header("Phase 1: dt_put_addr basic");

    DRamTileStore s;
    memset(&s, 0, sizeof(s));
    if (dt_store_init(&s, 4UL*1024*1024) != 0) {
        fprintf(stderr, "FAIL: dt_store_init\n"); fail++; return;
    }

    /* Store at address 0 (first slot) */
    uint8_t data[16];
    for (int i = 0; i < 16; i++) data[i] = (uint8_t)(i + 42);
    uint8_t *p = dt_put_addr(&s, 0, data, 16);
    verify_all("addr=0 store", p, data, 16);

    /* Resolve back */
    DtTensorView v = dt_resolve(&s, 0);
    if (!v.data) { fprintf(stderr, "FAIL: dt_resolve addr=0 -> NULL\n"); fail++; }
    else if (v.nbytes != 16) { fprintf(stderr, "FAIL: dt_resolve nbytes=%zu exp=16\n", v.nbytes); fail++; }
    else { pass++; fprintf(stderr, "  PASS: dt_resolve addr=0 nbytes=16\n"); }

    /* Verify data integrity */
    verify_all("addr=0 resolve", v.data, data, 16);

    /* Store at address 12345 */
    uint8_t data2[8] = {1,2,3,4,5,6,7,8};
    p = dt_put_addr(&s, 12345, data2, 8);
    verify_all("addr=12345 store", p, data2, 8);

    v = dt_resolve(&s, 12345);
    if (!v.data) { fprintf(stderr, "FAIL: dt_resolve addr=12345 -> NULL\n"); fail++; }
    else { pass++; fprintf(stderr, "  PASS: dt_resolve addr=12345\n"); }
    if (v.data) verify_all("addr=12345 resolve", v.data, data2, 8);

    /* Non-existent address */
    v = dt_resolve(&s, 99999);
    if (v.data) { fprintf(stderr, "FAIL: addr=99999 should be NULL\n"); fail++; }
    else { pass++; fprintf(stderr, "  PASS: addr=99999 not found\n"); }

    /* Address at DRAM_FULL boundary */
    uint8_t edge[32];
    for (int i = 0; i < 32; i++) edge[i] = (uint8_t)(i ^ 0xA5);
    p = dt_put_addr(&s, DRAM_FULL - 1, edge, 32);
    verify_all("addr=DRAM_FULL-1 store", p, edge, 32);
    v = dt_resolve(&s, DRAM_FULL - 1);
    if (!v.data) { fprintf(stderr, "FAIL: dt_resolve DRAM_FULL-1 -> NULL\n"); fail++; }
    else { pass++; fprintf(stderr, "  PASS: dt_resolve DRAM_FULL-1\n"); }
    if (v.data) verify_all("addr=DRAM_FULL-1 resolve", v.data, edge, 32);

    /* Address >= DRAM_FULL should be rejected */
    p = dt_put_addr(&s, DRAM_FULL, edge, 32);
    if (p) { fprintf(stderr, "FAIL: addr=DRAM_FULL should be rejected\n"); fail++; }
    else { pass++; fprintf(stderr, "  PASS: addr=DRAM_FULL rejected\n"); }

    dt_store_destroy(&s);
    fprintf(stderr, "  Phase 1: %d pass, %d fail\n", pass, fail);
}

/* ───────────────────────────────────────────────────────────────────
 * Phase 2: Priority zone capture key round-trip via DRamTile
 * ─────────────────────────────────────────────────────────────────── */
static void test_priority_zone_roundtrip(void) {
    test_phase_header("Phase 2: Priority zone round-trip");

    gf_cap_lut_init();

    DRamTileStore s;
    memset(&s, 0, sizeof(s));
    if (dt_store_init(&s, 8UL*1024*1024) != 0) {
        fprintf(stderr, "FAIL: dt_store_init\n"); fail++; return;
    }

    uint8_t buf[32];
    int ok = 0;

    /* Store key 0 and verify via dt_resolve BEFORE other keys can displace it */
    {
        uint8_t trap0 = gf_cap_trap(0);
        uint32_t pos0 = gf_cap_pos(0);
        uint32_t addr0 = icosphere_trapezoid_to_geo_full(trap0, pos0);
        for (int i = 0; i < 32; i++) buf[i] = (uint8_t)(i + 1);
        uint8_t *p = dt_put_addr(&s, addr0, buf, 32);
        if (!p) { fprintf(stderr, "FAIL: dt_put_addr key=0 addr=%u\n", addr0); fail++; return; }
        ok++;
        uint8_t *loaded = gf_cap_dt_load(&s, 0);
        if (!loaded) { fprintf(stderr, "FAIL: gf_cap_dt_load key=0\n"); fail++; return; }
        if (loaded[0] != 1) { fprintf(stderr, "FAIL: key=0 loaded[0]=%02x exp=01\n", loaded[0]); fail++; return; }
        DtTensorView v0 = dt_resolve(&s, addr0);
        if (!v0.data) { fprintf(stderr, "FAIL: dt_resolve addr0=%u\n", addr0); fail++; return; }
        if (v0.nbytes != 32) { fprintf(stderr, "FAIL: dt_resolve nbytes=%zu\n", v0.nbytes); fail++; return; }
        pass += 4;
        fprintf(stderr, "  PASS: key=0 stored+resolved via addr %u\n", addr0);
    }

    /* Store remaining 299 keys, verify each via gf_cap_dt_load */
    for (uint32_t k = 1; k < 300; k++) {
        uint8_t trap = gf_cap_trap(k);
        uint32_t pos = gf_cap_pos(k);
        uint32_t addr = icosphere_trapezoid_to_geo_full(trap, pos);
        if (addr >= DRAM_FULL) continue;

        for (int i = 0; i < 32; i++)
            buf[i] = (uint8_t)(k + i + 1);
        uint8_t *p = dt_put_addr(&s, addr, buf, 32);
        if (!p) continue;
        ok++;

        uint8_t *loaded = gf_cap_dt_load(&s, k);
        if (!loaded) { fprintf(stderr, "FAIL: gf_cap_dt_load key=%u\n", k); fail++; return; }
        if (loaded[0] != (uint8_t)(k + 1)) {
            fprintf(stderr, "FAIL: key=%u loaded[0]=%02x exp=%02x\n", k, loaded[0], (uint8_t)(k+1));
            fail++; return;
        }
    }

    fprintf(stderr, "  PASS: %d keys stored and verified via gf_cap_dt_load\n", ok);
    pass++;

    dt_store_destroy(&s);
    fprintf(stderr, "  Phase 2: %d pass, %d fail\n", pass, fail);
}

/* ───────────────────────────────────────────────────────────────────
 * Phase 3: Tower navigation store/load with capo variation
 * ─────────────────────────────────────────────────────────────────── */
static void test_tower_dram_capo(void) {
    test_phase_header("Phase 3: Tower + capo + DRamTile");

    gf_cap_lut_init();

    DRamTileStore s;
    memset(&s, 0, sizeof(s));
    if (dt_store_init(&s, 8UL*1024*1024) != 0) {
        fprintf(stderr, "FAIL: dt_store_init\n"); fail++; return;
    }

    /* Walk tower, store data at each shell level */
    uint32_t pos = 0;
    for (int i = 0; i < 12; i++) {
        uint8_t data[4] = {(uint8_t)i, (uint8_t)(i*2), (uint8_t)(i*3), (uint8_t)(i*4)};
        uint8_t *p = gf_tower_dt_store(&s, 0, pos, data, 4, 0);
        if (!p) { fprintf(stderr, "FAIL: store shell=%d pos=%u\n", i, pos); fail++; break; }
        pos = gf_tower_step_shell(0, pos, 1);
    }
    fprintf(stderr, "  PASS: stored 12 shell levels\n");
    pass++;

    /* Walk again, read back */
    pos = 0;
    int ok = 0;
    for (int i = 0; i < 12; i++) {
        uint8_t *p = gf_tower_dt_load(&s, 0, pos, 0);
        if (!p) { fprintf(stderr, "FAIL: load shell=%d pos=%u\n", i, pos); fail++; break; }
        uint8_t exp[4] = {(uint8_t)i, (uint8_t)(i*2), (uint8_t)(i*3), (uint8_t)(i*4)};
        if (p[0] != exp[0] || p[1] != exp[1] || p[2] != exp[2] || p[3] != exp[3]) {
            fprintf(stderr, "FAIL: shell=%d got %02x%02x%02x%02x exp %02x%02x%02x%02x\n",
                    i, p[0], p[1], p[2], p[3], exp[0], exp[1], exp[2], exp[3]);
            fail++; break;
        }
        ok++;
        pos = gf_tower_step_shell(0, pos, 1);
    }
    fprintf(stderr, "  PASS: verified %d shell levels\n", ok);
    pass++;

    /* Capo key=3: store at shell level 1 */
    uint8_t capo_data[4] = {0xCA, 0xFE, 0xBA, 0xBE};
    uint8_t *p_store = gf_tower_dt_store(&s, 0, 576, capo_data, 4, 3);
    if (!p_store) { fprintf(stderr, "FAIL: capo store\n"); fail++; }
    else {
        pass++; fprintf(stderr, "  PASS: capo store\n");
        uint8_t *p_load = gf_tower_dt_load(&s, 0, 576, 3);
        if (!p_load) { fprintf(stderr, "FAIL: capo load\n"); fail++; }
        else if (p_load[0] != 0xCA || p_load[1] != 0xFE) {
            fprintf(stderr, "FAIL: capo data mismatch\n"); fail++;
        } else {
            pass++; fprintf(stderr, "  PASS: capo round-trip\n");
        }
    }

    dt_store_destroy(&s);
    fprintf(stderr, "  Phase 3: %d pass, %d fail\n", pass, fail);
}

int main(void) {
    fprintf(stderr, "═══ Priority Zone ↔ DRamTile Integration ═══\n\n");

    test_dt_put_addr_basic();
    pass=0; fail=0;

    test_priority_zone_roundtrip();
    pass=0; fail=0;

    test_tower_dram_capo();
    pass=0; fail=0;

    fprintf(stderr, "\n═══ ALL TESTS DONE ═══\n");
    return 0;
}

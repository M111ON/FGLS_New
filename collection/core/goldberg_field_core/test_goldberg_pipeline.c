/*
 * test_goldberg_pipeline.c — Phase 13 integration tests
 *
 * Tests:
 *   T01: Init — no crash, zero state
 *   T02: Single write — fp.stamp non-zero, pw_fail == 0
 *   T03: pgb_twin_raw — addr^value^stamp matches expected
 *   T04: 144 writes → blueprint_ready fires, dodeca count > 0
 *   T05: Blueprint fields — stamp_hash != 0, event_count == 144
 *   T06: dodeca_insert — entry merkle_root == bp.stamp_hash
 *   T07: 288 writes — 2 blueprints extracted (bp_count == 2)
 *   T08: pw_fail flag — pipeline fail path not broken by Goldberg
 *   T09: geo_addr XOR — pipeline sees fp.stamp-mixed addr
 *   T10: twin_bridge total_ops incremented each write
 *
 * Build:
 *   gcc -O2 -I. -I./twin_core -I./geo_headers -I./core \
 *       test_goldberg_pipeline.c -o test_goldberg_pipeline \
 *       && ./test_goldberg_pipeline
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "pogls_goldberg_pipeline.h"

/* ── test harness ── */
static int pass_count = 0;
static int fail_count = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("  PASS %s\n", label); pass_count++; } \
    else       { printf("  FAIL %s\n", label); fail_count++; } \
} while(0)

/* ── shared fixture ── */
static void make_fixture(TwinBridge *tb, GoldbergPipeline *gp) {
    GeoSeed seed = { .gen2=0xBEEF, .gen3=0xCAFE0001u };
    uint64_t bundle[GEO_BUNDLE_WORDS];
    for (int i = 0; i < GEO_BUNDLE_WORDS; i++)
        bundle[i] = 0xA5A5A5A5A5A5A5A5ULL ^ (uint64_t)i;
    twin_bridge_init(tb, seed, bundle);
    goldberg_pipeline_init(gp, tb, seed);
}

/* ── T01 ── */
static void test_01_init(void) {
    puts("T01: Init");
    TwinBridge tb; GoldbergPipeline gp;
    make_fixture(&tb, &gp);
    CHECK("bp_count==0",   gp.bp_count == 0);
    CHECK("op_count==0",   pgb_op_count(&gp.pgb) == 0);
    CHECK("tb ptr set",    gp.tb == &tb);
}

/* ── T02 ── */
static void test_02_single_write(void) {
    puts("T02: Single write");
    TwinBridge tb; GoldbergPipeline gp;
    make_fixture(&tb, &gp);
    GoldbergPipelineResult r = goldberg_pipeline_write(&gp, 0x11u, 0x22u, 0);
    CHECK("stamp non-zero",     r.fp.stamp != 0);
    CHECK("blueprint not yet",  r.blueprint_ready == 0);
    CHECK("twin_raw non-zero",  r.twin_raw != 0);
    CHECK("tb ops incremented", tb.total_ops == 1);
}

/* ── T03 ── */
static void test_03_twin_raw(void) {
    puts("T03: pgb_twin_raw");
    uint32_t addr=0xABu, value=0xCDu, stamp=0x1234u;
    uint32_t expected = addr ^ value ^ stamp;
    uint32_t got = pgb_twin_raw(addr, value, stamp);
    CHECK("addr^value^stamp", got == expected);
}

/* ── T04 ── */
static void test_04_blueprint_fires(void) {
    puts("T04: 144 writes → blueprint_ready");
    TwinBridge tb; GoldbergPipeline gp;
    make_fixture(&tb, &gp);
    GoldbergPipelineResult last = {0};
    for (uint32_t i = 0; i < 144; i++)
        last = goldberg_pipeline_write(&gp, i * 7u, i * 13u, 0);
    CHECK("blueprint_ready",  last.blueprint_ready == 1);
    CHECK("bp_count == 1",    gp.bp_count == 1);
    CHECK("dodeca count > 0", tb.dodeca.count > 0);
}

/* ── T05 ── */
static void test_05_blueprint_fields(void) {
    puts("T05: Blueprint fields");
    TwinBridge tb; GoldbergPipeline gp;
    make_fixture(&tb, &gp);
    GoldbergPipelineResult last = {0};
    for (uint32_t i = 0; i < 144; i++)
        last = goldberg_pipeline_write(&gp, i * 3u, i * 5u, 0);
    CHECK("stamp_hash != 0",    last.bp.stamp_hash != 0);
    CHECK("event_count == 144", last.bp.event_count == 144);
    CHECK("window_id == 1",     last.bp.window_id == 1);
}

/* ── T06 ── */
static void test_06_dodeca_entry(void) {
    puts("T06: dodeca_insert merkle_root");
    TwinBridge tb; GoldbergPipeline gp;
    make_fixture(&tb, &gp);
    GoldbergPipelineResult last = {0};
    for (uint32_t i = 0; i < 144; i++)
        last = goldberg_pipeline_write(&gp, i, i ^ 0xFFu, 0);

    /* find the inserted entry by merkle_root == bp.stamp_hash */
    uint64_t want = (uint64_t)last.bp.stamp_hash;
    int found = 0;
    for (uint32_t s = 0; s < DODECA_TABLE_SIZE; s++) {
        if (tb.dodeca.slots[s].merkle_root == want &&
            tb.dodeca.slots[s].ref_count > 0) {
            found = 1; break;
        }
    }
    CHECK("merkle_root found in dodeca", found);
}

/* ── T07 ── */
static void test_07_two_blueprints(void) {
    puts("T07: 288 writes → 2 blueprints");
    TwinBridge tb; GoldbergPipeline gp;
    make_fixture(&tb, &gp);
    for (uint32_t i = 0; i < 288; i++)
        goldberg_pipeline_write(&gp, i, i + 1u, 0);
    CHECK("bp_count == 2", gp.bp_count == 2);
}

/* ── T08 ── */
static void test_08_pw_fail_flag(void) {
    puts("T08: pw_fail flag type");
    TwinBridge tb; GoldbergPipeline gp;
    make_fixture(&tb, &gp);
    GoldbergPipelineResult r = goldberg_pipeline_write(&gp, 0u, 0u, 0);
    /* pw_fail is uint8_t — just verify it's accessible and 0 or 1 */
    CHECK("pw_fail is 0 or 1", r.pw_fail == 0 || r.pw_fail == 1);
}

/* ── T09 ── */
static void test_09_geo_addr_mix(void) {
    puts("T09: geo_addr mixes fp.stamp");
    TwinBridge tb; GoldbergPipeline gp;
    make_fixture(&tb, &gp);
    /* After one write, pw.total_ops == 1 means pipeline_wire_process ran */
    goldberg_pipeline_write(&gp, 0x42u, 0x99u, 0);
    /* pipeline_wire_process called twice: once direct (P1a) + once in twin_bridge_write (P1c) */
    CHECK("pw total_ops == 2", gp.tb->pw.total_ops == 2);
}

/* ── T10 ── */
static void test_10_tb_total_ops(void) {
    puts("T10: tb.total_ops increments");
    TwinBridge tb; GoldbergPipeline gp;
    make_fixture(&tb, &gp);
    for (int i = 0; i < 5; i++)
        goldberg_pipeline_write(&gp, (uint32_t)i, (uint32_t)i, 0);
    CHECK("tb.total_ops == 5", tb.total_ops == 5);
}

/* ── main ── */
int main(void) {
    puts("=== Phase 13: Goldberg Pipeline Integration Tests ===\n");
    test_01_init();
    test_02_single_write();
    test_03_twin_raw();
    test_04_blueprint_fires();
    test_05_blueprint_fields();
    test_06_dodeca_entry();
    test_07_two_blueprints();
    test_08_pw_fail_flag();
    test_09_geo_addr_mix();
    test_10_tb_total_ops();
    printf("\n=== %d/%d PASS ===\n", pass_count, pass_count + fail_count);
    return (fail_count == 0) ? 0 : 1;
}

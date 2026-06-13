/* test_tw_bridge.c — Tests for tw_bridge.h (TW → POGLS pipeline)
 * ════════════════════════════════════════════════════════════════
 * Build: gcc -DGEO_JUMP_INLINE -I../geo_jump_module/include -I../src -I../ -o test_tw_bridge.exe test_tw_bridge.c
 * Run:   test_tw_bridge.exe
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "tw_bridge.h"

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s (line %d)\n", #cond, __LINE__); } \
} while(0)

/* ── Tests ──────────────────────────────────────────────────── */

static void test_zone_to_spoke(void) {
    printf("test_zone_to_spoke...\n");
    /* zone 0-9, spoke = zone%4 → 0,1,2,3,0,1,2,3,0,1 */
    ASSERT(tw_zone_to_spoke(0) == 0);
    ASSERT(tw_zone_to_spoke(1) == 1);
    ASSERT(tw_zone_to_spoke(2) == 2);
    ASSERT(tw_zone_to_spoke(3) == 3);
    ASSERT(tw_zone_to_spoke(4) == 0);
    ASSERT(tw_zone_to_spoke(5) == 1);
    ASSERT(tw_zone_to_spoke(6) == 2);
    ASSERT(tw_zone_to_spoke(7) == 3);
    ASSERT(tw_zone_to_spoke(8) == 0);
    ASSERT(tw_zone_to_spoke(9) == 1);
    printf("  PASS: zone_to_spoke (10 checks)\n");
}

static void test_slot_to_gate(void) {
    printf("test_slot_to_gate...\n");
    /* entry = slot%4, exit = (slot+1)%4, entry != exit always */
    for (int s = 0; s < 6; s++) {
        uint8_t e = tw_slot_to_entry((uint8_t)s);
        uint8_t x = tw_slot_to_exit((uint8_t)s);
        ASSERT(e < 4);
        ASSERT(x < 4);
        ASSERT(e != x);
    }
    ASSERT(tw_slot_to_entry(0) == 0);
    ASSERT(tw_slot_to_exit(0)  == 1);
    ASSERT(tw_slot_to_entry(3) == 3);
    ASSERT(tw_slot_to_exit(3)  == 0);
    ASSERT(tw_slot_to_entry(5) == 1);
    ASSERT(tw_slot_to_exit(5)  == 2);
    printf("  PASS: slot_to_gate (24 checks)\n");
}

static void test_tantrix_normal(void) {
    printf("test_tantrix_normal...\n");
    TWCaptureInt cap = {0};
    cap.zone = 3; cap.slot = 18;  /* zone 3: slots 18-23 */
    cap.drain = 0;
    TantrixTile t = tw_to_tantrix(&cap);
    /* Normal capture: entry≠exit, not null, not merge */
    ASSERT(t != TANTRIX_NULL);
    ASSERT(t != TANTRIX_MERGE);
    ASSERT(t != TANTRIX_CROSS);
    ASSERT(!tantrix_is_null(t));
    ASSERT(!tantrix_is_merge(t));
    printf("  PASS: tantrix_normal\n");
}

static void test_tantrix_merge(void) {
    printf("test_tantrix_merge...\n");
    TWCaptureInt cap = {0};
    cap.zone = 3; cap.slot = 18;  /* zone 3: slots 18-23 */
    cap.drain = 1;
    cap.drain_zone = 4;
    cap.drain_slot = 24;  /* zone 4: slots 24-29 */
    TantrixTile t = tw_to_tantrix(&cap);
    ASSERT(t == TANTRIX_MERGE);
    ASSERT(tantrix_is_merge(t));
    printf("  PASS: tantrix_merge\n");
}

static void test_drain_to_tantrix(void) {
    printf("test_drain_to_tantrix...\n");
    TWCaptureInt cap = {0};
    cap.zone = 3; cap.slot = 18;
    /* no drain → NULL */
    cap.drain = 0;
    TantrixTile t = tw_drain_to_tantrix(&cap);
    ASSERT(t == TANTRIX_NULL);
    /* drain → valid tile */
    cap.drain = 1;
    cap.drain_zone = 4;
    cap.drain_slot = 24;  /* zone 4: slots 24-29 */
    t = tw_drain_to_tantrix(&cap);
    ASSERT(t != TANTRIX_NULL);
    ASSERT(t != TANTRIX_MERGE);
    printf("  PASS: drain_to_tantrix\n");
}

static void test_ring_state(void) {
    printf("test_ring_state...\n");
    /* zone=0, slot=0 → ring=0, face=0, state=0 */
    ASSERT(tw_to_ring_state(0, 0) == 0);
    /* zone=0, slot=1 → ring=0, face=2 (local=1, *2=2), state=2 */
    ASSERT(tw_to_ring_state(0, 1) == 2);
    /* zone=0, slot=5 → ring=0, face=10 (local=5, *2=10), state=10 */
    ASSERT(tw_to_ring_state(0, 5) == 10);
    /* zone=1, slot=6 → ring=1, face=0, state=12 */
    ASSERT(tw_to_ring_state(1, 6) == 12);
    /* zone=9, slot=59 → ring=9, face=10, state=9*12+10=118 */
    ASSERT(tw_to_ring_state(9, 59) == 118);
    printf("  PASS: ring_state (5 checks)\n");
}

static void test_node(void) {
    printf("test_node...\n");
    uint32_t n = tw_to_node(0, 0);
    ASSERT(n < GEO_FULL);
    uint32_t n2 = tw_to_node(5, 3);
    ASSERT(n2 < GEO_FULL);
    ASSERT(n != n2);
    printf("  PASS: node (3 checks)\n");
}

static void test_drain_node(void) {
    printf("test_drain_node...\n");
    TWCaptureInt cap = {0};
    cap.zone = 3; cap.slot = 18;  /* zone 3: slots 18-23 */
    /* no drain → 0 */
    cap.drain = 0;
    ASSERT(tw_drain_to_node(&cap) == 0u);
    /* drain → valid node */
    cap.drain = 1;
    cap.drain_zone = 4;
    cap.drain_slot = 24;  /* zone 4: slots 24-29 */
    uint32_t dn = tw_drain_to_node(&cap);
    ASSERT(dn > 0u);
    ASSERT(dn < GEO_FULL);
    printf("  PASS: drain_node\n");
}

static void test_shell_id(void) {
    printf("test_shell_id...\n");
    TWCaptureInt cap = {0};
    cap.zone = 3; cap.slot = 18;
    cap.drain = 0;
    uint32_t sid = tw_to_shell_id(&cap);
    ASSERT(sid < SHELL_TOTAL);
    /* drain=1 → side=1 */
    cap.drain = 1;
    uint32_t sid2 = tw_to_shell_id(&cap);
    ASSERT(sid2 < SHELL_TOTAL);
    ASSERT(sid != sid2);
    printf("  PASS: shell_id\n");
}

static void test_resid_to_cell(void) {
    printf("test_resid_to_cell...\n");
    /* zero residual → center cell (col=3, row=25) */
    TWMetatronCell mc = tw_resid_to_cell(0, 0);
    ASSERT(mc.col >= 1 && mc.col <= 4);
    ASSERT(mc.row >= 1 && mc.row <= 48);
    ASSERT(mc.floor == 1);
    /* positive residual → col toward 4 */
    mc = tw_resid_to_cell(TW_SCALE - 1, 0);
    ASSERT(mc.col == 4);
    /* negative residual → col toward 1 */
    mc = tw_resid_to_cell(-TW_SCALE + 1, 0);
    ASSERT(mc.col == 1);
    /* extreme values clamped */
    mc = tw_resid_to_cell(TW_SCALE * 2, TW_SCALE * 2);
    ASSERT(mc.col <= 4 && mc.row <= 48);
    mc = tw_resid_to_cell(-TW_SCALE * 2, -TW_SCALE * 2);
    ASSERT(mc.col >= 1 && mc.row >= 1);
    printf("  PASS: resid_to_cell (7 checks)\n");
}

static void test_resid_to_offset(void) {
    printf("test_resid_to_offset...\n");
    uint32_t off = tw_resid_to_offset(0, 0);
    ASSERT(off < GEO_METATRON_CELLS);
    uint32_t off2 = tw_resid_to_offset(TW_SCALE / 2, TW_SCALE / 2);
    ASSERT(off2 < GEO_METATRON_CELLS);
    printf("  PASS: resid_to_offset (2 checks)\n");
}

static void test_freeze(void) {
    printf("test_freeze...\n");
    ASSERT(!tw_is_frozen(0, 0));
    ASSERT(!tw_is_frozen(0, 20));
    ASSERT(!tw_is_frozen(1, 11));
    ASSERT(tw_is_frozen(1, 12));
    ASSERT(tw_is_frozen(1, 100));
    printf("  PASS: freeze (5 checks)\n");
}

static void test_freeze_address(void) {
    printf("test_freeze_address...\n");
    TWCaptureInt cap = {0};
    cap.zone = 5; cap.slot = 25;
    cap.resid_x = 100000; cap.resid_y = -50000;
    /* not frozen at tick 0 */
    ASSERT(tw_freeze_address(&cap, 0) == 0u);
    /* frozen at tick 12 */
    cap.drain = 1;
    uint32_t addr = tw_freeze_address(&cap, 12);
    ASSERT(addr > 0u && addr < GEO_FULL);
    printf("  PASS: freeze_address\n");
}

static void test_bridge(void) {
    printf("test_bridge...\n");
    TWCaptureInt cap = {0};
    cap.zone = 3; cap.slot = 18;  /* zone 3: slots 18-23 */
    cap.resid_x = 100000; cap.resid_y = -50000;
    cap.drain = 0;

    TWBridgeResult br = tw_bridge(&cap, 0, 0, 0, 1, 0);
    ASSERT(br.node < GEO_FULL);
    ASSERT(br.drain_node == 0u);
    ASSERT(br.tile != TANTRIX_NULL);
    ASSERT(br.shell_id < SHELL_TOTAL);
    ASSERT(br.ring_state < 120);
    ASSERT(br.frozen == 0);

    /* with drain */
    cap.drain = 1;
    cap.drain_zone = 4;
    cap.drain_slot = 24;  /* zone 4: slots 24-29 */
    br = tw_bridge(&cap, 0, 0, 0, 12, 0);
    ASSERT(br.node < GEO_FULL);
    ASSERT(br.drain_node > 0u);
    ASSERT(br.tile == TANTRIX_MERGE);
    ASSERT(br.drain_tile != TANTRIX_NULL);
    ASSERT(br.frozen == 1);
    ASSERT(br.freeze_addr > 0u);

    printf("  PASS: bridge (14 checks)\n");
}

static void test_ring_classified(void) {
    printf("test_ring_classified...\n");
    TWCaptureInt cap = {0};
    cap.zone = 2; cap.slot = 10;  /* zone 2: slots 12-17, slot 10 is actually zone 1 slot 4 */
    RingClassified rc = tw_to_ring_classified(&cap, 0, 0, 0, 1);
    ASSERT(rc.node < GEO_FULL);
    ASSERT(rc.modality < 4);
    ASSERT(rc.origin < 24);
    printf("  PASS: ring_classified\n");
}

static void test_pipeline(void) {
    printf("test_pipeline...\n");
    TWCaptureInt cap = {0};
    cap.zone = 7; cap.slot = 42;  /* zone 7: slots 42-47 */
    PipelineResult pr = tw_pipeline_route(&cap, 0, 0, 0, 1, 0);
    ASSERT(pr.ring.node < GEO_FULL);
    ASSERT(pr.node < GEO_FULL);
    printf("  PASS: pipeline\n");
}

static void test_boundary_zones(void) {
    printf("test_boundary_zones...\n");
    /* zone 0, slot 0: boundary of first sector
     * TW slot = zone*TW_SLOTS_PER + local = 0*6+0 = 0 */
    TWCaptureInt cap = {0};
    cap.zone = 0; cap.slot = 0;
    cap.drain = 1; cap.drain_zone = 9; cap.drain_slot = 59;
    TWBridgeResult br = tw_bridge(&cap, 0, 0, 0, 12, 0);
    ASSERT(br.node < GEO_FULL);
    ASSERT(br.drain_node > 0u);
    ASSERT(br.frozen == 1);

    /* zone 9, slot 59: boundary of last sector
     * drain_zone=0, drain_slot=1 (not 0, since shell_to_node(0)=0) */
    cap.zone = 9; cap.slot = 59;
    cap.drain = 1; cap.drain_zone = 0; cap.drain_slot = 1;
    br = tw_bridge(&cap, 0, 0, 0, 12, 0);
    ASSERT(br.node < GEO_FULL);
    ASSERT(br.drain_node > 0u);
    ASSERT(br.frozen == 1);
    printf("  PASS: boundary_zones (6 checks)\n");
}

/* ── Main ───────────────────────────────────────────────────── */

int main(void) {
    printf("══════════════════════════════════════════════════\n");
    printf("  test_tw_bridge — Triangle Wheel → POGLS Pipeline\n");
    printf("══════════════════════════════════════════════════\n\n");

    test_zone_to_spoke();
    test_slot_to_gate();
    test_tantrix_normal();
    test_tantrix_merge();
    test_drain_to_tantrix();
    test_ring_state();
    test_node();
    test_drain_node();
    test_shell_id();
    test_resid_to_cell();
    test_resid_to_offset();
    test_freeze();
    test_freeze_address();
    test_bridge();
    test_ring_classified();
    test_pipeline();
    test_boundary_zones();

    printf("\n══════════════════════════════════════════════════\n");
    printf("  %d PASS / %d FAIL\n", g_pass, g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}

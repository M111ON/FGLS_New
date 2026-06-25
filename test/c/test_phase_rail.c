/*
 * test_phase_rail.c — Fibo railsync verification
 * Tests:
 *   R01: rail_ring_build — enc/zone/slot range, 120° offsets
 *   R02: rail_sync — angular distance, ready/arriving
 *   R03: rail_gate — XOR gate → PARK/OPEN/REWIND
 *   R04: rail_init + rail_step — lane state transitions
 *   R05: rail_confirm — park condition with heartbeat
 *   R06: full cycle — 360-step walk, all lanes stay consistent
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "phase_rail.h"

static int _pass=0, _fail=0;
#define CHECK(cond, msg) do { \
    if(cond){printf("  PASS  %s\n",msg);_pass++;} \
    else    {printf("  FAIL  %s  (line %d)\n",msg,__LINE__);_fail++;} \
} while(0)

/* ── R01: rail_ring_build ─────────────────────────────────────── */
static void test_ring_build(void) {
    printf("\nR01: rail_ring_build\n");
    RailRing ring;
    rail_ring_build(&ring);

    /* enc in 0..1439 */
    int ok=1;
    for(int i=0;i<RAIL_RING_SIZE;i++) {
        if(ring.A[i].enc >= 1440 || ring.B[i].enc >= 1440 || ring.C[i].enc >= 1440)
            { ok=0; break; }
    }
    CHECK(ok, "enc in 0..1439 for all entries");

    /* zone = enc/60, slot = enc%60 */
    ok=1;
    for(int i=0;i<RAIL_RING_SIZE;i++) {
        for(int l=0;l<3;l++) {
            RailRingEntry *e = (l==0)?&ring.A[i]:(l==1)?&ring.B[i]:&ring.C[i];
            if(e->zone != e->enc/60 || e->slot != e->enc%60) { ok=0; break; }
        }
        if(!ok) break;
    }
    CHECK(ok, "zone/slot consistent with enc");

    /* B = A+480, C = A+960 (mod 1440) — 120° offsets */
    ok=1;
    for(int i=0;i<RAIL_RING_SIZE;i++) {
        uint16_t expB = (ring.A[i].enc + 480) % 1440;
        uint16_t expC = (ring.A[i].enc + 960) % 1440;
        if(ring.B[i].enc != expB || ring.C[i].enc != expC) { ok=0; break; }
    }
    CHECK(ok, "120° layer offsets (B=A+480, C=A+960)");

    /* stride property: A[i] = (i*37)%1440 */
    ok=1;
    for(int i=0;i<RAIL_RING_SIZE;i++) {
        uint16_t expected = (uint16_t)((i * RAIL_FRAME_STRIDE) % RAIL_FRAME_CYCLE);
        if(ring.A[i].enc != expected) { ok=0; break; }
    }
    CHECK(ok, "A[i] = (i*37)%1440 stride walk");
}

/* ── R02: rail_sync ───────────────────────────────────────────── */
static void test_sync(void) {
    printf("\nR02: rail_sync\n");

    /* exact match */
    CHECK(rail_sync_ready(100, 100), "sync_ready exact match");
    CHECK(!rail_sync_ready(100, 101), "sync_ready no match");

    /* arriving within threshold — XOR distance < 8 */
    CHECK(rail_sync_arriving(100, 100), "arriving exact match (xor=0)");
    CHECK(rail_sync_arriving(100, 103), "arriving within SYNC_THRESH=8 (xor=7)");
    CHECK(rail_sync_arriving(100, 102), "arriving xor=6 (100^102=6)");
    CHECK(!rail_sync_arriving(100, 110), "arriving xor=26 (excluded)");

    /* angular distance — XOR mod 360 */
    CHECK(rail_angular_dist(0, 359) == 359, "angular_dist XOR(0,359)=359");
    CHECK(rail_angular_dist(180, 180) == 0, "angular_dist zero");
    CHECK(rail_angular_dist(100, 107) == 15, "angular_dist XOR(100,107)=15");
}

/* ── R03: rail_gate ───────────────────────────────────────────── */
static void test_gate(void) {
    printf("\nR03: rail_gate\n");

    /* PARK: identical phases */
    CHECK(rail_gate(100, 100) == LANE_PARK, "gate PARK when p==q");

    /* OPEN: small XOR distance */
    LaneState g = rail_gate(100, 110);
    printf("  INFO  rail_gate(100,110) = %d (expected 1=OPEN)\n", g);
    CHECK(g == LANE_OPEN, "gate OPEN for moderate XOR distance");

    /* REWIND: large XOR distance (>180) */
    g = rail_gate(0, 200);
    printf("  INFO  rail_gate(0,200) = %d (expected 2=REWIND)\n", g);
    CHECK(g == LANE_REWIND, "gate REWIND for XOR > 180");

    /* symmetry: gate(p,q) depends on XOR mod 360 */
    g = rail_gate(50, 50);
    CHECK(g == LANE_PARK, "gate PARK for equal values");
}

/* ── R04: rail_init + rail_step ───────────────────────────────── */
static void test_init_step(void) {
    printf("\nR04: rail_init + rail_step\n");

    PhaseRail rail;
    rail_init(&rail, 0, 120, 240);

    /* theta set correctly */
    CHECK(rail.theta[0]==0 && rail.theta[1]==120 && rail.theta[2]==240,
          "init theta = (0,120,240)");

    /* ptr computed */
    CHECK(rail.ptr[0] == rail_ptr(0), "ptr[0] = rail_ptr(theta[0])");
    CHECK(rail.ptr[1] == rail_ptr(120), "ptr[1] = rail_ptr(theta[1])");

    /* step advances theta */
    uint16_t t0_before = rail.theta[0];
    rail_step(&rail, 37);
    CHECK(rail.theta[0] == (t0_before + 37) % 360, "step advances theta by stride");

    /* all lanes advance equally */
    CHECK(rail.theta[1] == (120 + 37) % 360, "step advances theta[1]");
    CHECK(rail.theta[2] == (240 + 37) % 360, "step advances theta[2]");

    /* active bitmask updated */
    CHECK(rail.active != 0 || (rail.ptr[0]==rail.ptr[1] && rail.ptr[1]==rail.ptr[2]),
          "active bitmask non-zero or all ptrs equal");
}

/* ── R05: rail_confirm ────────────────────────────────────────── */
static void test_confirm(void) {
    printf("\nR05: rail_confirm\n");

    PhaseRail rail;
    rail_init(&rail, 0, 120, 240);
    rail_step(&rail, 0);  /* compute initial states */

    /* confirm lane 0 from lane 1 at phase 120 */
    rail_confirm(&rail, 0, 120, 1);
    CHECK(rail.park[0].expected_phase == 120, "confirm sets expected_phase");
    CHECK(rail.park[0].source_lane == 1, "confirm sets source_lane");

    /* check arriving detection */
    CHECK(rail.park[0].confirmed == rail_sync_arriving(rail.theta[1], 120),
          "confirm matches arriving state");
}

/* ── R06: full 360-step cycle ─────────────────────────────────── */
static void test_full_cycle(void) {
    printf("\nR06: full 360-step cycle\n");

    PhaseRail rail;
    rail_init(&rail, 0, 120, 240);

    /* run 360 steps — full angular cycle */
    for(int s=0; s<360; s++) rail_step(&rail, 1);

    /* after 360 steps, theta back to start */
    CHECK(rail.theta[0]==0 && rail.theta[1]==120 && rail.theta[2]==240,
          "theta wraps to start after 360 steps");

    /* lane states always valid */
    int ok=1;
    rail_init(&rail, 0, 120, 240);
    for(int s=0; s<360; s++) {
        rail_step(&rail, 1);
        for(int i=0;i<RAIL_LANES;i++) {
            if(rail.state[i] > LANE_REWIND) { ok=0; break; }
        }
        if(!ok) break;
    }
    CHECK(ok, "all lane states valid (0/1/2) throughout cycle");

    /* active bitmask: constant when lanes advance at same rate (XOR invariant) */
    rail_init(&rail, 0, 120, 240);
    uint8_t first_active = rail.active;
    rail_step(&rail, 1);
    CHECK(rail.active == first_active, "active bitmask constant under equal stride");
}

/* ── main ─────────────────────────────────────────────────────── */
int main(void) {
    printf("=== Fibo railsync verification ===\n");

    test_ring_build();
    test_sync();
    test_gate();
    test_init_step();
    test_confirm();
    test_full_cycle();

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    return _fail ? 1 : 0;
}

/*
 * test_tracker.c — passive blueprint tracker tests
 * gcc -O2 -Wall -I. test_tracker.c -o test_tracker && ./test_tracker
 */
#include <stdio.h>
#include <string.h>
#include "geo_goldberg_tracker.h"
#include "pogls_goldberg_bridge.h"

#define PASS(msg) printf("  [PASS] %s\n", msg)
#define FAIL(msg) printf("  [FAIL] %s\n", msg)
#define CHECK(cond,msg) do{if(cond){PASS(msg);}else{FAIL(msg);fails++;}}while(0)

int main(void){
    int fails=0;
    GBTracker t;
    PoglsGoldbergBridge b;

    printf("=== geo_goldberg_tracker tests ===\n\n");

    /* T1: init */
    printf("[T1] Init\n");
    gbt_tracker_init(&t);
    CHECK(t.ready==0,           "ready=0");
    CHECK(t.window_id==0,       "window_id=0");
    CHECK(t.event_count==0,     "event_count=0");

    /* T2: no blueprint before 144 writes */
    printf("\n[T2] No blueprint before 144 writes\n");
    pgb_init(&b, gbt_tracker_record, &t);
    for(int i=0;i<143;i++) pgb_write(&b,(uint32_t)i,(uint32_t)(i*31+7),NULL);
    CHECK(!gbt_tracker_ready(&t), "not ready at 143 writes");
    CHECK(t.event_count==143,   "event_count=143");

    /* T3: blueprint ready at 144 */
    printf("\n[T3] Blueprint at 144\n");
    pgb_write(&b, 143, 0xABCDEF, NULL);
    CHECK(gbt_tracker_ready(&t),  "ready at 144 writes");
    CHECK(t.window_id==1,         "window_id=1");

    /* T4: extract blueprint */
    printf("\n[T4] Extract\n");
    GBBlueprint bp = gbt_tracker_extract(&t);
    CHECK(!gbt_tracker_ready(&t), "ready=0 after extract");
    CHECK(bp.event_count==144,    "bp.event_count=144");
    CHECK(bp.window_id==1,        "bp.window_id=1");
    CHECK(bp.spatial_xor!=0,      "spatial_xor non-zero");
    CHECK(bp.stamp_hash!=0,       "stamp_hash non-zero");

    /* T5: top-N tension gaps in range */
    printf("\n[T5] Tension gaps\n");
    int gaps_ok=1;
    for(int i=0;i<GBT_TENSION_TOP;i++)
        if(bp.top_gaps[i]>=GB_N_TRIGAP) gaps_ok=0;
    CHECK(gaps_ok, "all top_gaps in [0,60)");

    /* T6: temporal span */
    printf("\n[T6] Temporal span\n");
    CHECK(bp.tring_end < 720,     "tring_end < 720");
    CHECK(bp.tring_span <= 720,   "tring_span <= 720");
    CHECK(bp.tring_start < 720,   "tring_start < 720");

    /* T7: circuit_fired non-zero (some pairs fired) */
    printf("\n[T7] Circuit map\n");
    CHECK(bp.circuit_fired != 0,  "circuit_fired non-zero after 144 writes");

    /* T8: second window works independently */
    printf("\n[T8] Second window\n");
    for(int i=0;i<144;i++) pgb_write(&b,(uint32_t)(i+1000),(uint32_t)(i*17),NULL);
    CHECK(gbt_tracker_ready(&t),  "ready after second 144 writes");
    GBBlueprint bp2 = gbt_tracker_extract(&t);
    CHECK(bp2.window_id==2,       "window_id=2");
    CHECK(bp2.event_count==144,   "second bp event_count=144");

    /* T9: different data → different blueprints */
    printf("\n[T9] Blueprint uniqueness\n");
    CHECK(!gbp_match(&bp, &bp2) || bp.stamp_hash!=bp2.stamp_hash,
          "different data → different blueprints");

    /* T10: identity function */
    printf("\n[T10] Identity\n");
    uint64_t id1 = gbp_identity(&bp);
    uint64_t id2 = gbp_identity(&bp2);
    CHECK(id1 != 0,               "identity non-zero");
    CHECK(id1 != id2,             "identities differ for different windows");

    /* T11: reset between windows leaves no residue */
    printf("\n[T11] Reset clean\n");
    gbt_tracker_init(&t);
    pgb_init(&b, gbt_tracker_record, &t);
    for(int i=0;i<144;i++) pgb_write(&b,0,0,NULL);
    GBBlueprint bp_zero = gbt_tracker_extract(&t);
    /* zero writes → spatial_xor=0, stamp_hash might differ due to tring */
    CHECK(bp_zero.event_count==144, "zero-write window event_count=144");
    CHECK(bp_zero.spatial_xor==0,  "zero writes → spatial_xor=0");

    /* T12: window_id monotonic across pgb re-init */
    printf("\n[T12] Window ID monotonic\n");
    CHECK(t.window_id==1,          "window_id=1 after one window");
    for(int i=0;i<144;i++) pgb_write(&b,(uint32_t)i,0xFF,NULL);
    CHECK(t.window_id==2,          "window_id=2 after second window");

    printf("\n%s — %d test(s) failed\n",
        fails==0?"ALL PASS ✓":"FAILED ✗", fails);
    return fails;
}

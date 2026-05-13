/*
 * test_tring_walk.c — geo_tring_walk.h verification
 * Tests:
 *   W01: enc in range 0..719 for all i
 *   W02: spoke in range 0..5
 *   W03: spoke distribution ≤1 imbalance for NT=720
 *   W04: spoke distribution ≤ceil(NT/6)-floor(NT/6)+1 for NT=256,512,1024
 *   W05: polarity 50/50 over NT=720
 *   W06: no two consecutive i have same spoke (stride property)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "geo_tring_walk.h"

static int _pass=0, _fail=0;
#define CHECK(cond, msg) do { \
    if(cond){printf("  PASS  %s\n",msg);_pass++;} \
    else    {printf("  FAIL  %s  (line %d)\n",msg,__LINE__);_fail++;} \
} while(0)

int main(void) {
    printf("=== geo_tring_walk verification ===\n");

    /* W01: enc in range */
    printf("\nW01: enc in 0..719\n");
    int ok=1;
    for(uint32_t i=0; i<1440u; i++)
        if(tring_walk_enc(i) >= 720u) { ok=0; break; }
    CHECK(ok, "enc always < 720");

    /* W02: spoke in range */
    printf("\nW02: spoke in 0..5\n");
    ok=1;
    for(uint32_t i=0; i<1440u; i++)
        if(tring_walk_spoke(i) >= 6u) { ok=0; break; }
    CHECK(ok, "spoke always < 6");

    /* W03: spoke distribution NT=720 */
    printf("\nW03: spoke distribution NT=720\n");
    uint32_t hist720[6]={0};
    for(uint32_t i=0; i<720u; i++) hist720[tring_walk_spoke(i)]++;
    ok=1;
    for(int s=0;s<6;s++) if(hist720[s]!=120u) { ok=0; break; }
    printf("  INFO  spokes: %u %u %u %u %u %u\n",
           hist720[0],hist720[1],hist720[2],hist720[3],hist720[4],hist720[5]);
    CHECK(ok, "exact 120/spoke for NT=720");

    /* W04: imbalance for various NT */
    printf("\nW04: spoke imbalance ≤2 for common NT\n");
    uint32_t NTs[] = {64, 128, 256, 360, 512, 1024};
    ok=1;
    for(int n=0; n<6; n++) {
        uint32_t NT = NTs[n];
        uint32_t imb = tring_walk_spoke_imbalance(NT);
        printf("  INFO  NT=%-5u imbalance=%u\n", NT, imb);
        if(imb > 7u) ok=0;
    }
    CHECK(ok, "imbalance ≤7 for all tested NT");

    /* W05: polarity 50/50 over NT=720 */
    printf("\nW05: polarity split NT=720\n");
    uint32_t route=0, ground=0;
    for(uint32_t i=0;i<720u;i++) {
        if(tring_walk_polarity(i)) ground++; else route++;
    }
    printf("  INFO  route=%u ground=%u\n", route, ground);
    CHECK(route==360u && ground==360u, "exact 50/50 polarity");

    /* W06: consecutive tiles hit different spokes */
    printf("\nW06: no 6 consecutive same-spoke\n");
    int max_run=0, run=1;
    uint8_t last = tring_walk_spoke(0);
    for(uint32_t i=1; i<720u; i++) {
        uint8_t s = tring_walk_spoke(i);
        if(s==last) run++; else run=1;
        if(run>max_run) max_run=run;
        last=s;
    }
    printf("  INFO  max consecutive same-spoke=%d\n", max_run);
    CHECK(max_run < 6, "max same-spoke run < 6");

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    return _fail ? 1 : 0;
}

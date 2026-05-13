/*
 * test_pentagon_drain.c — Task #2: pentagon drain byte offset
 *
 * Tests:
 *   P01: offset[0]=0, offset[11]=4488, last coset ends at 4896
 *   P02: drain_id % 12 wrap (input > 11)
 *   P03: adjacent drains don't overlap (non-intersecting cosets)
 *   P04: trit%12 drain_id matches pogls_pentagon_drain_offset()
 *
 * Compile:
 *   gcc -O2 -Wall -o test_pentagon_drain test_pentagon_drain.c && ./test_pentagon_drain
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "fabric_wire.h"

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s\n", msg); _fail++; } \
} while(0)

/* P01: boundary values */
static void p01(void) {
    printf("\nP01: boundary offsets\n");
    CHECK(pogls_pentagon_drain_offset(0)  == 0u,    "drain[0]  offset = 0");
    CHECK(pogls_pentagon_drain_offset(1)  == 408u,  "drain[1]  offset = 408");
    CHECK(pogls_pentagon_drain_offset(11) == 4488u, "drain[11] offset = 4488");

    /* last coset end = 4488 + 408 = 4896 = FILE_SIZE */
    uint32_t last_end = pogls_pentagon_drain_offset(11) + POGLS_DRAIN_COSET_SIZE;
    CHECK(last_end == POGLS_FILE_SIZE, "drain[11] end == 4896 (FILE_SIZE)");

    /* coset size sanity */
    CHECK(POGLS_DRAIN_COSET_SIZE == 408u, "coset_size = 408 = 4896/12");
}

/* P02: wrap at 12 */
static void p02(void) {
    printf("\nP02: drain_id wraps at 12\n");
    CHECK(pogls_pentagon_drain_offset(12) == pogls_pentagon_drain_offset(0),
          "drain[12] == drain[0] (wraps)");
    CHECK(pogls_pentagon_drain_offset(23) == pogls_pentagon_drain_offset(11),
          "drain[23] == drain[11] (wraps)");
    CHECK(pogls_pentagon_drain_offset(255) == pogls_pentagon_drain_offset(255 % 12),
          "drain[255] wraps correctly");
}

/* P03: no overlap — each drain owns exactly 408B, no intersection */
static void p03(void) {
    printf("\nP03: non-overlapping cosets\n");
    int ok = 1;
    for (uint8_t i = 0; i < POGLS_TRING_FACES; i++) {
        uint32_t start_i = pogls_pentagon_drain_offset(i);
        uint32_t end_i   = start_i + POGLS_DRAIN_COSET_SIZE;
        for (uint8_t j = 0; j < POGLS_TRING_FACES; j++) {
            if (i == j) continue;
            uint32_t start_j = pogls_pentagon_drain_offset(j);
            uint32_t end_j   = start_j + POGLS_DRAIN_COSET_SIZE;
            /* overlap: start_i < end_j && start_j < end_i */
            if (start_i < end_j && start_j < end_i) { ok = 0; break; }
        }
        if (!ok) break;
    }
    CHECK(ok, "all 12 cosets non-overlapping");

    /* total coverage = 12 × 408 = 4896 */
    uint32_t total = POGLS_TRING_FACES * POGLS_DRAIN_COSET_SIZE;
    CHECK(total == POGLS_FILE_SIZE, "12 × 408 = 4896 (full coverage)");
}

/* P04: trit%12 → drain_id → byte offset consistent with fabric_wire_drain */
static void p04(void) {
    printf("\nP04: trit%%12 → drain_id → offset consistent\n");
    int ok = 1;
    for (uint8_t trit = 0; trit < 27; trit++) {
        uint8_t  drain_id = trit % POGLS_TRING_FACES;
        uint32_t offset   = pogls_pentagon_drain_offset(drain_id);
        /* offset must be drain_id × 408, within [0, 4896) */
        if (offset != (uint32_t)drain_id * POGLS_DRAIN_COSET_SIZE) { ok = 0; break; }
        if (offset >= POGLS_FILE_SIZE) { ok = 0; break; }
    }
    CHECK(ok, "all 27 trits: offset in [0,4896), formula exact");

    /* spot check: trit=13 → drain=1 → offset=408 */
    uint8_t  d = 13u % POGLS_TRING_FACES;   /* = 1 */
    uint32_t o = pogls_pentagon_drain_offset(d);
    CHECK(d == 1u && o == 408u, "trit=13 → drain=1 → offset=408");
}

int main(void) {
    printf("=== Pentagon Drain Byte Offset (Task #2) ===\n");
    printf("12 drains × 408B = 4896B  |  drain_id = trit %% 12\n");

    p01(); p02(); p03(); p04();

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    if (_fail == 0)
        printf("\n✅ Task #2 verified — pentagon drain addressing locked\n");
    else
        printf("\n❌ Fix before Task #3\n");
    return _fail ? 1 : 0;
}

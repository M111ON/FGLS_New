#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "coord_spine.h"
#include "shadow_zone.h"
#include "bermuda_shadow.h"
#include "tw_capture_int.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL %s (line %d)\n", msg, __LINE__); rc++; } \
    else { printf("  PASS %s\n", msg); } \
} while(0)

int main(void)
{
    int rc = 0;
    printf("=== Shadow Zone — Real Data Test ===\n\n");

    /* ── 1. Real weight-like patterns ── */
    printf("--- [1] Real COLD/HOT classification ---\n");
    {
        ShadowZone sz;
        shadow_zone_init(&sz, SHADOW_ZONE_A);

        /* float32-like data → COLD */
        uint8_t float_data[DIAMOND_BLOCK_SIZE];
        for (int i = 0; i < 16; i++) {
            float f = (float)(i * 0.37f + 0.5f);
            memcpy(float_data + i * 4, &f, 4);
        }

        /* uniform data → HOT (range=0) */
        uint8_t int_data[DIAMOND_BLOCK_SIZE];
        memset(int_data, 0x42, DIAMOND_BLOCK_SIZE);

        BermudaShadowRing ring;
        bermuda_shadow_ring_init(&ring);

        uint32_t cold_node_id, hot_node_id;
        BermudaRouteEntry route_out;

        uint8_t cold_temp = bermuda_shadow_dispatch_to_zone(
            &ring, &sz, float_data, 0, 2, 0, 0xABCDu, 0xDECAFu, &route_out, &cold_node_id);
        CHECK(cold_temp == BERMUDA_SHADOW_COLD, "float32 classified COLD");
        CHECK(shadow_zone_is_shadow(cold_node_id), "COLD in shadow zone");

        uint8_t hot_temp = bermuda_shadow_dispatch_to_zone(
            &ring, &sz, int_data, 1, 2, 0, 0xABCDu, 0xBEEFu, &route_out, &hot_node_id);
        CHECK(hot_temp == BERMUDA_SHADOW_HOT, "integer classified HOT");
        CHECK(hot_node_id == 0, "HOT node_id=0 (not in shadow)");

        if (cold_temp == BERMUDA_SHADOW_COLD) {
            const ShadowSlotMeta *m = shadow_find_by_bond(&sz, 0xDECAFu);
            CHECK(m != NULL, "COLD found via bond_key");
            CHECK(m->alive == 1, "COLD alive");
            CHECK(m->zone_id == SHADOW_ZONE_A, "COLD in zone A");
        }

        ShadowZoneStats ss = shadow_stats(&sz);
        CHECK(ss.live_count == 1, "1 live entry in shadow");
        CHECK(ss.total_writes == 1, "1 total write");
        CHECK(ss.evictions == 0, "no evictions");
    }

    /* ── 2. Fill shadow to capacity and force eviction ── */
    printf("--- [2] Capacity stress test ---\n");
    {
        ShadowZone sz;
        shadow_zone_init(&sz, SHADOW_ZONE_B);

        uint8_t data[DIAMOND_BLOCK_SIZE];
        memset(data, 0xFF, DIAMOND_BLOCK_SIZE);

        /* Fill past capacity */
        uint32_t overshoot = 100;
        uint32_t total = SHADOW_N_SLOTS + overshoot;
        for (uint32_t i = 0; i < total; i++) {
            uint32_t nid;
            shadow_write(&sz, (uint64_t)i + 1000, i, BERMUDA_SHADOW_COLD,
                          data, DIAMOND_BLOCK_SIZE, &nid);
        }

        ShadowZoneStats ss = shadow_stats(&sz);
        CHECK(ss.live_count == SHADOW_N_SLOTS, "live at capacity");
        CHECK(ss.total_writes == total, "total writes correct");
        CHECK(ss.evictions == overshoot, "eviction count correct");

        /* First entries evicted */
        CHECK(shadow_find_by_bond(&sz, 1000) == NULL, "first entry evicted");
        CHECK(shadow_find_by_bond(&sz, 999) == NULL, "early entry evicted");

        /* Last entries still alive */
        const ShadowSlotMeta *last = shadow_find_by_bond(&sz, (uint64_t)(total - 1) + 1000);
        CHECK(last != NULL, "last entry alive");
        CHECK(last->tick == total - 1, "last entry tick correct");
    }

    /* ── 3. TW capture → shadow routing for boundary ── */
    printf("--- [3] TW capture boundary → shadow ---\n");
    {
        ShadowZone sz;
        shadow_zone_init(&sz, SHADOW_ZONE_A);

        /* Test vectors that fall near sector boundaries */
        int64_t boundary_tests[][2] = {
            {20735, 100},    /* near boundary (sector 0 boundary) */
            {100, 207350},   /* near +Y axis boundary */
            {0, 207360},     /* exactly on boundary */
            {50000, 50000},  /* mid-sector (should not drain) */
        };
        int n_tests = 4;

        for (int i = 0; i < n_tests; i++) {
            int64_t vx = boundary_tests[i][0];
            int64_t vy = boundary_tests[i][1];

            TWShadowCapture sc;
            tw_capture_shadow(vx, vy, &sz, &sc);

            printf("  v=(%5lld,%5lld) zone=%u slot=%u drain=%u shadow=%u node=%u\n",
                   (long long)vx, (long long)vy,
                   sc.capture.zone, sc.capture.slot,
                   sc.capture.drain, sc.shadow_zone, sc.shadow_node_id);

            if (sc.capture.drain) {
                CHECK(sc.shadow_zone == SHADOW_ZONE_A, "drain → shadow zone A");
                CHECK(shadow_zone_is_shadow(sc.shadow_node_id), "node in shadow space");
            }
        }

        ShadowZoneStats ss = shadow_stats(&sz);
        printf("  shadow: live=%u total=%u evict=%u\n",
               ss.live_count, ss.total_writes, ss.evictions);
    }

    /* ── 4. Two-zone interleaved writes ── */
    printf("--- [4] Dual zone interleave ---\n");
    {
        ShadowZone za, zb;
        shadow_zone_init(&za, SHADOW_ZONE_A);
        shadow_zone_init(&zb, SHADOW_ZONE_B);

        uint8_t data[DIAMOND_BLOCK_SIZE];

        /* Write alternating to each zone */
        for (int i = 0; i < 100; i++) {
            memset(data, (uint8_t)i, DIAMOND_BLOCK_SIZE);
            uint32_t nid;
            if (i % 2 == 0)
                shadow_write(&za, (uint64_t)i, i, BERMUDA_SHADOW_COLD,
                              data, DIAMOND_BLOCK_SIZE, &nid);
            else
                shadow_write(&zb, (uint64_t)i, i, BERMUDA_SHADOW_COLD,
                              data, DIAMOND_BLOCK_SIZE, &nid);
        }

        ShadowZoneStats sa = shadow_stats(&za);
        ShadowZoneStats sb = shadow_stats(&zb);
        CHECK(sa.live_count == 50, "zone A: 50 writes");
        CHECK(sb.live_count == 50, "zone B: 50 writes");

        /* Verify data integrity across zones */
        for (int i = 0; i < 100; i += 2) {
            const ShadowSlotMeta *ma = shadow_find_by_bond(&za, (uint64_t)i);
            CHECK(ma != NULL && ma->tick == (uint32_t)i,
                  "zone A: entry intact");
        }
        for (int i = 1; i < 100; i += 2) {
            const ShadowSlotMeta *mb = shadow_find_by_bond(&zb, (uint64_t)i);
            CHECK(mb != NULL && mb->tick == (uint32_t)i,
                  "zone B: entry intact");
        }
    }

    /* ── 5. Zero-copy data access via node_id ── */
    printf("--- [5] Zero-copy by node_id ---\n");
    {
        ShadowZone sz;
        shadow_zone_init(&sz, SHADOW_ZONE_A);

        /* Write with known pattern */
        uint8_t pattern[DIAMOND_BLOCK_SIZE];
        for (int i = 0; i < 64; i++)
            pattern[i] = (uint8_t)(i ^ 0xA5);

        uint32_t node_id;
        shadow_write(&sz, 0xC0DEDEu, 99, BERMUDA_SHADOW_COLD,
                      pattern, DIAMOND_BLOCK_SIZE, &node_id);

        /* Read back — node_id gives direct access to data location */
        CHECK(shadow_zone_is_shadow(node_id), "node_id in shadow");

        const ShadowSlotMeta *m = shadow_find_by_bond(&sz, 0xC0DEDEu);
        CHECK(m != NULL, "metadata found");
        CHECK(m->bond_key == 0xC0DEDEu, "bond_key matches");
        CHECK(m->tick == 99, "tick matches");
        CHECK(m->zone_id == SHADOW_ZONE_A, "zone matches");

        /* free and verify gone */
        CHECK(shadow_free(&sz, 0xC0DEDEu) == SHADOW_OK, "free ok");
        CHECK(shadow_find_by_bond(&sz, 0xC0DEDEu) == NULL, "gone after free");

        ShadowZoneStats ss = shadow_stats(&sz);
        CHECK(ss.live_count == 0, "empty after free");
    }

    /* ── 6. Bermuda HW verify still works ── */
    printf("--- [6] bermuda_shadow_verify ---\n");
    CHECK(bermuda_shadow_verify() == 0, "bermuda_shadow_verify");
    CHECK(shadow_verify() == 0, "shadow_verify");

    /* ── Report ── */
    printf("\n=== %s ===\n", rc == 0 ? "ALL PASS" : "SOME FAILED");
    return rc;
}

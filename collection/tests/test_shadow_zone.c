#include <stdio.h>
#include <string.h>
#include "coord_spine.h"
#include "shadow_zone.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL %s\n", msg); rc++; } \
    else { printf("  PASS %s\n", msg); } \
} while(0)

int main(void)
{
    int rc = 0;
    printf("=== Shadow Zone Test Suite ===\n\n");

    printf("Constants:\n");
    printf("  GEO_FULL=%u\n", GEO_FULL);
    printf("  SHADOW_N_SLOTS=%u\n", SHADOW_N_SLOTS);
    printf("  SHADOW_ZONE_A=%u base=%u\n", SHADOW_ZONE_A, SHADOW_NODE_BASE(SHADOW_ZONE_A));
    printf("  SHADOW_ZONE_B=%u base=%u\n", SHADOW_ZONE_B, SHADOW_NODE_BASE(SHADOW_ZONE_B));
    printf("  SHADOW_BUFFER_SIZE=%u\n", SHADOW_BUFFER_SIZE);
    printf("  shadow_total_capacity=%u\n\n", shadow_total_capacity());

    /* T1: constants sanity */
    printf("T1: Constants:\n");
    CHECK(SHADOW_ZONES == 2, "2 shadow zones");
    CHECK(SHADOW_N_SLOTS == GEO_FULL / 12, "1728 slots per zone");
    CHECK(SHADOW_NODE_BASE(SHADOW_ZONE_A) == 10 * SHADOW_N_SLOTS, "zone A base");
    CHECK(SHADOW_NODE_BASE(SHADOW_ZONE_B) == 11 * SHADOW_N_SLOTS, "zone B base");
    CHECK(SHADOW_NODE_BASE(SHADOW_ZONE_B) + SHADOW_N_SLOTS == GEO_FULL, "exact fit");

    /* T2: shadow_zone_is_shadow */
    printf("T2: is_shadow:\n");
    CHECK(shadow_zone_is_shadow(0) == 0, "node 0 not shadow");
    CHECK(shadow_zone_is_shadow(10000) == 0, "node 10000 not shadow");
    CHECK(shadow_zone_is_shadow(18000) == 1, "node 18000 is shadow (zone A)");
    CHECK(shadow_zone_is_shadow(19500) == 1, "node 19500 is shadow (zone B)");
    CHECK(shadow_zone_is_shadow(GEO_FULL - 1) == 1, "last node is shadow");

    /* T3: write/read cycle */
    printf("T3: Write/Read cycle:\n");
    {
        uint8_t data[DIAMOND_BLOCK_SIZE];
        for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 7 + 0xAB);

        ShadowZone z;
        shadow_zone_init(&z, SHADOW_ZONE_A);

        uint32_t node_id;
        CHECK(shadow_write(&z, 0xAABBCCDDu, 1, BERMUDA_SHADOW_COLD,
                            data, DIAMOND_BLOCK_SIZE, &node_id) == SHADOW_OK,
              "write ok");

        CHECK(z.live_count == 1, "live_count=1");
        CHECK(z.total_writes == 1, "total_writes=1");
        CHECK(z.evictions == 0, "no evictions");
        CHECK(shadow_zone_is_shadow(node_id) == 1, "node_id in shadow space");

        const ShadowSlotMeta *m = shadow_find_by_bond(&z, 0xAABBCCDDu);
        CHECK(m != NULL, "find by bond_key");
        CHECK(m->alive == 1, "alive=1");
        CHECK(m->bond_key == 0xAABBCCDDu, "bond_key matches");
        CHECK(m->temperature == BERMUDA_SHADOW_COLD, "temperature=COLD");
        CHECK(m->zone_id == SHADOW_ZONE_A, "zone=A");
    }

    /* T4: write multiple entries, eviction */
    printf("T4: Write multiples / eviction:\n");
    {
        ShadowZone z;
        shadow_zone_init(&z, SHADOW_ZONE_A);

        uint8_t data[DIAMOND_BLOCK_SIZE];
        memset(data, 0x42, DIAMOND_BLOCK_SIZE);

        uint32_t count = 100;
        uint32_t first_node_id = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t nid;
            shadow_write(&z, (uint64_t)i + 1, i, BERMUDA_SHADOW_COLD,
                          data, DIAMOND_BLOCK_SIZE, &nid);
            if (i == 0) first_node_id = nid;
        }

        CHECK(z.live_count == count, "live count = 100");
        CHECK(z.total_writes == count, "total_writes = 100");
        CHECK(z.evictions == 0, "no evictions yet");

        /* fill past capacity (1728) */
        for (uint32_t i = count; i < SHADOW_N_SLOTS + 10; i++) {
            uint32_t nid;
            shadow_write(&z, (uint64_t)i + 1, i, BERMUDA_SHADOW_COLD,
                          data, DIAMOND_BLOCK_SIZE, &nid);
        }

        CHECK(z.live_count == SHADOW_N_SLOTS, "live_count at capacity");
        CHECK(z.evictions == 10, "10 evictions after overflow");
        CHECK(z.total_writes == SHADOW_N_SLOTS + 10, "total writes correct");

        /* first entry should be evicted */
        CHECK(shadow_find_by_bond(&z, 1) == NULL, "first entry evicted");
    }

    /* T5: free and re-find */
    printf("T5: Free:\n");
    {
        ShadowZone z;
        shadow_zone_init(&z, SHADOW_ZONE_B);

        uint8_t data[DIAMOND_BLOCK_SIZE];
        memset(data, 0x99, DIAMOND_BLOCK_SIZE);

        uint32_t node_id;
        shadow_write(&z, 0xDEADBEEFu, 42, BERMUDA_SHADOW_COLD,
                      data, DIAMOND_BLOCK_SIZE, &node_id);

        CHECK(z.live_count == 1, "write ok");

        CHECK(shadow_free(&z, 0xDEADBEEFu) == SHADOW_OK, "free ok");
        CHECK(z.live_count == 0, "live_count=0 after free");
        CHECK(shadow_find_by_bond(&z, 0xDEADBEEFu) == NULL, "not found after free");
    }

    /* T6: write same bond_key overwrites */
    printf("T6: Overwrite:\n");
    {
        ShadowZone z;
        shadow_zone_init(&z, SHADOW_ZONE_A);

        uint8_t data[DIAMOND_BLOCK_SIZE];
        memset(data, 0, DIAMOND_BLOCK_SIZE);

        uint32_t n1, n2;
        shadow_write(&z, 0xB0B0B0B0u, 0, BERMUDA_SHADOW_COLD,
                      data, DIAMOND_BLOCK_SIZE, &n1);
        shadow_write(&z, 0xB0B0B0B0u, 1, BERMUDA_SHADOW_COLD,
                      data, DIAMOND_BLOCK_SIZE, &n2);

        const ShadowSlotMeta *m = shadow_find_by_bond(&z, 0xB0B0B0B0u);
        CHECK(m != NULL, "found after duplicate bond_key");
        CHECK(m->tick == 1, "last write wins (tick=1)");
        CHECK(n1 != n2, "different node_ids");
    }

    /* T7: shadow_verify() */
    printf("T7: shadow_verify():\n");
    int v = shadow_verify();
    CHECK(v == 0, "shadow_verify passes");

    printf("\n=== Results: %s ===\n", rc == 0 ? "ALL PASS" : "SOME FAILED");
    return rc;
}

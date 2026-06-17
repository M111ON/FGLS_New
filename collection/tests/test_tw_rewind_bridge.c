/*
 * test_tw_rewind_bridge.c — Smoke test for SID node_id ↔ geo_rewind.h bridge
 *
 * Compile:
 *   gcc -O2 -I. -Isrc -Icore/core -Igeo_jump_module/include -Itests -o build/test_tw_rewind_bridge tests/test_tw_rewind_bridge.c
 *
 * Verifies:
 *   1. tw_sid_node_to_enc / tw_enc_to_sid_walk roundtrip
 *   2. tw_node_pack_key / tw_node_unpack_key roundtrip
 *   3. tw_bridge_rewind_store / tw_bridge_rewind_find roundtrip
 *   4. tw_bridge_stats
 */

#include <stdio.h>
#include <string.h>
#include "tw_rewind_bridge.h"

#define CHK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); return 1; } \
    else { printf("  PASS: %s\n", msg); } \
} while(0)

int test_sid_enc_roundtrip(void) {
    printf("\n=== SID node_id ↔ enc roundtrip ===\n");

    CHK(tw_sid_node_to_enc(0) == GEO_WALK[0], "node_id=0 → enc = GEO_WALK[0]");
    CHK(tw_sid_node_to_enc(719) == GEO_WALK[719], "node_id=719 → enc = GEO_WALK[719]");
    CHK(tw_sid_node_to_enc(1728) == GEO_WALK[0], "node_id=1728 → enc = GEO_WALK[0] (pentagon wrap)");
    /* 20735 % 1728 = 1727, 1727 % 720 = 287 */
    CHK(tw_sid_node_to_enc(20735) == GEO_WALK[287], "node_id=20735 → enc = GEO_WALK[287]");
    CHK(tw_sid_node_to_enc(20736) == 0xFFFFFFFFu, "node_id >= GEO_FULL → 0xFFFFFFFF");

    /* Roundtrip: enc → walk_pos (batch verify all 720 walk positions) */
    uint32_t enc = tw_sid_node_to_enc(0);
    CHK(tw_enc_to_sid_walk(enc) == 0, "enc=GEO_WALK[0] → walk_pos=0");
    enc = tw_sid_node_to_enc(719);
    CHK(tw_enc_to_sid_walk(enc) == 719, "enc=GEO_WALK[719] → walk_pos=719");

    int ok = 1;
    for (uint32_t nid = 0; nid < 720; nid++) {
        uint32_t e = tw_sid_node_to_enc(nid);
        if (!tw_enc_is_valid(e)) { ok = 0; break; }
        uint16_t wp = tw_enc_to_sid_walk(e);
        if (wp != (nid % 720u)) { ok = 0; break; }
    }
    CHK(ok, "all 720 node_id → enc → walk_pos roundtrip");

    return 0;
}

int test_node_key_roundtrip(void) {
    printf("\n=== Node key pack/unpack ===\n");

    uint64_t k0 = tw_node_pack_key(0, 100, -50, 1);
    CHK(k0 != 0, "pack node_id=0 → non-zero key");

    uint32_t nid; int64_t rx, ry; uint8_t pent;
    tw_node_unpack_key(k0, &nid, &rx, &ry, &pent);
    CHK(nid == 0, "unpack node_id = 0");
    CHK(rx == 100, "unpack resid_x = 100");
    CHK(ry == -50, "unpack resid_y = -50");
    CHK(pent == 1, "unpack pentagon = 1");

    uint64_t k1 = tw_node_pack_key(1379, -200, 300, 3);
    tw_node_unpack_key(k1, &nid, &rx, &ry, &pent);
    CHK(nid == 1379, "unpack node_id = 1379");
    CHK(rx == -200, "unpack resid_x = -200");
    CHK(ry == 300, "unpack resid_y = 300");
    CHK(pent == 3, "unpack pentagon = 3");

    /* Boundary values */
    uint64_t k2 = tw_node_pack_key(20735, 8191, -8192, 12);
    tw_node_unpack_key(k2, &nid, &rx, &ry, &pent);
    CHK(nid == 20735, "unpack node_id = 20735 (max)");
    CHK(rx == 8191, "unpack resid_x = 8191 (max)");
    CHK(ry == -8192, "unpack resid_y = -8192 (min)");
    CHK(pent == 12, "unpack pentagon = 12 (max)");

    printf("  ALL node key roundtrip: PASS\n");
    return 0;
}

int test_bridge_store_find(void) {
    static RewindBuffer geo_rb;
    printf("\n=== Bridge store/find ===\n");

    TWFaceRewind tw_rb;
    tw_rewind_init(&tw_rb);

    /* Store a node_id with a known key */
    uint64_t key1 = tw_node_pack_key(42, 100, 200, 5);
    uint32_t enc1 = tw_bridge_rewind_store(&tw_rb, &geo_rb, key1, 42);
    CHK(enc1 != 0xFFFFFFFFu, "store node_id=42 returned valid enc");
    CHK(enc1 == GEO_WALK[42 % 720u], "store enc = GEO_WALK[walk_pos]");

    /* TWFaceRewind has it */
    CHK(tw_rewind_has(&tw_rb, 42), "TWFaceRewind has node_id=42");
    CHK(tw_rewind_find(&tw_rb, 42) == key1, "TWFaceRewind key matches");

    /* RewindBuffer has it */
    const TStreamChunk *p = rewind_find(&geo_rb, enc1);
    CHK(p != NULL, "RewindBuffer has enc1");

    /* Bridge find — combined result */
    TWBRewindResult r = tw_bridge_rewind_find(&tw_rb, &geo_rb, 42);
    CHK(r.tw_key == key1, "bridge find: tw_key matches");
    CHK(r.node_id == 42, "bridge find: node_id=42");
    CHK(r.has_geo == 1, "bridge find: has_geo=1");
    CHK(tw_enc_is_valid(enc1), "enc1 is valid");

    /* Store a second node */
    uint64_t key2 = tw_node_pack_key(500, -50, 75, 8);
    uint32_t enc2 = tw_bridge_rewind_store(&tw_rb, &geo_rb, key2, 500);
    CHK(enc2 != 0xFFFFFFFFu, "store node_id=500 returned valid enc");

    TWBRewindResult r2 = tw_bridge_rewind_find(&tw_rb, &geo_rb, 500);
    CHK(r2.tw_key == key2, "bridge find 2: tw_key matches");
    CHK(r2.node_id == 500, "bridge find 2: node_id=500");
    CHK(r2.has_geo == 1, "bridge find 2: has_geo=1");

    /* Lookup non-existent node */
    TWBRewindResult r3 = tw_bridge_rewind_find(&tw_rb, &geo_rb, 9999);
    CHK(r3.tw_key == 0, "bridge find miss: tw_key=0");
    CHK(r3.has_geo == 0, "bridge find miss: has_geo=0");

    /* bridge_has */
    CHK(tw_bridge_rewind_has(&tw_rb, &geo_rb, 42) == 1, "bridge_has node_id=42");
    CHK(tw_bridge_rewind_has(&tw_rb, &geo_rb, 500) == 1, "bridge_has node_id=500");
    CHK(tw_bridge_rewind_has(&tw_rb, &geo_rb, 9999) == 0, "bridge_has miss");

    printf("  ALL bridge store/find: PASS\n");
    return 0;
}

int test_bridge_stats(void) {
    printf("\n=== Bridge stats ===\n");

    TWFaceRewind tw_rb;
    tw_rewind_init(&tw_rb);
    static RewindBuffer geo_rb;
    rewind_init(&geo_rb);
    geo_rb.stored = 0;

    TWBridgeStats st;
    tw_bridge_stats(&tw_rb, &geo_rb, &st);
    CHK(st.tw_occupied == 0, "initial TW occupied = 0");
    CHK(st.geo_occupied == 0, "initial geo occupied = 0");

    /* Store a few nodes */
    for (int i = 0; i < 5; i++) {
        uint64_t key = tw_node_pack_key(i, i * 10, -i * 10, 1);
        tw_bridge_rewind_store(&tw_rb, &geo_rb, key, (uint32_t)i);
    }

    tw_bridge_stats(&tw_rb, &geo_rb, &st);
    CHK(st.tw_occupied == 5, "TW occupied = 5 after 5 stores");
    CHK(st.tw_stored >= 5, "TW stored >= 5");

    printf("  ALL bridge stats: PASS\n");
    return 0;
}

int main(void) {
    printf("=== tw_rewind_bridge.h Smoke Test ===\n");

    int fail = 0;
    fail += test_sid_enc_roundtrip();
    fail += test_node_key_roundtrip();
    fail += test_bridge_store_find();
    fail += test_bridge_stats();

    printf("\n=== %s: %d failures ===\n", fail ? "FAIL" : "PASS", fail);
    return fail;
}

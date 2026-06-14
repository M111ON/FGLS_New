/*
 * test_tw_rewind_bridge.c — Smoke test for SID↔geo_rewind.h bridge
 *
 * Compile:
 *   gcc -O2 -I. -Isrc -Icore/core -Itests -o build/test_tw_rewind_bridge tests/test_tw_rewind_bridge.c
 *
 * Verifies:
 *   1. tw_sid_tring_to_enc / tw_enc_to_sid_tring roundtrip
 *   2. tw_cap_pack_chunk / tw_chunk_unpack_cap roundtrip
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
    printf("\n=== SID tring ↔ enc roundtrip ===\n");

    CHK(tw_sid_tring_to_enc(0, 0) == GEO_WALK[0], "sid_tring=0 hex → enc = GEO_WALK[0]");
    CHK(tw_sid_tring_to_enc(719, 0) == GEO_WALK[719], "sid_tring=719 hex → enc = GEO_WALK[719]");
    CHK(tw_sid_tring_to_enc(1440, 0) == 0xFFFFFFFFu, "out of range → 0xFFFFFFFF");

    /* Tri: tring_pos = face*120 + 60 + zone*6 + slot = 60 for face=0,zone=0,slot=0,tri */
    CHK(tw_sid_tring_to_enc(60, 1) == GEO_WALK[0], "tri tring=60 → walk_pos=0");
    CHK(tw_sid_tring_to_enc(61, 1) == GEO_WALK[1], "tri tring=61 → walk_pos=1");
    CHK(tw_sid_tring_to_enc(719, 1) == GEO_WALK[659], "tri tring=719 → walk_pos=659");

    /* Roundtrip: enc → sid_tring (batch verify, single result) */
    uint32_t enc = GEO_WALK[0];
    CHK(tw_enc_to_sid_tring(enc) == 0, "enc=GEO_WALK[0] → sid_tring=0");
    enc = GEO_WALK[719];
    CHK(tw_enc_to_sid_tring(enc) == 719, "enc=GEO_WALK[719] → sid_tring=719");

    int ok = 1;
    for (uint16_t pos = 0; pos < 720; pos++) {
        uint32_t e = GEO_WALK[pos];
        if (tw_enc_to_sid_tring(e) != pos) { ok = 0; break; }
    }
    CHK(ok, "all 720 enc→sid_tring roundtrip");

    return 0;
}

int test_chunk_pack_unpack(void) {
    printf("\n=== Chunk pack/unpack ===\n");

    TWFaceCapture cap;
    memset(&cap, 0, sizeof(cap));
    cap.face = 3;
    cap.zone = 5;
    cap.slot = 2;
    cap.is_tri = 1;
    cap.resid_x = 12345;
    cap.resid_y = -6789;
    cap.tring_pos = 3*120 + 1*60 + 5*6 + 2;

    TStreamChunk ch = tw_cap_pack_chunk(&cap);
    CHK(ch.size == 8, "chunk size = 8");

    uint64_t key;
    TWFaceCapture cap2;
    memset(&cap2, 0xFF, sizeof(cap2));
    CHK(tw_chunk_unpack_cap(&ch, &cap2, &key) == 1, "unpack success");
    CHK(cap2.face == 3, "face preserved");
    CHK(cap2.zone == 5, "zone preserved");
    CHK(cap2.slot == 2, "slot preserved");
    CHK(cap2.is_tri == 1, "is_tri preserved");

    TStreamChunk empty;
    memset(&empty, 0, sizeof(empty));
    CHK(tw_chunk_unpack_cap(&empty, NULL, NULL) == 0, "empty chunk → fail");

    printf("  ALL chunk pack/unpack: PASS\n");
    return 0;
}

int test_bridge_store_find(void) {
    static RewindBuffer geo_rb;   /* static (BSS, not stack — RewindBuffer is ~4MB) */
    printf("\n=== Bridge store/find ===\n");

    TWFaceRewind tw_rb;
    tw_rewind_init(&tw_rb);

    /* Hex capture: should store in both */
    TWFaceCapture hex_cap;
    memset(&hex_cap, 0, sizeof(hex_cap));
    hex_cap.face = 0;
    hex_cap.zone = 1;
    hex_cap.slot = 3;
    hex_cap.is_tri = 0;
    hex_cap.resid_x = 100;
    hex_cap.resid_y = 200;
    hex_cap.tring_pos = 0*120 + 0*60 + 1*6 + 3;

    uint16_t hex_tring = hex_cap.tring_pos;
    CHK(hex_tring == 9, "hex cap tring_pos = 9");

    uint32_t enc = tw_bridge_rewind_store(&tw_rb, &geo_rb, &hex_cap);
    CHK(enc != 0xFFFFFFFFu, "hex store returned valid enc");
    CHK(enc == GEO_WALK[9], "hex store enc = GEO_WALK[9]");

    /* Verify TWFaceRewind has it */
    CHK(tw_rewind_has(&tw_rb, hex_tring), "TWFaceRewind has hex cap");

    /* Verify RewindBuffer has it */
    const TStreamChunk *p = rewind_find(&geo_rb, enc);
    CHK(p != NULL, "RewindBuffer has hex cap");
    CHK(p->size == 8, "chunk size = 8");

    /* Check bridge find */
    TWBRewindResult r = tw_bridge_rewind_find(&tw_rb, &geo_rb, hex_tring);
    CHK(r.tw_key != 0, "bridge find: tw_key non-zero");
    CHK(r.has_geo == 1, "bridge find: has_geo=1");
    CHK(r.cap.zone == 1, "bridge find: zone=1");
    CHK(r.cap.slot == 3, "bridge find: slot=3");

    /* Tri capture: store in TWFaceRewind only */
    TWFaceCapture tri_cap;
    memset(&tri_cap, 0, sizeof(tri_cap));
    tri_cap.face = 0;
    tri_cap.zone = 1;
    tri_cap.slot = 3;
    tri_cap.is_tri = 1;
    tri_cap.resid_x = 50;
    tri_cap.resid_y = -50;
    tri_cap.tring_pos = 0*120 + 1*60 + 1*6 + 3;

    uint16_t tri_tring = tri_cap.tring_pos;
    CHK(tri_tring == 69, "tri cap tring_pos = 69");

    uint32_t enc2 = tw_bridge_rewind_store(&tw_rb, &geo_rb, &tri_cap);
    CHK(enc2 == 0xFFFFFFFFu, "tri store skipped geo (returns 0xFFFFFFFF)");

    /* TWFaceRewind has tri */
    CHK(tw_rewind_has(&tw_rb, tri_tring), "TWFaceRewind has tri cap");

    /* RewindBuffer should NOT have tri */
    TWBRewindResult r2 = tw_bridge_rewind_find(&tw_rb, &geo_rb, tri_tring);
    CHK(r2.tw_key != 0, "bridge find tri: tw_key non-zero");
    CHK(r2.has_geo == 0, "bridge find tri: has_geo=0");
    CHK(r2.cap.is_tri == 1, "bridge find tri: is_tri=1");

    /* Check has() */
    CHK(tw_bridge_rewind_has(&tw_rb, &geo_rb, hex_tring, 0) == 1, "bridge_has hex");
    CHK(tw_bridge_rewind_has(&tw_rb, &geo_rb, tri_tring, 1) == 1, "bridge_has tri");

    printf("  ALL bridge store/find: PASS\n");
    return 0;
}

int test_bridge_stats(void) {
    printf("\n=== Bridge stats ===\n");

    TWFaceRewind tw_rb;
    tw_rewind_init(&tw_rb);
    static RewindBuffer geo_rb;   /* ~4MB struct — must be static/BSS, not stack */
    rewind_init(&geo_rb);
    geo_rb.stored = 0;

    TWBridgeStats st;
    tw_bridge_stats(&tw_rb, &geo_rb, &st);
    CHK(st.tw_occupied == 0, "initial TW occupied = 0");
    CHK(st.geo_occupied == 0, "initial geo occupied = 0");

    /* Store a few caps */
    TWFaceCapture cap;
    for (int i = 0; i < 5; i++) {
        memset(&cap, 0, sizeof(cap));
        cap.face = 0;
        cap.zone = 0;
        cap.slot = i;
        cap.tring_pos = i;
        tw_bridge_rewind_store(&tw_rb, &geo_rb, &cap);
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
    fail += test_chunk_pack_unpack();
    fail += test_bridge_store_find();
    fail += test_bridge_stats();

    printf("\n=== %s: %d failures ===\n", fail ? "FAIL" : "PASS", fail);
    return fail;
}

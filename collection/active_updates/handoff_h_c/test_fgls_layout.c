/*
 * test_fgls_layout.c — FrustumBlock layout verification
 *
 * Tests:
 *   L01: compile-time sizes (header=17, meta=1440, block=4896)
 *   L02: field offsets inside FrustumMeta
 *   L03: frustum_header() cast → correct address inside reserved[]
 *   L04: header magic write/read via overlay (no memcpy)
 *   L05: header fields don't overlap data or meta boundary
 *   L06: drain_state[0..11] addressable, no out-of-bounds
 *   L07: shadow_state[0..27] addressable, no out-of-bounds
 *   L08: merkle_roots[drain][byte] addressable
 *   L09: write to data[0] and data[3455] — no bleed into meta
 *   L10: write to reserved[17..575] — no bleed into header overlay
 *
 * Compile:
 *   gcc -O2 -Wall -o test_fgls_layout test_fgls_layout.c && ./test_fgls_layout
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "frustum_layout_v2.h"

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s\n", msg); _fail++; } \
} while(0)

/* ── L01: compile-time sizes ────────────────────────────── */
static void l01(void) {
    printf("\nL01: compile-time sizes\n");
    CHECK(sizeof(FrustumHeader) == 17,   "FrustumHeader == 17B");
    CHECK(sizeof(FrustumMeta)   == 1440, "FrustumMeta   == 1440B");
    CHECK(sizeof(FrustumBlock)  == 4896, "FrustumBlock  == 4896B");
}

/* ── L02: field offsets inside FrustumMeta ──────────────── */
static void l02(void) {
    printf("\nL02: FrustumMeta field offsets\n");
    CHECK(offsetof(FrustumMeta, letter_map)   ==   0, "letter_map  @ meta+0");
    CHECK(offsetof(FrustumMeta, slope_map)    == 144, "slope_map   @ meta+144");
    CHECK(offsetof(FrustumMeta, drain_state)  == 432, "drain_state @ meta+432");
    CHECK(offsetof(FrustumMeta, drain_ctrl)   == 444, "drain_ctrl  @ meta+444");
    CHECK(offsetof(FrustumMeta, shadow_state) == 448, "shadow_state@ meta+448");
    CHECK(offsetof(FrustumMeta, shadow_ctrl)  == 476, "shadow_ctrl @ meta+476");
    CHECK(offsetof(FrustumMeta, merkle_roots) == 480, "merkle_roots@ meta+480");
    CHECK(offsetof(FrustumMeta, reserved)     == 864, "reserved    @ meta+864");
}

/* ── L03: frustum_header() points into reserved[0] ─────── */
static void l03(void) {
    printf("\nL03: frustum_header() address\n");
    FrustumBlock b;
    memset(&b, 0, sizeof(b));

    FrustumHeader *hdr = frustum_header(&b);
    void *expected     = &b.meta.reserved[0];

    CHECK((void*)hdr == expected, "frustum_header() == &reserved[0]");

    /* absolute offset from block start = 3456 + 864 = 4320 */
    ptrdiff_t off = (uint8_t*)hdr - (uint8_t*)&b;
    CHECK(off == 4320, "header absolute offset == 4320");
}

/* ── L04: header magic write/read via overlay ───────────── */
static void l04(void) {
    printf("\nL04: header magic write/read via overlay\n");
    FrustumBlock b;
    memset(&b, 0, sizeof(b));

    FrustumHeader *hdr = frustum_header(&b);
    hdr->magic[0] = 'F'; hdr->magic[1] = 'G';
    hdr->magic[2] = 'L'; hdr->magic[3] = 'S';
    hdr->version        = 1;
    hdr->rotation_state = 3;
    hdr->block_id       = 0xDEADBEEFCAFEBABEull;

    /* re-read via cast */
    const FrustumHeader *r = frustum_header_c(&b);
    CHECK(r->magic[0] == 'F' && r->magic[3] == 'S', "magic FGLS readable");
    CHECK(r->version        == 1,                    "version=1 readable");
    CHECK(r->rotation_state == 3,                    "rotation_state=3 readable");
    CHECK(r->block_id == 0xDEADBEEFCAFEBABEull,      "block_id 8B readable");

    /* also verify raw bytes in reserved[] */
    CHECK(b.meta.reserved[0] == 'F', "reserved[0]='F' via direct access");
    CHECK(b.meta.reserved[4] == 1,   "reserved[4]=version via direct access");
}

/* ── L05: header does not overlap data or meta zones ─────── */
static void l05(void) {
    printf("\nL05: zone boundary — no overlap\n");
    FrustumBlock b;
    memset(&b, 0, sizeof(b));

    uint8_t *base  = (uint8_t*)&b;
    uint8_t *hdr   = (uint8_t*)frustum_header(&b);
    uint8_t *data  = base;                        /* offset 0    */
    uint8_t *meta  = base + FGLS_DATA_BYTES;      /* offset 3456 */

    /* header must be inside meta zone */
    CHECK(hdr >= meta,                   "header start >= meta start");
    CHECK(hdr + 17 <= base + FGLS_TOTAL_BYTES, "header end <= block end");

    /* header must not touch data zone */
    CHECK(hdr >= data + FGLS_DATA_BYTES, "header does not overlap data zone");
    (void)meta;
}

/* ── L06: drain_state addressable ───────────────────────── */
static void l06(void) {
    printf("\nL06: drain_state[0..11] write/read\n");
    FrustumBlock b;
    memset(&b, 0, sizeof(b));

    for (uint8_t i = 0; i < FGLS_DRAIN_COUNT; i++)
        b.meta.drain_state[i] = i | 0x80u;  /* set high bit as marker */

    int ok = 1;
    for (uint8_t i = 0; i < FGLS_DRAIN_COUNT; i++)
        if (b.meta.drain_state[i] != (i | 0x80u)) { ok = 0; break; }

    CHECK(ok, "all 12 drain_state slots write/read correctly");
    CHECK(FGLS_DRAIN_COUNT == 12, "FGLS_DRAIN_COUNT == 12");
}

/* ── L07: shadow_state addressable ─────────────────────── */
static void l07(void) {
    printf("\nL07: shadow_state[0..27] write/read\n");
    FrustumBlock b;
    memset(&b, 0, sizeof(b));

    for (uint8_t i = 0; i < FGLS_SHADOW_COUNT; i++)
        b.meta.shadow_state[i] = (uint8_t)(i ^ 0x55u);

    int ok = 1;
    for (uint8_t i = 0; i < FGLS_SHADOW_COUNT; i++)
        if (b.meta.shadow_state[i] != (uint8_t)(i ^ 0x55u)) { ok = 0; break; }

    CHECK(ok, "all 28 shadow_state slots write/read correctly");
    CHECK(FGLS_SHADOW_COUNT == 28, "FGLS_SHADOW_COUNT == 28");
}

/* ── L08: merkle_roots addressable ─────────────────────── */
static void l08(void) {
    printf("\nL08: merkle_roots[drain][byte] write/read\n");
    FrustumBlock b;
    memset(&b, 0, sizeof(b));

    /* write pattern: drain_id in first byte, ~drain_id in last byte */
    for (uint8_t d = 0; d < FGLS_DRAIN_COUNT; d++) {
        b.meta.merkle_roots[d][0]                    = d;
        b.meta.merkle_roots[d][FGLS_MERKLE_BYTES-1]  = (uint8_t)~d;
    }

    int ok = 1;
    for (uint8_t d = 0; d < FGLS_DRAIN_COUNT; d++) {
        if (b.meta.merkle_roots[d][0] != d) { ok = 0; break; }
        if (b.meta.merkle_roots[d][FGLS_MERKLE_BYTES-1] != (uint8_t)~d)
            { ok = 0; break; }
    }
    CHECK(ok, "12×32 merkle_roots write/read correctly");
}

/* ── L09: data zone boundary — no bleed into meta ────────── */
static void l09(void) {
    printf("\nL09: data zone boundary\n");
    FrustumBlock b;
    memset(&b, 0, sizeof(b));

    b.data[0]                    = 0xAA;
    b.data[FGLS_DATA_BYTES - 1]  = 0xBB;

    /* meta must still be zeroed */
    CHECK(b.meta.letter_map[0]  == 0, "data write doesn't bleed to meta start");
    CHECK(b.data[0]             == 0xAA, "data[0] readable");
    CHECK(b.data[FGLS_DATA_BYTES-1] == 0xBB, "data[last] readable");

    /* offsetof data == 0 */
    CHECK(offsetof(FrustumBlock, data) == 0,    "data at offset 0");
    CHECK(offsetof(FrustumBlock, meta) == 3456, "meta at offset 3456");
}

/* ── L10: reserved[17..] doesn't disturb header overlay ─── */
static void l10(void) {
    printf("\nL10: reserved[17..575] safe beyond header\n");
    FrustumBlock b;
    memset(&b, 0, sizeof(b));

    FrustumHeader *hdr = frustum_header(&b);
    hdr->block_id = 0x1234567890ABCDEFull;

    /* write to reserved beyond header (byte 17 onward) */
    for (int i = 17; i < 576; i++)
        b.meta.reserved[i] = (uint8_t)(i & 0xFF);

    /* header must be unchanged */
    CHECK(hdr->block_id == 0x1234567890ABCDEFull,
          "header overlay intact after reserved[17+] write");

    /* spot check reserved[17] */
    CHECK(b.meta.reserved[17] == (uint8_t)(17 & 0xFF),
          "reserved[17] writable independently of header");
}

/* ════════════════════════════════════════════════════════ */

int main(void) {
    printf("=== FrustumBlock Layout Verification ===\n");
    printf("4896B = 3456(data) + 1440(meta), header overlay @ reserved[0..16]\n");

    l01(); l02(); l03(); l04(); l05();
    l06(); l07(); l08(); l09(); l10();

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    if (_fail == 0)
        printf("\n✅ Layout verified — safe to wire drain_gap (Task #1)\n");
    else
        printf("\n❌ Fix layout before proceeding\n");
    return _fail ? 1 : 0;
}
/*
 * test_multichunk_rewind.c — multi-chunk per spoke + rewind replay
 * ═════════════════════════════════════════════════════════════════
 * Tests:
 *   MC01: spoke opens with LCGW_GROUND_SLOTS chunks (not 1)
 *   MC02: 16 writes fill all chunk slots, each slot has distinct raw
 *   MC03: write 17+ → ring wraps, ci=0 overwritten with new seed
 *   MC04: all 16 chunk slots valid after 16 writes
 *   MC05: chunk at ci matches build_from_seed(seeds[ci], base_wc+ci)
 *
 *   RW01: ghost + rewind restores lane.ghosted=0
 *   RW02: rewind restores read path (lcgw_read_payload returns non-NULL)
 *   RW03: rewound raw matches original raw byte-exact
 *   RW04: rewind removes entry from RewindBuffer
 *   RW05: rewind on non-ghosted spoke returns -1
 *   RW06: rewind on unknown gslot (not in rb) returns -1
 *   RW07: double ghost → single rewind restores; second rewind fails
 *   RW08: write after rewind works normally (spoke live again)
 *   RW09: rewind all 6 spokes after full ghost cycle
 *
 * Compile:
 *   gcc -O2 -Wall -I. -o test_multichunk_rewind \
 *       test/test_multichunk_rewind.c && ./test_multichunk_rewind
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* ── stubs ───────────────────────────────────────────────────── */
#define GEO_SLOTS            576u
#define GEO_BLOCK_BOUNDARY   288u
#define GEOMATRIX_PATHS       18
typedef struct { uint8_t phase; uint64_t sig; uint16_t hpos; uint16_t idx; uint8_t bit; } GeoPacket;
typedef struct { uint32_t x; } GeomatrixStatsV3;
typedef struct {
    uint32_t stamp_hash; uint32_t spatial_xor; uint32_t window_id;
    uint32_t circuit_fired; uint16_t tring_start; uint16_t tring_end;
    uint16_t tring_span; uint16_t top_gaps[4];
} GBBlueprint;
typedef struct { int blueprint_ready; GBBlueprint bp; } GBPResult;
typedef struct { GBPResult gpr; } TGWResult;
typedef struct { uint64_t *_bundle; } TGWCtx;
static inline uint64_t geo_compute_sig64(const uint64_t *b, uint8_t p) { (void)b; return p; }
static inline bool geomatrix_batch_verdict(GeoPacket *b, const uint64_t *u, GeomatrixStatsV3 *s)
    { (void)b;(void)u;(void)s; return true; }
typedef struct { uint32_t x; } FtsTwinStore;
typedef struct { uint32_t writes; } FtsTwinStats;
typedef struct { uint64_t mr,sh,sl,of,hc,sg; } DodecaEntry;
static inline void fts_init(FtsTwinStore *f, uint64_t s) { (void)f;(void)s; }
static inline void fts_write(FtsTwinStore *f, uint64_t a, uint64_t v, const DodecaEntry *e)
    { (void)f;(void)a;(void)v;(void)e; }
static inline FtsTwinStats fts_stats(const FtsTwinStore *f) { (void)f; FtsTwinStats s={0}; return s; }
typedef struct { uint32_t count; } PayloadStore;
static inline void pl_init(PayloadStore *p) { p->count=0; }
static inline void pl_write(PayloadStore *p, uint64_t a, uint64_t v) { (void)a;(void)v; p->count++; }
#define LC_GATE_GROUND 3
typedef struct { uint32_t gate_counts[4]; } LCTwinGateCtx;
static inline void lc_twin_gate_init(LCTwinGateCtx *c) { memset(c,0,sizeof(*c)); }
static inline TGWResult tgw_write(TGWCtx *c, uint64_t a, uint64_t v, uint8_t h)
    { (void)c;(void)a;(void)v;(void)h; TGWResult r={0}; return r; }
static inline void tgw_batch(TGWCtx *c, const uint64_t *a, const uint64_t *v, uint32_t n, int f)
    { (void)c;(void)a;(void)v;(void)n;(void)f; }
static inline void _null_flush(const uint64_t *a, const uint64_t *v, uint32_t n, void *ctx)
    { (void)a;(void)v;(void)n;(void)ctx; }

#include "geo_addr_net.h"
#include "lc_hdr.h"
#include "tgw_lc_bridge_p6.h"
#include "tgw_dispatch_v2.h"
#include "tgw_ground_lcgw.h"

/* ── harness ─────────────────────────────────────────────────── */
static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s\n", msg); _fail++; } \
} while(0)
#define INFO(...) printf("  INFO  " __VA_ARGS__)

/* spoke 0 base addr (polarity=1): enc=60 → spoke=0 */
#define SP0_ADDR  60u
#define SP1_ADDR  180u
#define SP2_ADDR  300u

/* ════════════════════════════ MULTI-CHUNK ════════════════════ */

static void mc01(void) {
    printf("\nMC01: spoke opens with LCGW_GROUND_SLOTS=%u chunks\n",
           LCGW_GROUND_SLOTS);
    lcgw_reset(); geo_addr_net_init();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    lcgw_ground_write(&gs, SP0_ADDR, 0xAAu);   /* triggers lazy open */

    uint8_t spoke = geo_net_encode(SP0_ADDR).spoke;
    int gslot = gs.lanes[spoke].gslot;
    uint32_t chunk_count = lcgw_files[gslot].chunk_count;

    INFO("spoke=%u  gslot=%d  chunk_count=%u  (expect %u)\n",
         spoke, gslot, chunk_count, LCGW_GROUND_SLOTS);

    CHECK(gslot >= 0,                         "lane opened (gslot≥0)");
    CHECK(chunk_count == LCGW_GROUND_SLOTS,   "chunk_count == LCGW_GROUND_SLOTS");
}

static void mc02(void) {
    printf("\nMC02: %u writes fill all chunk slots, each with distinct raw\n",
           LCGW_GROUND_SLOTS);
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* write LCGW_GROUND_SLOTS times with distinct vals */
    for (uint32_t i = 0; i < LCGW_GROUND_SLOTS; i++)
        lcgw_ground_write(&gs, SP0_ADDR, (uint64_t)(i + 1u) * 0x100u);

    uint8_t spoke = geo_net_encode(SP0_ADDR).spoke;
    LCGWSpokeLane *lane = &gs.lanes[spoke];
    LCGWFile *gf = &lcgw_files[lane->gslot];

    /* all chunks must be valid */
    int all_valid = 1;
    for (uint32_t ci = 0; ci < LCGW_GROUND_SLOTS; ci++)
        if (!gf->chunks[ci].valid) { all_valid = 0; break; }

    /* all chunk raws must be distinct */
    int all_distinct = 1;
    for (uint32_t i = 0; i < LCGW_GROUND_SLOTS && all_distinct; i++)
        for (uint32_t j = i + 1; j < LCGW_GROUND_SLOTS; j++)
            if (memcmp(gf->chunks[i].raw, gf->chunks[j].raw, LCGW_RAW_BYTES) == 0) {
                INFO("chunks[%u] == chunks[%u] COLLISION\n", i, j);
                all_distinct = 0; break;
            }

    INFO("write_count=%u  chunk_cursor=%u  all_valid=%d  all_distinct=%d\n",
         lane->write_count, lane->chunk_cursor, all_valid, all_distinct);

    CHECK(lane->write_count == LCGW_GROUND_SLOTS, "write_count == 16");
    CHECK(all_valid,   "all 16 chunk slots valid");
    CHECK(all_distinct,"all 16 chunk raws distinct");
}

static void mc03(void) {
    printf("\nMC03: write 17+ → ring wraps, ci=0 overwritten\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* fill all 16 slots */
    for (uint32_t i = 0; i < LCGW_GROUND_SLOTS; i++)
        lcgw_ground_write(&gs, SP0_ADDR, (uint64_t)i);

    uint8_t spoke = geo_net_encode(SP0_ADDR).spoke;
    LCGWSpokeLane *lane = &gs.lanes[spoke];
    LCGWFile *gf = &lcgw_files[lane->gslot];

    /* snapshot chunk[0] raw before wrap */
    uint8_t raw_before[LCGW_RAW_BYTES];
    memcpy(raw_before, gf->chunks[0].raw, LCGW_RAW_BYTES);
    uint64_t seed_before = lane->seeds[0];

    /* write one more (ci wraps to 0) */
    lcgw_ground_write(&gs, SP0_ADDR, 0xDEADu);

    int raw_changed  = (memcmp(gf->chunks[0].raw, raw_before, LCGW_RAW_BYTES) != 0);
    int seed_changed = (lane->seeds[0] != seed_before);

    INFO("chunk_cursor=%u  raw_changed=%d  seed_changed=%d\n",
         lane->chunk_cursor, raw_changed, seed_changed);

    CHECK(lane->chunk_cursor == 1u, "cursor advanced to 1 after wrap-write");
    CHECK(raw_changed,  "chunk[0].raw overwritten on ring wrap");
    CHECK(seed_changed, "seeds[0] updated on ring wrap");
}

static void mc04(void) {
    printf("\nMC04: all %u chunk slots valid after %u writes\n",
           LCGW_GROUND_SLOTS, LCGW_GROUND_SLOTS);
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    for (uint32_t i = 0; i < LCGW_GROUND_SLOTS; i++)
        lcgw_ground_write(&gs, SP1_ADDR, (uint64_t)i * 13u + 7u);

    uint8_t spoke = geo_net_encode(SP1_ADDR).spoke;
    LCGWFile *gf = &lcgw_files[gs.lanes[spoke].gslot];

    uint32_t valid_count = 0;
    for (uint32_t ci = 0; ci < LCGW_GROUND_SLOTS; ci++)
        if (gf->chunks[ci].valid) valid_count++;

    INFO("valid_count=%u / %u\n", valid_count, LCGW_GROUND_SLOTS);
    CHECK(valid_count == LCGW_GROUND_SLOTS, "all 16 chunks valid");
}

static void mc05(void) {
    printf("\nMC05: each chunk[ci] matches build_from_seed(seeds[ci], base_wc+ci)\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* write exactly LCGW_GROUND_SLOTS times */
    for (uint32_t i = 0; i < LCGW_GROUND_SLOTS; i++)
        lcgw_ground_write(&gs, SP2_ADDR, (uint64_t)i * 0x55u);

    uint8_t spoke = geo_net_encode(SP2_ADDR).spoke;
    LCGWSpokeLane *lane  = &gs.lanes[spoke];
    LCGWFile      *gf    = &lcgw_files[lane->gslot];

    /* base_wc: chunk ci was built when write_count == ci (0-indexed) */
    int all_ok = 1;
    for (uint32_t ci = 0; ci < LCGW_GROUND_SLOTS; ci++) {
        uint8_t expected[LCGW_RAW_BYTES];
        lcgw_build_from_seed(expected, lane->seeds[ci], ci);
        if (memcmp(gf->chunks[ci].raw, expected, LCGW_RAW_BYTES) != 0) {
            INFO("chunk[%u] mismatch\n", ci);
            all_ok = 0;
        }
    }
    INFO("spoke=%u  all_chunks_match=%d\n", spoke, all_ok);
    CHECK(all_ok, "all 16 chunks match build_from_seed(seeds[ci], ci)");
}

/* ════════════════════════════ REWIND ════════════════════════ */

static void rw01(void) {
    printf("\nRW01: ghost + rewind restores lane.ghosted=0\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    lcgw_ground_write(&gs, SP0_ADDR, 0x11u);
    uint8_t spoke = geo_net_encode(SP0_ADDR).spoke;

    lcgw_ground_delete(&gs, spoke);
    CHECK(gs.lanes[spoke].ghosted == 1u, "ghosted after delete");

    int r = lcgw_ground_rewind(&gs, spoke);
    INFO("rewind return=%d  ghosted=%u  total_ghosted=%u\n",
         r, gs.lanes[spoke].ghosted, gs.total_ghosted);

    CHECK(r == 0,                          "rewind returns 0 (ok)");
    CHECK(gs.lanes[spoke].ghosted == 0u,   "lane.ghosted=0 after rewind");
    CHECK(gs.total_ghosted == 0u,          "total_ghosted decremented");
}

static void rw02(void) {
    printf("\nRW02: rewind restores read path (read_payload non-NULL)\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    lcgw_ground_write(&gs, SP0_ADDR, 0x22u);
    uint8_t spoke = geo_net_encode(SP0_ADDR).spoke;
    int gslot = gs.lanes[spoke].gslot;

    lcgw_ground_delete(&gs, spoke);

    LCPalette pal = {0};
    const uint8_t *p_before = lcgw_read_payload(gslot, 0u, &pal);
    CHECK(p_before == NULL, "payload=NULL after ghost");

    lcgw_ground_rewind(&gs, spoke);

    const uint8_t *p_after = lcgw_read_payload(gslot, 0u, &pal);
    INFO("payload after rewind: %s\n", p_after ? "non-NULL (ok)" : "NULL (bad)");
    CHECK(p_after != NULL, "payload non-NULL after rewind (path restored)");
}

static void rw03(void) {
    printf("\nRW03: rewound raw matches original raw byte-exact\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* write a few chunks */
    for (uint32_t i = 0; i < 4; i++)
        lcgw_ground_write(&gs, SP1_ADDR, (uint64_t)i * 0x77u);

    uint8_t spoke = geo_net_encode(SP1_ADDR).spoke;
    LCGWFile *gf = &lcgw_files[gs.lanes[spoke].gslot];

    /* snapshot all 4 written chunks */
    uint8_t snap[4][LCGW_RAW_BYTES];
    for (uint32_t ci = 0; ci < 4; ci++)
        memcpy(snap[ci], gf->chunks[ci].raw, LCGW_RAW_BYTES);

    lcgw_ground_delete(&gs, spoke);
    lcgw_ground_rewind(&gs, spoke);

    int all_match = 1;
    for (uint32_t ci = 0; ci < 4; ci++) {
        if (memcmp(gf->chunks[ci].raw, snap[ci], LCGW_RAW_BYTES) != 0) {
            INFO("chunk[%u] mismatch after rewind\n", ci);
            all_match = 0;
        }
    }
    INFO("all 4 chunks match after rewind: %d\n", all_match);
    CHECK(all_match, "rewound chunks byte-exact match original");
}

static void rw04(void) {
    printf("\nRW04: rewind removes entry from RewindBuffer\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    lcgw_ground_write(&gs, SP0_ADDR, 0x33u);
    uint8_t spoke = geo_net_encode(SP0_ADDR).spoke;

    lcgw_ground_delete(&gs, spoke);
    uint8_t rb_count_after_delete = gs.rb.count;

    lcgw_ground_rewind(&gs, spoke);

    INFO("rb.count: delete=%u  after_rewind=%u\n",
         rb_count_after_delete, gs.rb.count);
    CHECK(rb_count_after_delete == 1u,      "rb.count=1 after delete");
    CHECK(gs.rb.count == 0u,                "rb.count=0 after rewind (entry removed)");
}

static void rw05(void) {
    printf("\nRW05: rewind on non-ghosted spoke returns -1\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    lcgw_ground_write(&gs, SP0_ADDR, 0x44u);
    uint8_t spoke = geo_net_encode(SP0_ADDR).spoke;

    int r = lcgw_ground_rewind(&gs, spoke);   /* not ghosted */
    INFO("rewind on live spoke: r=%d\n", r);
    CHECK(r == -1, "rewind on non-ghosted spoke returns -1");
}

static void rw06(void) {
    printf("\nRW06: rewind with empty RewindBuffer returns -1\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    lcgw_ground_write(&gs, SP0_ADDR, 0x55u);
    uint8_t spoke = geo_net_encode(SP0_ADDR).spoke;

    /* manually ghost without going through lcgw_ground_delete
     * so rb stays empty */
    gs.lanes[spoke].ghosted = 1;
    lcgw_files[gs.lanes[spoke].gslot].chunks[0].ghosted = 1;
    gs.total_ghosted++;

    int r = lcgw_ground_rewind(&gs, spoke);
    INFO("rewind with no rb entry: r=%d\n", r);
    CHECK(r == -1, "rewind with gslot not in rb returns -1");
}

static void rw07(void) {
    printf("\nRW07: ghost → rewind → ghost again → rewind again\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    lcgw_ground_write(&gs, SP2_ADDR, 0x66u);
    uint8_t spoke = geo_net_encode(SP2_ADDR).spoke;

    /* cycle 1 */
    lcgw_ground_delete(&gs, spoke);
    int r1 = lcgw_ground_rewind(&gs, spoke);
    CHECK(r1 == 0,                        "1st rewind ok");
    CHECK(gs.lanes[spoke].ghosted == 0u,  "lane live after 1st rewind");

    /* cycle 2: write more, then ghost again */
    lcgw_ground_write(&gs, SP2_ADDR, 0x77u);
    lcgw_ground_delete(&gs, spoke);
    int r2 = lcgw_ground_rewind(&gs, spoke);
    INFO("2nd delete+rewind: r2=%d  ghosted=%u\n", r2, gs.lanes[spoke].ghosted);
    CHECK(r2 == 0,                        "2nd rewind ok");
    CHECK(gs.lanes[spoke].ghosted == 0u,  "lane live after 2nd rewind");
}

static void rw08(void) {
    printf("\nRW08: write after rewind works normally\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    lcgw_ground_write(&gs, SP0_ADDR, 0x88u);
    uint8_t spoke = geo_net_encode(SP0_ADDR).spoke;
    uint32_t wc_before = gs.lanes[spoke].write_count;

    lcgw_ground_delete(&gs, spoke);
    lcgw_ground_rewind(&gs, spoke);

    /* write after rewind */
    lcgw_ground_write(&gs, SP0_ADDR, 0x99u);
    uint32_t wc_after = gs.lanes[spoke].write_count;

    INFO("write_count: before=%u  after_rewind_write=%u\n", wc_before, wc_after);
    CHECK(wc_after == wc_before + 1u, "write_count incremented after rewind-then-write");
    CHECK(gs.lanes[spoke].ghosted == 0u, "lane still live after write");
}

static void rw09(void) {
    printf("\nRW09: rewind all 6 spokes after full ghost cycle\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* one write per spoke */
    uint64_t spoke_addrs[6];
    for (uint32_t sp = 0; sp < 6; sp++) {
        spoke_addrs[sp] = (uint64_t)(sp * 120u + 60u);
        lcgw_ground_write(&gs, spoke_addrs[sp], (uint64_t)sp * 0xABu);
    }

    /* snapshot all chunk[0] raws */
    uint8_t snap[6][LCGW_RAW_BYTES];
    for (uint32_t sp = 0; sp < 6; sp++) {
        LCGWFile *gf = &lcgw_files[gs.lanes[sp].gslot];
        memcpy(snap[sp], gf->chunks[0].raw, LCGW_RAW_BYTES);
    }

    /* ghost all */
    for (uint32_t sp = 0; sp < 6; sp++)
        lcgw_ground_delete(&gs, sp);

    CHECK(gs.total_ghosted == 6u, "all 6 ghosted");
    CHECK(gs.rb.count == 6u,      "rb.count == 6");

    /* rewind all */
    int all_ok = 1;
    for (uint32_t sp = 0; sp < 6; sp++) {
        int r = lcgw_ground_rewind(&gs, (uint8_t)sp);
        if (r != 0) { INFO("rewind spoke[%u] failed\n", sp); all_ok = 0; }
    }
    CHECK(all_ok,              "all 6 spokes rewound ok");
    CHECK(gs.total_ghosted == 0u, "total_ghosted=0 after all rewinds");
    CHECK(gs.rb.count == 0u,      "rb empty after all rewinds");

    /* verify raw byte-exact after rewind */
    int raws_ok = 1;
    for (uint32_t sp = 0; sp < 6; sp++) {
        LCGWFile *gf = &lcgw_files[gs.lanes[sp].gslot];
        if (memcmp(gf->chunks[0].raw, snap[sp], LCGW_RAW_BYTES) != 0) {
            INFO("spoke[%u] raw mismatch after rewind\n", sp);
            raws_ok = 0;
        }
    }
    INFO("all 6 spoke raws match after rewind: %d\n", raws_ok);
    CHECK(raws_ok, "all 6 spoke raws byte-exact after full ghost+rewind cycle");
}

/* ══════════════════════════════════════════════════════════════ */

int main(void) {
    geo_addr_net_init();

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  test_multichunk_rewind.c                            ║\n");
    printf("║  multi-chunk per spoke + rewind replay               ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    printf("\n── Multi-Chunk ────────────────────────────────────────\n");
    mc01(); mc02(); mc03(); mc04(); mc05();

    printf("\n── Rewind Replay ──────────────────────────────────────\n");
    rw01(); rw02(); rw03(); rw04(); rw05();
    rw06(); rw07(); rw08(); rw09();

    printf("\n══════════════════════════════════════════════════════\n");
    printf("=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    if (_fail == 0)
        printf("\n✓ multi-chunk + rewind verified\n"
               "  LCGW_GROUND_SLOTS=%u chunks/spoke, ghost=reversible\n",
               LCGW_GROUND_SLOTS);
    return (_fail > 0) ? 1 : 0;
}

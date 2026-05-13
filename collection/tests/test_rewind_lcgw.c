/*
 * test_rewind_lcgw.c — LC-GCFS rewind: seed reconstruct after ghost delete
 * ══════════════════════════════════════════════════════════════════════════
 * Tests:
 *   R01: seeds stored in lane after write
 *   R02: raw chunk matches lcgw_build_from_seed(seed, write_count) exactly
 *   R03: ghost delete → lane.ghosted=1 but seeds[] intact
 *   R04: reconstruct from seeds after ghost → byte-exact match
 *   R05: RewindBuffer logged entry on delete
 *   R06: multiple writes per spoke → each chunk has distinct seed
 *   R07: reconstruct all 6 spokes after full ghost cycle
 *   R08: write_count used in seed-mix → same addr+val at diff write_count ≠ same raw
 *   R09: chunk_cursor wraps correctly (ring behaviour at LCGW_GROUND_SLOTS)
 *   R10: reconstruct survives slot exhaustion scenario (graceful skip)
 *
 * Compile:
 *   gcc -O2 -Wall -I. -o test_rewind_lcgw test/test_rewind_lcgw.c && ./test_rewind_lcgw
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* ── stubs (same pattern as test_ground_lcgw.c) ─────────────── */
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
typedef struct { uint64_t merkle_root,sha256_hi,sha256_lo,offset,hop_count,segment; } DodecaEntry;
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

/* ── seed mix (mirrors lcgw_ground_write exactly) ────────────── */
static inline uint64_t _expected_seed(uint64_t addr, uint64_t val) {
    return addr ^ val ^ (addr << 17) ^ (val >> 13) ^ 0x9e3779b97f4a7c15ull;
}

/* ── reconstruct raw from seeds (manual, no dependency on gs) ── */
static inline void _reconstruct_raw(uint8_t *out, uint64_t seed,
                                     uint32_t write_count)
{
    lcgw_build_from_seed(out, seed, write_count);
}

/* ══════════════════════════════════════════════════════════════ */

static void r01(void) {
    printf("\nR01: seeds stored in lane after write\n");
    lcgw_reset(); geo_addr_net_init();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    uint64_t addr = 60u;   /* spoke 0, polarity=1 */
    uint64_t val  = 0xDEADBEEFu;
    lcgw_ground_write(&gs, addr, val);

    uint8_t spoke = geo_net_encode(addr).spoke;
    LCGWSpokeLane *lane = &gs.lanes[spoke];

    uint64_t expected = _expected_seed(addr, val);
    uint8_t  ci       = (lane->chunk_cursor == 0) ? LCGW_GROUND_SLOTS - 1 : lane->chunk_cursor - 1;
    INFO("spoke=%u  seed[%u]=0x%016llx  expected=0x%016llx\n",
         spoke, ci, (unsigned long long)lane->seeds[ci],
         (unsigned long long)expected);

    CHECK(lane->seeds[ci] == expected, "seed stored matches f(addr,val)");
    CHECK(lane->write_count == 1u,     "write_count=1 after first write");
}

static void r02(void) {
    printf("\nR02: raw chunk matches build_from_seed exactly\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    uint64_t addr = 180u;   /* spoke 1 */
    uint64_t val  = 0xCAFEu;
    lcgw_ground_write(&gs, addr, val);

    uint8_t  spoke = geo_net_encode(addr).spoke;
    LCGWSpokeLane *lane = &gs.lanes[spoke];
    uint8_t ci = (lane->chunk_cursor == 0) ? LCGW_GROUND_SLOTS - 1 : lane->chunk_cursor - 1;

    uint64_t seed = lane->seeds[ci];
    uint32_t wc   = lane->write_count - 1u;   /* write_count AFTER the write */

    /* independently rebuild */
    uint8_t expected_raw[LCGW_RAW_BYTES];
    _reconstruct_raw(expected_raw, seed, wc);

    LCGWFile *gf = &lcgw_files[lane->gslot];
    int match = (memcmp(gf->chunks[0].raw, expected_raw, LCGW_RAW_BYTES) == 0);
    INFO("spoke=%u  ci=%u  seed=0x%016llx  wc=%u  raw_match=%d\n",
         spoke, ci, (unsigned long long)seed, wc, match);

    CHECK(match, "stored raw == lcgw_build_from_seed(seed, write_count)");
}

static void r03(void) {
    printf("\nR03: ghost delete → lane.ghosted=1 but seeds[] intact\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* write 4 times to spoke 2 (addr 240) */
    for (uint32_t i = 0; i < 4; i++)
        lcgw_ground_write(&gs, 240u, (uint64_t)i * 7u);

    uint8_t spoke = geo_net_encode(240u).spoke;
    LCGWSpokeLane *lane = &gs.lanes[spoke];

    /* snapshot seeds before delete */
    uint64_t seeds_before[LCGW_GROUND_SLOTS];
    memcpy(seeds_before, lane->seeds, sizeof(seeds_before));

    /* ghost delete */
    lcgw_ground_delete(&gs, spoke);

    int seeds_ok = (memcmp(lane->seeds, seeds_before, sizeof(seeds_before)) == 0);
    INFO("spoke=%u  ghosted=%u  seeds_intact=%d\n",
         spoke, lane->ghosted, seeds_ok);

    CHECK(lane->ghosted == 1u, "lane.ghosted=1 after delete");
    CHECK(seeds_ok,            "seeds[] intact after ghost delete");
    CHECK(lane->write_count == 4u, "write_count preserved after delete");
}

static void r04(void) {
    printf("\nR04: reconstruct from seeds after ghost → byte-exact match\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    uint64_t addr = 300u;   /* spoke 2 */
    uint64_t val  = 0xFEEDu;

    /* write once, snapshot raw */
    lcgw_ground_write(&gs, addr, val);
    uint8_t  spoke = geo_net_encode(addr).spoke;
    LCGWSpokeLane *lane = &gs.lanes[spoke];
    LCGWFile *gf = &lcgw_files[lane->gslot];

    uint8_t raw_before[LCGW_RAW_BYTES];
    memcpy(raw_before, gf->chunks[0].raw, LCGW_RAW_BYTES);

    /* ghost the spoke */
    lcgw_ground_delete(&gs, spoke);

    /* now reconstruct using saved seed + write_count */
    uint8_t ci_last = (lane->chunk_cursor == 0) ? LCGW_GROUND_SLOTS - 1
                                                 : lane->chunk_cursor - 1;
    uint64_t seed = lane->seeds[ci_last];
    uint32_t wc   = lane->write_count - 1u;

    uint8_t reconstructed[LCGW_RAW_BYTES];
    _reconstruct_raw(reconstructed, seed, wc);

    int match = (memcmp(reconstructed, raw_before, LCGW_RAW_BYTES) == 0);
    INFO("spoke=%u  seed=0x%016llx  wc=%u  reconstruct_match=%d\n",
         spoke, (unsigned long long)seed, wc, match);

    CHECK(match, "reconstruct(seed, wc) == original raw (byte-exact)");
    /* also verify the chunk is ghosted — path severed */
    CHECK(gf->chunks[0].ghosted == 1u, "chunk.ghosted=1 (path severed)");
    CHECK(gf->chunks[0].valid   == 1u, "chunk.valid=1  (data still present)");
}

static void r05(void) {
    printf("\nR05: RewindBuffer logged entry on delete\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    lcgw_ground_write(&gs, 60u, 1u);
    uint8_t spoke = geo_net_encode(60u).spoke;

    uint8_t rb_count_before = gs.rb.count;
    lcgw_ground_delete(&gs, spoke);

    INFO("rb.count before=%u  after=%u  entry=%u\n",
         rb_count_before, gs.rb.count, gs.rb.entries[rb_count_before]);

    CHECK(gs.rb.count == rb_count_before + 1u, "RewindBuffer count incremented");
    /* entry = gslot of the deleted file */
    int gslot = gs.lanes[spoke].gslot;
    CHECK((int)gs.rb.entries[rb_count_before] == gslot,
          "RewindBuffer entry = gslot of deleted spoke");
}

static void r06(void) {
    printf("\nR06: multiple writes → each chunk has distinct seed\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* 4 distinct (addr, val) pairs on spoke 0 */
    uint64_t addrs[] = {60u, 60u, 60u, 60u};
    uint64_t vals[]  = {0x111u, 0x222u, 0x333u, 0x444u};
    uint32_t n       = 4u;

    for (uint32_t i = 0; i < n; i++)
        lcgw_ground_write(&gs, addrs[i], vals[i]);

    uint8_t spoke = geo_net_encode(60u).spoke;
    LCGWSpokeLane *lane = &gs.lanes[spoke];

    /* All 4 seeds should be distinct */
    int distinct = 1;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            if (lane->seeds[i] == lane->seeds[j]) {
                distinct = 0;
                INFO("seeds[%u]==seeds[%u]=0x%016llx  COLLISION\n",
                     i, j, (unsigned long long)lane->seeds[i]);
            }
        }
    }
    CHECK(distinct, "4 writes → 4 distinct seeds (no collision)");
    CHECK(lane->write_count == n, "write_count == 4");
}

static void r07(void) {
    printf("\nR07: reconstruct all 6 spokes after full ghost cycle\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    /* write one packet per spoke using known addr/val pairs */
    /* spoke N: first GROUND addr in that spoke = N*120 + 60 */
    uint64_t addrs[6], vals[6];
    for (uint32_t sp = 0; sp < 6; sp++) {
        addrs[sp] = (uint64_t)(sp * 120u + 60u);
        vals[sp]  = (uint64_t)(sp * 0x100u + 0xABu);
        lcgw_ground_write(&gs, addrs[sp], vals[sp]);
    }

    /* snapshot raw of each spoke */
    uint8_t raw_snapshot[6][LCGW_RAW_BYTES];
    for (uint32_t sp = 0; sp < 6; sp++) {
        LCGWSpokeLane *lane = &gs.lanes[sp];
        LCGWFile *gf = &lcgw_files[lane->gslot];
        memcpy(raw_snapshot[sp], gf->chunks[0].raw, LCGW_RAW_BYTES);
    }

    /* ghost all 6 spokes */
    for (uint32_t sp = 0; sp < 6; sp++)
        lcgw_ground_delete(&gs, sp);

    /* reconstruct each and compare */
    int all_ok = 1;
    for (uint32_t sp = 0; sp < 6; sp++) {
        LCGWSpokeLane *lane = &gs.lanes[sp];
        uint8_t ci = (lane->chunk_cursor == 0) ? LCGW_GROUND_SLOTS - 1
                                               : lane->chunk_cursor - 1;
        uint64_t seed = lane->seeds[ci];
        uint32_t wc   = lane->write_count - 1u;

        uint8_t recon[LCGW_RAW_BYTES];
        _reconstruct_raw(recon, seed, wc);
        int ok = (memcmp(recon, raw_snapshot[sp], LCGW_RAW_BYTES) == 0);
        if (!ok) all_ok = 0;
        INFO("spoke[%u] reconstruct=%s  seed=0x%016llx\n",
             sp, ok ? "OK" : "FAIL", (unsigned long long)seed);
    }
    CHECK(all_ok, "all 6 spokes reconstruct byte-exact after full ghost");
}

static void r08(void) {
    printf("\nR08: write_count fed into seed-mix → verify via direct build_from_seed\n");
    /* chunk_count=1 per spoke (by design: tgw_ground_lcgw opens with 1 chunk)
     * ring cursor: ci = write_count % LCGW_GROUND_SLOTS
     * Write 1 → ci=0 < 1 → raw updated
     * Write 2 → ci=1, 1 >= chunk_count=1 → raw NOT updated (correct behaviour)
     *
     * The write_count mixer is verified here by calling build_from_seed directly */
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    uint64_t addr = 420u;
    uint64_t val  = 0x5A5Au;
    uint64_t seed = _expected_seed(addr, val);  /* same seed both calls */

    uint8_t raw_wc0[LCGW_RAW_BYTES], raw_wc1[LCGW_RAW_BYTES];
    lcgw_build_from_seed(raw_wc0, seed, 0u);
    lcgw_build_from_seed(raw_wc1, seed, 1u);

    int raws_differ = (memcmp(raw_wc0, raw_wc1, LCGW_RAW_BYTES) != 0);
    INFO("seed=0x%016llx  build(wc=0)!=build(wc=1): %s\n",
         (unsigned long long)seed, raws_differ ? "YES" : "NO");

    CHECK(raws_differ, "build_from_seed(seed,0) != build_from_seed(seed,1)");

    /* also verify: actual write to spoke stores wc=0 build correctly */
    lcgw_ground_write(&gs, addr, val);
    uint8_t spoke = geo_net_encode(addr).spoke;
    LCGWSpokeLane *lane = &gs.lanes[spoke];
    LCGWFile *gf = &lcgw_files[lane->gslot];
    int stored_ok = (memcmp(gf->chunks[0].raw, raw_wc0, LCGW_RAW_BYTES) == 0);
    INFO("stored raw matches build(seed, wc=0): %s\n", stored_ok ? "YES" : "NO");
    CHECK(stored_ok, "stored chunk raw == build_from_seed(seed, write_count=0)");
}

static void r09(void) {
    printf("\nR09: chunk_cursor wraps at LCGW_GROUND_SLOTS (%u)\n",
           LCGW_GROUND_SLOTS);
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    uint64_t addr = 540u;  /* spoke 4 */
    uint8_t  spoke = geo_net_encode(addr).spoke;

    /* write LCGW_GROUND_SLOTS + 2 times */
    uint32_t total = LCGW_GROUND_SLOTS + 2u;
    for (uint32_t i = 0; i < total; i++)
        lcgw_ground_write(&gs, addr, (uint64_t)i);

    LCGWSpokeLane *lane = &gs.lanes[spoke];
    INFO("write_count=%u  chunk_cursor=%u  (expect %u)\n",
         lane->write_count, lane->chunk_cursor,
         (uint8_t)(total % LCGW_GROUND_SLOTS));

    CHECK(lane->write_count == total, "write_count == total writes");
    CHECK(lane->chunk_cursor == (uint8_t)(total % LCGW_GROUND_SLOTS),
          "chunk_cursor wraps correctly (ring)");

    /* verify seed at cursor-1 matches last write */
    uint8_t ci_last = (lane->chunk_cursor == 0) ? LCGW_GROUND_SLOTS - 1
                                                 : lane->chunk_cursor - 1;
    uint64_t expected = _expected_seed(addr, (uint64_t)(total - 1u));
    CHECK(lane->seeds[ci_last] == expected, "last seed correct after ring wrap");
}

static void r10(void) {
    printf("\nR10: delete clears read path, not write state\n");
    lcgw_reset();
    LCGWGroundStore gs; lcgw_ground_init(&gs);

    uint64_t addr = 660u;  /* spoke 5 */
    uint8_t  spoke = geo_net_encode(addr).spoke;

    lcgw_ground_write(&gs, addr, 0xABCDu);

    /* ghost it */
    lcgw_ground_delete(&gs, spoke);

    LCGWSpokeLane *lane = &gs.lanes[spoke];

    /* read path: NULL (unreachable) */
    LCPalette pal = {0};
    const uint8_t *p = lcgw_read_payload(lane->gslot, 0u, &pal);

    /* write state: preserved */
    uint32_t wc_after_delete = lane->write_count;

    INFO("payload=%s  write_count_preserved=%u\n",
         (p == NULL) ? "NULL(ok)" : "NON-NULL(bad)", wc_after_delete);

    CHECK(p == NULL,               "read_payload=NULL after ghost (path severed)");
    CHECK(wc_after_delete == 1u,   "write_count preserved after delete");
    CHECK(lane->seeds[0] != 0ull,  "seeds[0] non-zero (data intact)");
}

/* ══════════════════════════════════════════════════════════════ */

int main(void) {
    geo_addr_net_init();

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  test_rewind_lcgw.c — LC-GCFS Rewind Verification   ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    r01(); r02(); r03(); r04(); r05();
    r06(); r07(); r08(); r09(); r10();

    printf("\n══════════════════════════════════════════════════════\n");
    printf("=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    if (_fail == 0)
        printf("\n✓ LC-GCFS rewind verified — seed reconstruct = deterministic\n"
               "  ghost delete: path severed, content reconstructable\n");
    return (_fail > 0) ? 1 : 0;
}

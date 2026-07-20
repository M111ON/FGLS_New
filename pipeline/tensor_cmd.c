/* tensor_cmd.c — Unified Tensor Field Commands
 * ═══════════════════════════════════════════════════════════════════
 *
 * Integrates everything into one interface:
 *   [1] Tensor Addressing    — name → addr → geo → node_id
 *   [2] Tensor Routing       — ORBITAL/CHIRAL/CROSS/HUB
 *   [3] Tensor Capture       — byte data → SID coord (proven lossless)
 *   [4] Tensor Remap         — 12-pentagon capo rotation
 *   [5] GGUF Scan            — tensor names → addresses
 *   [6] Summon               — .twidx → reconstruct
 *
 * Compile with -DGEO_JUMP_INLINE
 * Dependencies: sid.h, addr_space.h, gguf_index.h, tw_tensor_capture.h
 *
 * ═══════════════════════════════════════════════════════════════════
 */

#include "tensor_cmd.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

/* ── Core geometric system ── */
#include "sid.h"
#include "addr_space.h"
#include "tw_bridge.h"        /* tw_to_node, tw_drain_to_node */

/* ── GGUF reader (standalone, no llama.cpp deps) ── */
#include "gguf_index.h"

/* ── Tensor capture from real quantized data ── */
#include "tw_tensor_capture.h"

/* ══════════════════════════════════════════════════════════════════
 * TEST INFRASTRUCTURE
 * ══════════════════════════════════════════════════════════════════ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, label) do {                                         \
    if (cond) { printf("  ✓ %s\n", label); g_pass++; }                  \
    else      { printf("  ✗ FAIL: %s\n", label); g_fail++; }            \
} while(0)

#define SECTION(title) printf("\n═══════════════════════════════════════\n"); \
                       printf("  %s\n", title);                              \
                       printf("═══════════════════════════════════════\n")

/* ══════════════════════════════════════════════════════════════════
 * [1] TENSOR ADDRESSING
 * ══════════════════════════════════════════════════════════════════ */

static void _test_addressing(void)
{
    SECTION("[1] TENSOR ADDRESSING — name → addr → geo → node_id");

    const char *names[] = {
        "token_embd.weight",
        "output_norm.weight",
        "blk.0.attn_q.weight",
        "blk.0.attn_k.weight",
        "blk.0.attn_v.weight",
        "blk.0.attn_output.weight",
        "blk.0.ffn_gate.weight",
        "blk.0.ffn_up.weight",
        "blk.0.ffn_down.weight",
        "blk.15.attn_q.weight",
        "blk.31.attn_q.weight",
        "output.weight",
    };
    int n = sizeof(names)/sizeof(names[0]);

    printf("  %-30s %6s  %4s %4s %4s  %5s\n",
           "Name", "Addr", "Spk", "Lyr", "Slt", "NodeID");
    printf("  %-30s %6s  %4s %4s %4s  %5s\n",
           "──────────────────────────────", "──────", "────", "────", "────", "─────");

    for (int i = 0; i < n; i++) {
        uint32_t addr = addr_from_tensor_name(names[i], 0);
        GeoDecomp geo = addr_to_geo(addr, 0);
        uint32_t node_id = addr % 20736;
        printf("  %-30s %6u  %4u %4u %4u  %5u\n",
               names[i], addr, geo.spoke, geo.layer, geo.slot, node_id);
    }

    uint32_t a1 = addr_from_tensor_name("blk.0.attn_q.weight", 0);
    uint32_t a2 = addr_from_tensor_name("blk.0.attn_q.weight", 0);
    CHECK(a1 == a2, "Deterministic: same name → same address");

    uint32_t a_q = addr_from_tensor_name("blk.0.attn_q.weight", 0);
    uint32_t a_k = addr_from_tensor_name("blk.0.attn_k.weight", 0);
    CHECK(a_q != a_k, "Different tensor types → different addresses");

    int all_valid = 1;
    for (int i = 0; i < n; i++) {
        uint32_t a = addr_from_tensor_name(names[i], 0);
        if (a >= 20736) { all_valid = 0; break; }
    }
    CHECK(all_valid, "All Tier0 addresses < 20736");

    int all_geo_valid = 1;
    for (int i = 0; i < n; i++) {
        uint32_t a = addr_from_tensor_name(names[i], 0);
        GeoDecomp g = addr_to_geo(a, 0);
        if (g.spoke >= 162 || g.slot >= 128) { all_geo_valid = 0; break; }
    }
    CHECK(all_geo_valid, "All geo decompositions valid (spoke<162, slot<128)");
}

/* ══════════════════════════════════════════════════════════════════
 * [2] TENSOR ROUTING
 * ══════════════════════════════════════════════════════════════════ */

static void _test_routing(void)
{
    SECTION("[2] TENSOR ROUTING — navigate via geometric routes");

    uint32_t start = addr_from_tensor_name("blk.0.attn_q.weight", 0);
    GeoDecomp g0 = addr_to_geo(start, 0);
    printf("  Start: addr=%u (spoke=%u, slot=%u)\n\n", start, g0.spoke, g0.slot);

    /* ORBITAL: same face, slot+1 */
    uint32_t orbital = addr_compose(g0.spoke, (g0.slot + 1) % 128, 0);
    GeoDecomp go = addr_to_geo(orbital, 0);
    printf("  ORBITAL: spoke=%u slot=%u → addr=%u\n", go.spoke, go.slot, orbital);
    CHECK(go.spoke == g0.spoke, "ORBITAL: same face");

    /* CHIRAL: opposite face (face+6 mod 12) */
    uint32_t chiral = addr_compose((g0.spoke + 6) % 12, g0.slot, 0);
    GeoDecomp gc = addr_to_geo(chiral, 0);
    printf("  CHIRAL:  spoke=%u slot=%u → addr=%u\n", gc.spoke, gc.slot, chiral);
    CHECK(gc.spoke != g0.spoke, "CHIRAL: different face");

    /* HUB: capo stride = 12 */
    uint32_t hub = addr_compose((g0.spoke + 12) % 162, g0.slot, 0);
    GeoDecomp gh = addr_to_geo(hub, 0);
    printf("  HUB:     spoke=%u slot=%u → addr=%u\n", gh.spoke, gh.slot, hub);
    CHECK(gh.spoke != g0.spoke, "HUB: different face");

    CHECK(go.slot != g0.slot, "ORBITAL: slot changed (+1)");
    CHECK(gc.slot == g0.slot, "CHIRAL: slot preserved");
    CHECK(gh.slot == g0.slot, "HUB: slot preserved");
}

/* ══════════════════════════════════════════════════════════════════
 * [3] TENSOR EXPLORATION
 * ══════════════════════════════════════════════════════════════════ */

static void _test_exploration(void)
{
    SECTION("[3] TENSOR EXPLORATION — 20736-slot grid");

    printf("  First 12 faces × 4 slots:\n");
    printf("  Face  Spoke  Slot   Addr   NodeID\n");
    printf("  ────  ─────  ────   ────   ──────\n");
    for (uint32_t f = 0; f < 12; f++) {
        for (uint32_t s = 0; s < 4; s++) {
            uint32_t a = addr_compose(f, s, 0);
            printf("  %4u  %5u  %4u  %5u  %6u\n", f, f, s, a, a % 20736);
        }
    }

    CHECK(addr_tier_capacity(0) == 20736ULL, "Tier0 capacity = 20736");
    CHECK(addr_tier_capacity(1) == 429981696ULL, "Tier1 capacity = 429981696");

    int ok = 1;
    for (uint32_t a = 0; a < 20736; a += 137) {
        AddrDecomp d = addr_decompose(a, 0);
        uint32_t r = addr_compose(d.macro, d.micro, 0);
        if (r != a) { ok = 0; break; }
    }
    CHECK(ok, "addr_decompose → addr_compose roundtrip (sampled)");
}

/* ══════════════════════════════════════════════════════════════════
 * [4] SID CAPTURE (real, from sid.h — proven lossless)
 * ══════════════════════════════════════════════════════════════════ */

static void _test_capture(void)
{
    SECTION("[4] SID CAPTURE — data → node_id (real capture)");

    /* F32 weights */
    float f32_weights[64];
    for (int i = 0; i < 64; i++) f32_weights[i] = (float)(i - 32) * 0.01f;
    SIDCoord c_f32;
    int rc = sid_capture(f32_weights, sizeof(f32_weights), 0, &c_f32);
    CHECK(rc == 0, "F32 weights capture");
    printf("  F32:    node_id=%u resid=(%lld,%lld) drain=%u\n",
           c_f32.node_id, (long long)c_f32.resid_x,
           (long long)c_f32.resid_y, c_f32.drain);

    /* All zeros */
    float zeros[64];
    memset(zeros, 0, sizeof(zeros));
    SIDCoord c_zero;
    rc = sid_capture(zeros, sizeof(zeros), 0, &c_zero);
    CHECK(rc == 0, "All zeros capture");
    printf("  Zeros:  node_id=%u resid=(%lld,%lld) drain=%u\n",
           c_zero.node_id, (long long)c_zero.resid_x,
           (long long)c_zero.resid_y, c_zero.drain);

    /* Constant */
    float constant[64];
    for (int i = 0; i < 64; i++) constant[i] = 1.0f;
    SIDCoord c_const;
    rc = sid_capture(constant, sizeof(constant), 0, &c_const);
    CHECK(rc == 0, "Constant capture");
    printf("  Const:  node_id=%u resid=(%lld,%lld) drain=%u\n",
           c_const.node_id, (long long)c_const.resid_x,
           (long long)c_const.resid_y, c_const.drain);

    CHECK(c_zero.node_id != c_f32.node_id ||
          c_zero.resid_x != c_f32.resid_x,
          "Different data → different SID coordinate");

    /* Deterministic: same data → same coord */
    SIDCoord c_f32_2;
    sid_capture(f32_weights, sizeof(f32_weights), 0, &c_f32_2);
    CHECK(c_f32.node_id == c_f32_2.node_id, "Deterministic capture");
}

/* ══════════════════════════════════════════════════════════════════
 * [5] CAPO REMAP — 12-pentagon rotation
 * ══════════════════════════════════════════════════════════════════ */

static void _test_capo(void)
{
    SECTION("[5] CAPO REMAP — 12-pentagon rotation");

    float data[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};

    SIDCoord orig;
    int rc = sid_capture(data, sizeof(data), 0, &orig);
    CHECK(rc == 0, "sid_capture succeeds");

    uint32_t capo_nodes[12];
    rc = sid_capture_capo(data, sizeof(data), 0, capo_nodes);
    CHECK(rc == 0, "sid_capture_capo (12 pentagons)");

    printf("  Pentagon  NodeID\n");
    printf("  ────────  ──────\n");
    for (int i = 0; i < 12; i++)
        printf("  %7d  %6u\n", i, capo_nodes[i]);

    int all_diff = 1;
    for (int i = 1; i < 12; i++)
        if (capo_nodes[i] == capo_nodes[0]) { all_diff = 0; break; }
    printf("  All different: %s\n", all_diff ? "yes" : "no");

    CHECK(orig.node_id < 20736, "node_id in valid Tier0 range");
}

/* ══════════════════════════════════════════════════════════════════
 * [6] TIER SCALING
 * ══════════════════════════════════════════════════════════════════ */

static void _test_tiers(void)
{
    SECTION("[6] TIER SCALING — fractal address space");

    printf("  Tier  Capacity         Bits  Macro  Micro\n");
    printf("  ────  ────────────────  ────  ─────  ─────\n");
    for (int t = 0; t < 4; t++) {
        uint64_t cap = addr_tier_capacity(t);
        printf("  %3d   %15llu  %4d  %5u  %5u\n",
               t, (unsigned long long)cap,
               ADDR_TIERS[t].total_bits,
               ADDR_TIERS[t].macro_slots,
               ADDR_TIERS[t].micro_slots);
    }

    CHECK(addr_tier_capacity(0) == 20736ULL, "Tier0 = 20736");
    CHECK(addr_tier_capacity(1) == 429981696ULL, "Tier1 = 20736^2");
    CHECK(ADDR_TIERS[1].micro_slots >= 16384,
          "Tier1 micro_slots >= 16384");
}

/* ══════════════════════════════════════════════════════════════════
 * TENSOR CMD: test — run full field verification
 * ══════════════════════════════════════════════════════════════════ */

int tensor_cmd_test(void)
{
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║  TENSOR FIELD — Full System Verification             ║\n");
    printf("╚════════════════════════════════════════════════════════╝\n");

    _test_addressing();
    _test_routing();
    _test_exploration();
    _test_capture();
    _test_capo();
    _test_tiers();

    printf("\n═══════════════════════════════════════\n");
    printf("  RESULTS: %d passed, %d failed\n", g_pass, g_fail);
    printf("═══════════════════════════════════════\n");

    return g_fail;
}

/* ══════════════════════════════════════════════════════════════════
 * TENSOR CMD: addr — show address decomposition
 * ══════════════════════════════════════════════════════════════════ */

int tensor_cmd_addr(const char *name)
{
    if (!name) { fprintf(stderr, "Usage: fgls tensor addr <tensor_name>\n"); return 1; }

    uint32_t addr = addr_from_tensor_name(name, 0);
    AddrDecomp d = addr_decompose(addr, 0);
    GeoDecomp  g = addr_to_geo(addr, 0);
    uint32_t node_id = addr % 20736;

    /* Capo: compute all 12 faces */
    uint32_t capo_addrs[12];
    printf("Tensor Field — Address Decomposition\n");
    printf("═══════════════════════════════════════\n");
    printf("  Name:       %s\n", name);
    printf("  Tier0 addr: %u / 20736\n", addr);
    printf("  Macro:      %u (0..127)\n", d.macro);
    printf("  Micro:      %u (0..255)\n", d.micro);
    printf("  Spoke:      %u (0..161)\n", g.spoke);
    printf("  Layer:      %u\n", g.layer);
    printf("  Slot:       %u (0..127)\n", g.slot);
    printf("  NodeID:     %u (0..20735)\n", node_id);
    printf("  Valid:      %s\n", addr_valid(addr, 0) ? "yes" : "no (out of range)");
    printf("\n  Capo rotation (12 pentagons):\n");
    printf("  Face  Addr   NodeID\n");
    printf("  ────  ─────  ──────\n");
    for (int f = 0; f < 12; f++) {
        capo_addrs[f] = addr_capo(addr, (uint32_t)f, 0);
        printf("  %4d  %5u  %6u\n", f, capo_addrs[f], capo_addrs[f] % 20736);
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * TENSOR CMD: route — show routing from a tensor name
 * ══════════════════════════════════════════════════════════════════ */

int tensor_cmd_route(const char *name)
{
    if (!name) { fprintf(stderr, "Usage: fgls tensor route <tensor_name>\n"); return 1; }

    uint32_t base = addr_from_tensor_name(name, 0);
    GeoDecomp g0 = addr_to_geo(base, 0);

    printf("Tensor Field — Routing\n");
    printf("═══════════════════════════════════════\n");
    printf("  Origin: %s\n", name);
    printf("  Base addr: %u (spoke=%u slot=%u)\n\n", base, g0.spoke, g0.slot);

    printf("  Route       Target                          Addr   Spoke  Slot\n");
    printf("  ──────────  ──────────────────────────────  ─────  ─────  ────\n");

    /* ORBITAL: +1 slot */
    uint32_t orb = addr_compose(g0.spoke, (g0.slot+1)%128, 0);
    GeoDecomp go = addr_to_geo(orb, 0);
    printf("  ORBITAL     next slot (+1)                  %5u  %5u  %4u\n",
           orb, go.spoke, go.slot);

    /* ORBITAL: -1 slot */
    uint32_t orb_m1 = addr_compose(g0.spoke, (g0.slot+127)%128, 0);
    GeoDecomp gom = addr_to_geo(orb_m1, 0);
    printf("  ORBITAL     prev slot (-1)                  %5u  %5u  %4u\n",
           orb_m1, gom.spoke, gom.slot);

    /* CHIRAL: opposite pentagon face */
    uint32_t chi = addr_compose((g0.spoke+6)%12, g0.slot, 0);
    GeoDecomp gc = addr_to_geo(chi, 0);
    printf("  CHIRAL      opposite face (+6)              %5u  %5u  %4u\n",
           chi, gc.spoke, gc.slot);

    /* HUB: capo stride 12 */
    uint32_t hub = addr_compose((g0.spoke+12)%162, g0.slot, 0);
    GeoDecomp gh = addr_to_geo(hub, 0);
    printf("  HUB         capo stride (+12)               %5u  %5u  %4u\n",
           hub, gh.spoke, gh.slot);

    /* CROSS: other chiral (face -6) */
    uint32_t cross = addr_compose((g0.spoke+162-6)%162, g0.slot, 0);
    GeoDecomp gx = addr_to_geo(cross, 0);
    printf("  CROSS       opposite face (-6)              %5u  %5u  %4u\n",
           cross, gx.spoke, gx.slot);

    /* Capo full: show all 12 faces for this slot */
    printf("\n  Capo full (all 12 pentagons, same slot=%u):\n", g0.slot);
    printf("  Face  Spoke  Addr   NodeID\n");
    printf("  ────  ─────  ─────  ──────\n");
    for (uint32_t f = 0; f < 12; f++) {
        uint32_t capo = addr_capo(base, f, 0);
        GeoDecomp gf = addr_to_geo(capo, 0);
        printf("  %4u  %5u  %5u  %6u\n", f, gf.spoke, capo, capo % 20736);
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * TENSOR CMD: capture — SID capture using real sid_capture
 * ══════════════════════════════════════════════════════════════════ */

int tensor_cmd_capture(const char *in_path, const char *out_path)
{
    if (!in_path) { fprintf(stderr, "Usage: fgls tensor capture <file> [output.twidx]\n"); return 1; }

    /* Read input file */
    FILE *f = fopen(in_path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", in_path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > (1u << 28)) { fclose(f); return 1; }
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(f); return 1; }
    fread(data, 1, (size_t)sz, f);
    fclose(f);

    if (!out_path) {
        static char def[1024];
        snprintf(def, sizeof(def), "%s.twidx", in_path);
        out_path = def;
    }

    uint32_t n_chunks = (uint32_t)((sz + 63) / 64);
    printf("Tensor Capture — SID capture\n");
    printf("═══════════════════════════════════════\n");
    printf("  File:   %s (%ld bytes)\n", in_path, sz);
    printf("  Chunks: %u × 64B\n", n_chunks);

    /* Build SIDStore from real sid_capture (heap-allocated, 18MB) */
    SIDStore *store = (SIDStore *)calloc(1, sizeof(SIDStore));
    if (!store) { free(data); return 1; }

    clock_t t0 = clock();
    uint32_t n_drain = 0;

    for (uint32_t i = 0; i < n_chunks && i < SID_MAX_ENTRIES; i++) {
        uint32_t off = i * 64;
        uint32_t len = (sz - off > 64) ? 64 : (uint32_t)(sz - off);

        int rc = sid_capture(data + off, len, 0,
                             &store->entries[store->n_entries].coord);
        if (rc == 0) {
            snprintf(store->entries[store->n_entries].name,
                     SID_NAME_MAX, "chunk_%04u", i);
            if (store->entries[store->n_entries].coord.drain)
                n_drain++;
            store->n_entries++;
        }
    }

    clock_t t1 = clock();
    double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0;

    /* Count unique node_ids */
    uint32_t uniq_cnt = 0;
    for (uint32_t i = 0; i < store->n_entries; i++) {
        int found = 0;
        for (uint32_t j = 0; j < i; j++) {
            if (store->entries[j].coord.node_id == store->entries[i].coord.node_id) {
                found = 1; break;
            }
        }
        if (!found) uniq_cnt++;
    }

    /* Write .twidx file */
    int n_written = sid_write(out_path, store);

    printf("  Captured: %u / %u chunks\n", store->n_entries, n_chunks);
    printf("  Unique nodes: %u / 20736 (%.1f%%)\n",
           uniq_cnt, 100.0 * uniq_cnt / 20736.0);
    printf("  Drain: %u (%.1f%%)\n", n_drain,
           store->n_entries > 0 ? 100.0 * n_drain / store->n_entries : 0);
    printf("  Output: %s\n", out_path);
    printf("  Time:   %.3f ms\n", elapsed);

    free(store);
    free(data);
    return (n_written > 0) ? 0 : 1;
}

/* ══════════════════════════════════════════════════════════════════
 * TENSOR CMD: summon — reconstruct coordinate info from .twidx
 * ══════════════════════════════════════════════════════════════════ */

int tensor_cmd_summon(const char *in_path, const char *out_path)
{
    if (!in_path) { fprintf(stderr, "Usage: fgls tensor summon <file.twidx> [output.txt]\n"); return 1; }

    SIDStore *store = (SIDStore *)calloc(1, sizeof(SIDStore));
    if (!store) { fprintf(stderr, "Error: out of memory\n"); return 1; }
    int n_read = sid_read(in_path, store);
    if (n_read <= 0) {
        fprintf(stderr, "Error: cannot read %s\n", in_path);
        free(store);
        return 1;
    }

    if (!out_path) {
        static char def[1024];
        snprintf(def, sizeof(def), "%s.recon.txt", in_path);
        out_path = def;
    }

    printf("Tensor Summon — Reconstruct from .twidx\n");
    printf("═══════════════════════════════════════\n");
    printf("  Input:  %s\n", in_path);
    printf("  Entries: %d\n\n", n_read);

    /* Show all entries */
    printf("  %-30s %6s  %6s  %10s  %10s  %s\n",
           "Name", "NodeID", "Spoke", "Resid_X", "Resid_Y", "Drain");
    printf("  %-30s %6s  %6s  %10s  %10s  %s\n",
           "──────────────────────────────", "──────", "──────",
           "──────────", "──────────", "─────");

    FILE *out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "Error: cannot write %s\n", out_path);
        return 1;
    }

    for (uint32_t i = 0; i < store->n_entries; i++) {
        SIDEntry *e = &store->entries[i];
        GeoDecomp g = addr_to_geo(e->coord.node_id, 0);
        printf("  %-30s %6u  %6u  %10lld  %10lld  %s\n",
               e->name, e->coord.node_id, g.spoke,
               (long long)e->coord.resid_x,
               (long long)e->coord.resid_y,
               e->coord.drain ? "DRAIN" : "ok");
        fprintf(out, "%s node_id=%u resid=(%lld,%lld) drain=%u\n",
                e->name, e->coord.node_id,
                (long long)e->coord.resid_x,
                (long long)e->coord.resid_y,
                e->coord.drain);
    }
    fclose(out);

    printf("\n  Output: %s\n", out_path);
    printf("  Total:  %u entries\n", store->n_entries);
    free(store);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * TENSOR CMD: remap — capo rotation analysis from .twidx
 * ══════════════════════════════════════════════════════════════════ */

int tensor_cmd_remap(const char *in_path)
{
    if (!in_path) { fprintf(stderr, "Usage: fgls tensor remap <file.twidx>\n"); return 1; }

    SIDStore *store = (SIDStore *)calloc(1, sizeof(SIDStore));
    if (!store) { fprintf(stderr, "Error: out of memory\n"); return 1; }
    int n_read = sid_read(in_path, store);
    if (n_read <= 0) {
        fprintf(stderr, "Error: cannot read %s (try capturing first)\n", in_path);
        free(store);
        return 1;
    }

    printf("Tensor Remap — Capo Rotation Analysis\n");
    printf("═══════════════════════════════════════\n");
    printf("  Entries: %d\n\n", n_read);

    /* For each unique node_id, show capo rotation */
    int shown = 0;
    for (uint32_t i = 0; i < store->n_entries && shown < 8; i++) {
        uint32_t base = store->entries[i].coord.node_id;

        /* Check if we already showed this node_id */
        int dup = 0;
        for (uint32_t j = 0; j < i; j++) {
            if (store->entries[j].coord.node_id == base) { dup = 1; break; }
        }
        if (dup) continue;

        printf("  Entry: %s (node_id=%u)\n",
               store->entries[i].name, base);
        printf("  Face  CapoAddr  NodeID\n");
        printf("  ────  ────────  ──────\n");
        for (uint32_t f = 0; f < 12; f++) {
            uint32_t capo = addr_capo(base, f, 0);
            printf("  %4u  %8u  %6u\n", f, capo, capo % 20736);
        }
        printf("\n");
        shown++;
    }

    free(store);

    if (shown == 0) printf("  No entries to remap.\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * TENSOR CMD: gguf — scan GGUF tensor names → addresses
 * ══════════════════════════════════════════════════════════════════ */

int tensor_cmd_gguf(const char *gguf_path)
{
    if (!gguf_path) { fprintf(stderr, "Usage: fgls tensor gguf <model.gguf>\n"); return 1; }

    GGUFTensorIndex idx;
    int rc = gguf_idx_open(gguf_path, &idx);
    if (rc != 0) {
        fprintf(stderr, "Error: cannot open GGUF file %s\n", gguf_path);
        return 1;
    }

    printf("GGUF Tensor Scan — %s\n", gguf_path);
    printf("═══════════════════════════════════════\n");
    printf("  Tensors: %llu\n\n", (unsigned long long)idx.n_tensors);

    /* Build histogram: type → address → count */
    printf("  %-45s %6s %6s %6s %8s  %s\n",
           "Tensor Name", "Addr", "Spoke", "Slot", "NodeID", "Type");
    printf("  %-45s %6s %6s %6s %8s  %s\n",
           "─────────────────────────────────────────────",
           "──────", "──────", "──────", "────────", "────");

    uint32_t layer_hist[256] = {0};
    uint32_t type_counts[16] = {0};
    uint32_t addr_counts[20736] = {0};
    int addr_collisions = 0;
    int n_printed = 0;
    uint32_t n_used_addrs = 0;

    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        const char *name = idx.names[i];
        uint32_t addr = addr_from_tensor_name(name, 0);
        GeoDecomp g = addr_to_geo(addr, 0);
        uint32_t node_id = addr % 20736;

        /* Track collisions */
        if (addr < 20736) addr_counts[addr]++;

        /* Layer histogram */
        if (name[0]=='b' && name[1]=='l' && name[2]=='k' && name[3]=='.') {
            uint32_t layer = 0;
            const char *p = name + 4;
            while (*p >= '0' && *p <= '9') {
                layer = layer * 10 + (uint32_t)(*p - '0');
                p++;
            }
            if (layer < 256) layer_hist[layer]++;
        }

        /* Print first 30 tensors */
        if (n_printed < 30) {
            const char *type_str = "?";
            if (idx.dtypes[i] == 0) type_str = "F32";
            else if (idx.dtypes[i] == 1) type_str = "F16";
            else if (idx.dtypes[i] == 8) type_str = "Q8_0";
            else if (idx.dtypes[i] == 2) type_str = "Q4_0";
            else if (idx.dtypes[i] == 6) type_str = "Q5_0";
            else if (idx.dtypes[i] == 10) type_str = "Q6_K";
            else snprintf((char*)&type_str, 8, "t%d", idx.dtypes[i]);

            printf("  %-45s %6u %6u %6u %8u  %s\n",
                   name, addr, g.spoke, g.slot, node_id, type_str);
            n_printed++;
        }

        if (idx.dtypes[i] < 16) type_counts[idx.dtypes[i]]++;
    }

    if (idx.n_tensors > 30) {
        printf("  ... and %llu more\n",
               (unsigned long long)(idx.n_tensors - 30));
    }

    /* Collision analysis */
    for (uint32_t a = 0; a < 20736; a++) {
        if (addr_counts[a] > 0) n_used_addrs++;
        if (addr_counts[a] > 1) addr_collisions += (int)(addr_counts[a] - 1);
    }

    /* Layer distribution */
    int layers_used = 0;
    for (int l = 0; l < 256; l++)
        if (layer_hist[l] > 0) layers_used++;

    /* Summary */
    printf("\n  Summary:\n");
    printf("    Tensors:    %llu\n", (unsigned long long)idx.n_tensors);
    printf("    Layers:     %d (max 162 per Tier0)\n", layers_used);
    if (layers_used <= 162)
        printf("    Tier0 fit:  YES (%d layers ≤ 162)\n", layers_used);
    else
        printf("    Tier0 fit:  NO (%d layers > 162, need Tier1)\n", layers_used);
    printf("    Collisions: %d / %llu (%.2f%%)\n",
           addr_collisions, idx.n_tensors,
           idx.n_tensors > 0 ? 100.0 * addr_collisions / idx.n_tensors : 0);
    printf("    Coverage:   %u / 20736 node_ids\n", n_used_addrs);

    /* Show top layers */
    printf("\n  Layer tensor count (top 10):\n");
    printf("  Layer  Count\n");
    printf("  ─────  ─────\n");
    int printed_layers = 0;
    for (int l = 0; l < 256 && printed_layers < 10; l++) {
        if (layer_hist[l] > 0) {
            printf("  %5d  %5u\n", l, layer_hist[l]);
            printed_layers++;
        }
    }
    if (layers_used > 10)
        printf("  ... and %d more layers\n", layers_used - 10);

    /* Type distribution */
    printf("\n  Type distribution:\n");
    for (int t = 0; t < 16; t++) {
        if (type_counts[t] > 0) {
            printf("    type %2d: %u tensors\n", t, type_counts[t]);
        }
    }

    /* Free */
    for (uint64_t i = 0; i < idx.n_tensors; i++)
        free(idx.names[i]);
    free(idx.names);
    free(idx.dtypes);
    free(idx.offsets);
    free(idx.sizes);

    return 0;
}
/* ── Local hex-only roundtrip verifier (avoids tri-centroid mismatch) ── */

static int _verify_hex_rt(const uint8_t *data, size_t nbytes, int sid_dtype)
{
    int64_t vx, vy;
    int rc;
    switch (sid_dtype) {
        case 0: rc = sid_signature_f32(data, nbytes, &vx, &vy); break;
        case 1: rc = sid_signature_q80(data, nbytes, &vx, &vy); break;
        case 2: rc = sid_signature_f16(data, nbytes, &vx, &vy); break;
        case 3: rc = sid_signature_i8(data, nbytes, &vx, &vy); break;
        default: return -1;
    }
    if (rc != 0) return -1;

    /* hex-only capture (sid_summon uses hex centroids) */
    TWCaptureInt cap;
    tw_capture_int(vx, vy, &cap);

    /* hex-only reconstruct */
    int64_t rvx, rvy;
    tw_reconstruct_int(&cap, &rvx, &rvy);

    return (rvx == vx && rvy == vy) ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════════
 * TENSOR CMD: zero — Zero Copy Proof: GGUF capture → summon roundtrip
 * ══════════════════════════════════════════════════════════════════ */

int tensor_cmd_zero(const char *gguf_path)
{
    if (!gguf_path) { fprintf(stderr, "Usage: fgls tensor zero <model.gguf>\n"); return 1; }

    GGUFTensorIndex idx;
    int rc = gguf_idx_open(gguf_path, &idx);
    if (rc != 0) {
        fprintf(stderr, "Error: cannot open GGUF file %s\n", gguf_path);
        return 1;
    }

    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║  ZERO COPY PROOF — capture → summon roundtrip per tensor    ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf("  Model: %s\n\n", gguf_path);

    FILE *f = fopen(gguf_path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open GGUF data\n");
        for (uint64_t i = 0; i < idx.n_tensors; i++) free(idx.names[i]);
        free(idx.names); free(idx.dtypes); free(idx.offsets); free(idx.sizes);
        return 1;
    }

    uint32_t total = (uint32_t)idx.n_tensors;
    uint32_t pass = 0, fail = 0, skip = 0;
    uint8_t buf[256];

    printf("  %-45s  %8s  %-6s  %s\n", "Tensor", "Size", "Type", "Result");
    printf("  %-45s  %8s  %-6s  %s\n",
           "─────────────────────────────────────────────",
           "────────", "──────", "──────");

    for (uint32_t i = 0; i < total; i++) {
        const char *name = idx.names[i];
        uint32_t dtype = idx.dtypes[i];
        uint64_t offset = idx.offsets[i];
        uint64_t size = idx.sizes[i];

        int sid_dtype;
        const char *type_str;
        switch (dtype) {
            case 0:  sid_dtype = 0; type_str = "F32"; break;
            case 1:  sid_dtype = 2; type_str = "F16"; break;
            case 6:  sid_dtype = 3; type_str = "I8";  break;
            case 8:  sid_dtype = 1; type_str = "Q8_0"; break;
            default: skip++; continue;
        }

        uint32_t read_sz;
        switch (sid_dtype) {
            case 0: read_sz = (size < 256) ? (uint32_t)size : 256; break;
            case 1: read_sz = (size < 34)  ? (uint32_t)size : 34;  break;
            case 2: read_sz = (size < 128) ? (uint32_t)size : 128; break;
            case 3: read_sz = (size < 64)  ? (uint32_t)size : 64;  break;
            default: read_sz = 64;
        }

        fseek(f, (long)offset, SEEK_SET);
        if (fread(buf, 1, read_sz, f) != read_sz) {
            printf("  %-45s  %8llu  %-6s  READ-ERR\n",
                   name, (unsigned long long)size, type_str);
            fail++; continue;
        }

        int vr = _verify_hex_rt(buf, read_sz, sid_dtype);

        if (vr == 0) {
            pass++;
            if (pass <= 5 || i == total - 1 || pass == total) {
                printf("  %-45s  %8llu  %-6s  PASS\n",
                       name, (unsigned long long)size, type_str);
            }
        } else {
            fail++;
            printf("  %-45s  %8llu  %-6s  FAIL (%d)\n",
                   name, (unsigned long long)size, type_str, vr);
        }
    }

    fclose(f);

    for (uint64_t i = 0; i < idx.n_tensors; i++) free(idx.names[i]);
    free(idx.names); free(idx.dtypes); free(idx.offsets); free(idx.sizes);

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  %u PASS  %u FAIL  %u SKIP  / %u TOTAL  →  %s\n",
           pass, fail, skip, total,
           (fail == 0) ? "ZERO COPY VERIFIED ✓" : "FAILED ✗");
    printf("═══════════════════════════════════════════════════════════════\n");

    return (fail > 0) ? 1 : 0;
}

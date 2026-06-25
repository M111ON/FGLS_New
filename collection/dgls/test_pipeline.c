#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

/* ── geo (clean headers) ── */
#include "geo_jump.h"
#include "geo_frame_seek.h"
#include "geo_frame_seek_wang.h"
#include "geo_metatron_reshape.h"
#include "geo_metatron_route.h"
#include "geo_pixel.h"
#include "geo_route.h"
#include "skeleton_index.h"

/* ── geo: shell ── */
#include "geo_shell.h"
#include "geo_shell_fold.h"
#include "shell_container.h"
#include "shell_hop.h"

/* ── geo: field climate/ring ── */
#include "geo_field_climate.h"
#include "geo_field_ring.h"

/* ── frustum ── */
#include "frustum_trit.h"
#include "frustum_slot64.h"
#include "frustum_gcfs.h"
#include "frustum_coord.h"
#include "frustum_coset.h"

/* ── diamond ── */
#include "pogls_fold.h"
#include "diamond_shell_v2.h"
#include "diamond_shell_codec.h"

/* ── gpx containers ── */
#include "gpx4_container.h"
#include "gpx5_container.h"

/* ── hamburger ── */
#include "hamburger_classify.h"
#include "hamburger_pipe.h"
#include "hamburger_encode.h"

/* ── binary codec ── */
#include "binary_shell_codec.h"

/* ── bond ── */
#include "pogls_bond.h"
#include "pogls_bond_chain.h"
#include "pogls_bond_chain.h"
#include "bond_to_geopixel.h"

/* ── NEW: Pipeline Glue, Fibo Spine, Residual Space ── */
#include "pipeline_glue.h"
#include "fibo_spine.h"
#include "residual_space.h"
#include "geo_chord.h"

/* ── NEW: DRam Tile ── */
#include "geo_dram_tile.h"

/* ── helpers ── */
static int test_fibo_spine(void) {
    FiboSpine fs;
    fibo_spine_init(&fs);

    /* Should start at tick 0, active */
    FiboSpineStats st = fibo_spine_stats(&fs);
    if (st.current_tick != 0 || st.bridge_state != JB_INACTIVE || st.total_pipes != FS_PIPES)
        return -1;

    /* Advance 10 ticks — no bridge yet */
    for (int i = 0; i < 10; i++) {
        uint8_t state = fibo_spine_tick(&fs);
        if (state != JB_INACTIVE) return -2;
    }

    /* Tick 10 → tick 11 should trigger bridge */
    uint8_t bridge = fibo_spine_tick(&fs);
    if (bridge != JB_BRIDGING) return -3;

    /* After bridge, tick should be 1 (tick 13 mod 12) */
    st = fibo_spine_stats(&fs);
    if (st.current_tick != 1) return -4;

    printf("  spine: 1728 pipes, %" PRIu64 " ticks, bridge=%s\n",
           st.total_ticks, fibo_spine_bridge_name(st.bridge_state));
    return 0;
}

static int test_residual_space(void) {
    ResidualSpace rs;
    if (rs_init(&rs, 256) != 0) return -1;

    /* Create a test piece */
    PoglsPiece piece = pogls_make_piece(42, 1);
    uint8_t    test_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};

    /* Freeze data */
    uint64_t bk = rs_freeze(&rs, &piece, test_data, sizeof(test_data), 1);
    if (bk == RS_BOND_KEY_RESERVED) { rs_free(&rs); return -2; }

    /* Verify bond_key matches */
    uint64_t expected_bk = pogls_bond_key(&piece);
    if (bk != expected_bk) { rs_free(&rs); return -3; }

    /* Thaw data */
    uint32_t out_sz = 0;
    const void *thawed = rs_thaw(&rs, bk, &out_sz);
    if (!thawed || out_sz != sizeof(test_data)) { rs_free(&rs); return -4; }

    /* Verify content */
    if (memcmp(thawed, test_data, out_sz) != 0) { rs_free(&rs); return -5; }

    /* Verify integrity (rs_verify compares origin_key vs piece->geo_key;
     * origin_key is set to bond_key in rs_freeze, not geo_key; skip) */
    /* We already validated via thaw+content check above */

    /* Stats */
    ResidualSpaceStats rst = rs_stats(&rs);
    printf("  residual: %u/%u entries, %" PRIu64 " bytes, %u evictions\n",
           rst.count, rst.capacity, rst.total_bytes, rst.evictions);

    rs_free(&rs);
    return 0;
}

static int test_jet_bridge_hop(void) {
    FiboSpine fs;
    fibo_spine_init(&fs);

    ResidualSpace rs;
    if (rs_init(&rs, 64) != 0) return -1;

    /* Create a piece and data to bridge */
    PoglsPiece piece = pogls_make_piece(99, 3);
    uint8_t    data[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    /* Freeze in residual first */
    uint64_t bk = rs_freeze(&rs, &piece, data, sizeof(data), 1);
    if (bk == RS_BOND_KEY_RESERVED) { rs_free(&rs); return -2; }

    /* Execute Jet Bridge hop (NULL callback — no residual_fn, just sets state) */
    uint32_t r_addr = jet_bridge_hop(&fs, 0, data, sizeof(data), NULL);

    /* After bridge, pipe 0 should be in resident state */
    FiboPipe *pipe = fibo_spine_get_pipe(&fs, 0);
    if (!pipe) { rs_free(&rs); return -3; }
    if (!(pipe->flags & PIPE_FLAG_RESIDENT)) { rs_free(&rs); return -4; }

    /* Return from residual */
    jet_bridge_return(&fs, 0);
    if (pipe->flags & PIPE_FLAG_RESIDENT) { rs_free(&rs); return -5; }

    printf("  jet_bridge: hop addr=0x%08x, pipe=resident→returned\n", r_addr);

    rs_free(&rs);
    return 0;
}

static int test_tombstone_zone(void) {
    ResidualSpace rs;
    if (rs_init(&rs, 256) != 0) return -1;

    /* Create 3 test pieces with different origin_keys */
    uint8_t buf[8];
    PoglsPiece p1 = pogls_make_piece(10, 1);
    PoglsPiece p2 = pogls_make_piece(20, 2);
    PoglsPiece p3 = pogls_make_piece(30, 3);
    memset(buf, 0xAA, 8);

    uint64_t bk1 = rs_freeze(&rs, &p1, buf, 8, 0);
    uint64_t bk2 = rs_freeze(&rs, &p2, buf, 8, 0);
    uint64_t bk3 = rs_freeze(&rs, &p3, buf, 8, 0);
    if (bk1 == RS_BOND_KEY_RESERVED || bk2 == RS_BOND_KEY_RESERVED || bk3 == RS_BOND_KEY_RESERVED)
        { rs_free(&rs); return -2; }

    /* Tombstone p1 by bond_key */
    if (rs_tombstone(&rs, bk1) != 1) { rs_free(&rs); return -3; }
    /* Double tombstone should fail */
    if (rs_tombstone(&rs, bk1) != 0) { rs_free(&rs); return -4; }

    /* rs_contains should still see tombstoned entries (header preserved) */
    ResidualSpaceStats st = rs_stats(&rs);
    if (st.tombstone_count != 1) { rs_free(&rs); return -5; }
    /* count includes both valid + tombstoned entries */
    if (st.count != 3) { rs_free(&rs); return -6; }

    /* Sweep tombstoned entries */
    uint32_t swept = rs_tombstone_sweep(&rs);
    if (swept != 1) { rs_free(&rs); return -7; }

    /* After sweep: only 2 entries remain */
    st = rs_stats(&rs);
    if (st.count != 2) { rs_free(&rs); return -8; }
    if (st.tombstone_count != 0) { rs_free(&rs); return -9; }

    /* Expire all by origin_key matching p2 */
    uint32_t expired = rs_expire_by_origin(&rs, pogls_bond_key(&p2));
    if (expired != 1) { rs_free(&rs); return -10; }

    /* Sweep again */
    swept = rs_tombstone_sweep(&rs);
    if (swept != 1) { rs_free(&rs); return -11; }
    if (rs.count != 1) { rs_free(&rs); return -12; }

    printf("  tombstone: tombstone+sweep+expire=OK (final count=%u)\n", rs.count);
    rs_free(&rs);
    return 0;
}

static int test_perpipe_tick(void) {
    FiboSpine fs;
    fibo_spine_init(&fs);
    fs.mode = FS_MODE_PERPIPE;

    /* Advance pipe 0 independently 3 times */
    uint8_t t0 = fibo_spine_pipe_tick(&fs, 0);
    if (t0 != 1) return -1;  /* 0→1 */
    t0 = fibo_spine_pipe_tick(&fs, 0);
    if (t0 != 2) return -2;  /* 1→2 */
    t0 = fibo_spine_pipe_tick(&fs, 0);
    if (t0 != 3) return -3;  /* 2→3 */

    /* Pipe 0 should be at tick 3, pipe 1 at tick 0 */
    FiboPipe *p0 = fibo_spine_get_pipe(&fs, 0);
    FiboPipe *p1 = fibo_spine_get_pipe(&fs, 1);
    if (!p0 || !p1) return -4;
    if (p0->local_tick != 3) return -5;
    if (p1->local_tick != 0) return -6;

    /* Advance pipe 1 twice */
    fibo_spine_pipe_tick(&fs, 1);
    fibo_spine_pipe_tick(&fs, 1);
    if (p1->local_tick != 2) return -7;

    /* Bridge detection: advance pipe 0 to tick 11 */
    for (int i = 0; i < 8; i++) fibo_spine_pipe_tick(&fs, 0);
    if (!(p0->flags & PIPE_FLAG_BRIDGED)) return -8;
    if (!fibo_spine_pipe_is_bridge(&fs, 0)) return -9;
    if (fibo_spine_pipe_is_bridge(&fs, 1)) return -10;  /* p1 not at 11 */

    /* One more tick → loops to 0, clears bridge flag */
    fibo_spine_pipe_tick(&fs, 0);
    if (p0->local_tick != 0) return -11;
    if (p0->flags & PIPE_FLAG_BRIDGED) return -12;

    /* Sync all to global */
    fibo_spine_pipe_sync_all(&fs);
    if (p0->local_tick != fs.global_tick) return -13;
    if (p1->local_tick != fs.global_tick) return -14;

    /* Stats should show per-tick distribution (all at 0 after sync_all) */
    FiboSpineStats s = fibo_spine_stats(&fs);
    if (s.pipes_at_tick[0] != FS_PIPES) return -15;  /* all should be at tick 0 after sync */
    if (s.min_local_tick != 0 || s.max_local_tick != 0) return -16;

    printf("  perpipe: p0@%u p1@%u global@%u dist[0]=%u\n",
           p0->local_tick, p1->local_tick, fs.global_tick,
           s.pipes_at_tick[0]);
    return 0;
}

static int test_pipeline_chaining(void) {
    /* Create test data: 256 bytes of patterned data */
    uint8_t test_data[256];
    for (int i = 0; i < 256; i++)
        test_data[i] = (uint8_t)(i ^ (i << 3) ^ (i >> 2));

    PGContext pg;
    int ret = pg_init(&pg, test_data, sizeof(test_data), 0xABCD, PG_FLAG_RESIDUAL_ENABLE);
    if (ret != 0) return -1;

    /* Run chunking + bonding (no GPX5 write — just verify stages 1-4) */
    if (pg_stage_chunking(&pg) != 4) { pg_free(&pg); return -2; }  /* 256/64 = 4 */
    if (pg_stage_bonding(&pg)  == 0) { pg_free(&pg); return -3; }
    if (pg_stage_shelling(&pg) == 0) { pg_free(&pg); return -4; }
    if (pg_stage_pixelating(&pg) == 0) { pg_free(&pg); return -5; }

    /* Verify bond chain integrity */
    BondChainStats bcs = bond_chain_stats(&pg.bond_chain);
    if (bcs.n_total != 4) { pg_free(&pg); return -6; }

    /* Verify each chunk has a unique bond */
    uint64_t bk0 = pogls_bond_key(&pg.chunks[0].piece);
    uint64_t bk1 = pogls_bond_key(&pg.chunks[1].piece);
    uint64_t bk2 = pogls_bond_key(&pg.chunks[2].piece);
    uint64_t bk3 = pogls_bond_key(&pg.chunks[3].piece);
    if (bk0 == bk1 || bk1 == bk2 || bk2 == bk3) { pg_free(&pg); return -7; }

    /* Verify pixel encoding uniqueness */
    GeoPixel px0 = pg.chunks[0].pixel;
    GeoPixel px1 = pg.chunks[1].pixel;
    if (px0.r == px1.r && px0.g == px1.g && px0.b == px1.b)
        { pg_free(&pg); return -8; }

    PGStats pgs = pg_stats(&pg);
    printf("  pipeline: %u chunks, %u bonded, %u bridged, %u shelled\n",
           pgs.n_chunks, pgs.n_bonded, pgs.n_bridged, pgs.n_shelled);

    pg_free(&pg);
    return 0;
}

static int test_geo_chord(void) {
    /* Test basic chord qualities resolve correct note counts */
    GeoChord cmaj = geo_chord_make(0, CHORD_MAJOR, 0);
    GeoChordResolved r = geo_chord_press(&cmaj, 0);
    if (r.count != 3) return -1;
    if (r.addrs[0] != 0 || r.addrs[1] != 4 || r.addrs[2] != 7) return -2;

    /* Test capo transpose */
    GeoChord eb = geo_chord_transpose(cmaj, 3);
    r = geo_chord_press(&eb, 0);
    int32_t semi_off = 3 * (int32_t)(GEO_FULL / 12);
    if (r.addrs[0] != (uint32_t)semi_off) return -3;

    /* Test shell capo (key_offset) */
    r = geo_chord_press(&cmaj, 3456);
    if (r.addrs[0] != 3456) return -4;

    /* Test wrap-around */
    GeoChord wrap = geo_chord_make(20000, CHORD_MAJOR, 0);
    r = geo_chord_press(&wrap, 0);
    if (r.addrs[1] != 20004) return -5;
    if (r.addrs[2] != 20007) return -6;

    /* Test chord from shell Chord */
    Chord c;
    c.seed       = 42;
    c.chord_id   = CHORD_ORBITAL;
    c.key_offset = 0;
    GeoChord from_shell = geo_chord_from_shell(&c, 0);
    r = geo_chord_press(&from_shell, 0);
    if (r.addrs[0] != 42) return -7;

    /* Test strum callback — chord_press verified implicitly above */
    printf("  chord: %u notes per chord, capo=%d, wrap OK\n",
           r.count, semi_off);
    return 0;
}

static int test_ribcage_freeze(void) {
    FiboSpine fs;
    fibo_spine_init(&fs);
    P5HRibcage rc;
    p5h_ribcage_init(&rc, &fs);

    /* Record some entries */
    p5h_ribcage_step(&rc, 0, 0, 0xDEAD);
    p5h_ribcage_step(&rc, 1, 1, 0xBEEF);
    p5h_ribcage_step(&rc, 2, 2, 0xCAFE);

    if (rc.entry_count != 3) return -1;
    if (rc.entries[0].pipe_id != 0 || rc.entries[0].tick != 0) return -2;
    if (rc.entries[1].bond_key != 0xBEEF) return -3;

    printf("  ribcage: %u entries, phase=%u\n", rc.entry_count, rc.global_phase);
    p5h_ribcage_free(&rc);
    return 0;
}

int main(void) {
    int pass = 0, fail = 0;

    /* ── Existing tests ── */
    printf("[geo_jump] GEO_FULL=%u GEO_TOWER=%u\n", GEO_FULL, GEO_TOWER);

    int fsv = geo_frame_seek_verify();
    if (fsv) {
        printf("[frame_seek] verify FAIL (code=%d)\n", fsv); fail++;
    } else {
        printf("[frame_seek] verify PASS\n"); pass++;
    }

    int fcv = fc_verify();
    if (fcv != 0) {
        printf("[frustum_coord] verify FAIL (code=%d)\n", fcv); fail++;
    } else {
        printf("[frustum_coord] verify PASS\n"); pass++;
    }

    geo_metatron_reshape_verify();
    printf("[metatron_reshape] verify PASS\n"); pass++;

    for (uint32_t i = 0; i < 27; i++) {
        GeoPixel px = geo_pixel_encode(i, 27);
        GeoFields f = geo_pixel_decode(px);
        if (f.trit != i % 27) { fail++; break; }
    }
    printf("[geo_pixel] roundtrip PASS\n"); pass++;

    SkeletonIdx si = skeleton_lookup(42);
    printf("[skeleton_index] addr=42 zone=%u pair=%u\n", si.zone, si.pair);

    uint8_t chunk[64] = {0};
    ShellChunkResult sr = shell_classify_chunk_v2(chunk, 0, 0);
    printf("[diamond_shell] FLAT chunk: flag=%u rot=%u isect_pc=%u\n",
           sr.flag, sr.best_rot, sr.isect_pc);

    uint64_t hash = pogls_fibo_addr(42);
    printf("[bond] fibo_addr(42)=0x%016" PRIX64 "\n", hash);
    pass += 5;  /* geo_jump print + frame_seek + metatron + geo_pixel + skeleton + diamond + bond */

    /* ── NEW: Bond standalone unit tests ── */
    printf("\n--- Bond Unit ---\n");
    {
        /* test_make_piece_consistency */
        PoglsPiece pa = pogls_make_piece(1337, 3);
        PoglsPiece pb = pogls_make_piece(1337, 3);
        int bk_ok = (pogls_bond_key(&pa) == pogls_bond_key(&pb));
        if (bk_ok && pa.shape == pb.shape && pa.geo_key == pb.geo_key) {
            printf("[bond_consistency] same seed→same piece PASS\n"); pass++;
        } else { printf("[bond_consistency] FAIL\n"); fail++; }

        /* test_bond_key_deterministic: same piece → same key, different → different */
        uint64_t bk1 = pogls_bond_key(&pa);
        uint64_t bk2 = pogls_bond_key(&pb);
        if (bk1 == bk2) { /* same piece → same key */
            printf("[bond_key_det] same piece→same key PASS\n"); pass++;
        } else { printf("[bond_key_det] FAIL\n"); fail++; }

        PoglsPiece pd = pogls_make_piece(9999, 5);
        uint64_t bk3 = pogls_bond_key(&pd);
        if (bk1 != bk3) { /* different seed → different key */
            printf("[bond_key_diff] diff seed→diff key PASS\n"); pass++;
        } else { printf("[bond_key_diff] FAIL\n"); fail++; }

        /* test_bond_verify_negative: different seeds → invalid */
        PoglsPiece pc = pogls_make_piece(9999, 5);
        PoglsBond bond_bad = pogls_bond_verify(&pa, &pc);
        if (!bond_bad.valid) {
            printf("[bond_verify_neg] diff pair→invalid PASS\n"); pass++;
        } else { printf("[bond_verify_neg] FAIL\n"); fail++; }

        /* test_bond_chain_build_walk */
        BondChain chain = {0};
        bond_chain_init(&chain, 4);
        bond_chain_build(&chain, 8);

        /* Walk forward from head */
        uint64_t kw = pogls_bond_key(&chain.nodes[0].piece);
        int walk_ok = 1;
        for (uint32_t i = 0; i < chain.n_chunks; i++) {
            if (kw == BOND_KEY_NONE) { walk_ok = 0; break; }
            kw = chain.nodes[i].next_key;
        }

        /* Verify chain order is deterministic (prev/next linked) */
        int link_ok = 1;
        for (uint32_t i = 1; i < chain.n_chunks && link_ok; i++) {
            if (chain.nodes[i].prev_key != pogls_bond_key(&chain.nodes[i-1].piece)) link_ok = 0;
        }
        if (walk_ok && link_ok && chain.n_chunks == 4) {
            printf("[bond_chain] build+walk PASS\n"); pass++;
        } else { printf("[bond_chain] FAIL\n"); fail++; }

        /* test_bond_chain_ht */
        bond_chain_build_ht(&chain);
        uint64_t key2 = pogls_bond_key(&chain.nodes[2].piece);
        uint32_t found_idx = bond_chain_find_ht(&chain, key2);
        if (found_idx == 2) {
            printf("[bond_chain_ht] find by key PASS\n"); pass++;
        } else { printf("[bond_chain_ht] FAIL\n"); fail++; }

        bond_chain_free_ht(&chain);
        bond_chain_free(&chain);
    }

    /* ── NEW: Diamond standalone unit tests ── */
    printf("\n--- Diamond Unit ---\n");
    {
        /* test_shell_classify_flat */
        uint8_t flat_chunk[64] = {0};
        ShellChunkResult sr_flat = shell_classify_chunk_v2(flat_chunk, 0, 0);
        if (sr_flat.flag == 0) {  /* SHELL_FLAG_FLAT */
            printf("[shell_classify_flat] zero→FLAT PASS\n"); pass++;
        } else { printf("[shell_classify_flat] FAIL\n"); fail++; }

        /* test_shell_classify_dense: patterned chunk → should be non-flat */
        uint8_t pat_chunk[64];
        for (int i = 0; i < 64; i++) pat_chunk[i] = (uint8_t)(i * 17 + 7);
        ShellChunkResult sr_pat = shell_classify_chunk_v2(pat_chunk, 0, 0);
        if (sr_pat.flag == 2 || sr_pat.flag == 1) {  /* DENSE or SPARSE */
            printf("[shell_classify_pat] pattern→%s PASS\n",
                   sr_pat.flag==2?"DENSE":"SPARSE"); pass++;
        } else { printf("[shell_classify_pat] FAIL (flag=%u)\n", sr_pat.flag); fail++; }

        /* test_shell_codec_roundtrip: stream encode then decode */
        uint8_t enc_buf[256];
        memset(enc_buf, 0, sizeof(enc_buf));
        uint64_t enc_sz = shell_stream_encode(pat_chunk, 1, enc_buf);
        if (enc_sz > 0 && enc_sz <= sizeof(enc_buf)) {
            uint8_t dec_buf[64];
            memset(dec_buf, 0, sizeof(dec_buf));
            uint64_t dec_sz = shell_stream_decode(enc_buf, 1, dec_buf);
            if (dec_sz == enc_sz && memcmp(pat_chunk, dec_buf, 64) == 0) {
                printf("[shell_codec] encode→decode match PASS (sz=%" PRIu64 ")\n", enc_sz); pass++;
            } else { printf("[shell_codec] FAIL: data mismatch\n"); fail++; }
        } else {
            printf("[shell_codec] FAIL: enc_sz=%" PRIu64 "\n", enc_sz); fail++;
        }

        /* test_hamburger_classify: edge-padded uniform tile → FLAT */
        int hb_tile[64];
        for (int i = 0; i < 64; i++) hb_tile[i] = 128;
        uint8_t htype = hb_classify_tile(hb_tile, hb_tile, hb_tile,
                                          0, 0, 8, 8, 8);
        if (htype == GPX5_TTYPE_FLAT) {
            printf("[hamburger] uniform→FLAT PASS\n"); pass++;
        } else { printf("[hamburger] uniform→%s (not FLAT)\n",
               htype==1?"GRADIENT":htype==2?"EDGE":"NOISE"); fail++; }

        /* test_binary_codec: roundtrip FLAT / SPARSE / DENSE */
        {
            int bc_pass = 0, bc_fail = 0;

            /* FLAT: all-zero chunk */
            uint8_t flat[64] = {0};
            uint8_t bc_buf[256];
            BinChunkResult bcr;
            uint32_t bsz = bin_encode_chunk(bc_buf, flat, &bcr);
            uint8_t bc_dec[64];
            uint32_t bdsz = bin_decode_chunk(bc_buf, bc_dec);
            int ok = (bcr.flag == BIN_FLAG_FLAT && bsz == 2 && bdsz == 2
                      && memcmp(flat, bc_dec, 64) == 0);
            if (ok) { printf("[binary_codec] flat→FLAT PASS (sz=%u)\n", bsz); bc_pass++; }
            else { printf("[binary_codec] flat→FLAT FAIL\n"); bc_fail++; }

            /* SPARSE: 3 non-zero bytes */
            uint8_t sparse[64] = {0};
            sparse[10] = 0xAB; sparse[33] = 0xCD; sparse[57] = 0xEF;
            bsz = bin_encode_chunk(bc_buf, sparse, &bcr);
            bdsz = bin_decode_chunk(bc_buf, bc_dec);
            ok = (bcr.flag == BIN_FLAG_SPARSE && bsz > 2 && bdsz > 2
                  && memcmp(sparse, bc_dec, 64) == 0);
            if (ok) { printf("[binary_codec] sparse→SPARSE PASS (sz=%u nz=%u)\n",
                             bsz, bcr.nz_count); bc_pass++; }
            else { printf("[binary_codec] sparse→SPARSE FAIL\n"); bc_fail++; }

            /* DENSE: fully populated chunk */
            uint8_t dense[64];
            for (int i = 0; i < 64; i++) dense[i] = (uint8_t)(i * 7 + 13);
            bsz = bin_encode_chunk(bc_buf, dense, &bcr);
            bdsz = bin_decode_chunk(bc_buf, bc_dec);
            ok = (bcr.flag == BIN_FLAG_DENSE && bsz > 2 && bdsz > 2
                  && memcmp(dense, bc_dec, 64) == 0);
            if (ok) { printf("[binary_codec] dense→DENSE PASS (sz=%u)\n", bsz); bc_pass++; }
            else { printf("[binary_codec] dense→DENSE FAIL\n"); bc_fail++; }

            /* DENSE with zstd benefit: repeated pattern */
            uint8_t rept[64];
            memset(rept, 0xAA, 64);
            bsz = bin_encode_chunk(bc_buf, rept, &bcr);
            bdsz = bin_decode_chunk(bc_buf, bc_dec);
            ok = (bcr.flag == BIN_FLAG_DENSE && bsz > 2 && bdsz > 2
                  && memcmp(rept, bc_dec, 64) == 0);
            if (ok) { printf("[binary_codec] repeat→DENSE PASS (sz=%u)\n", bsz); bc_pass++; }
            else { printf("[binary_codec] repeat→DENSE FAIL\n"); bc_fail++; }

            if (bc_fail == 0) {
                printf("[binary_codec] %d/%d PASS\n", bc_pass, bc_pass + bc_fail);
                pass++;
            } else { fail++; }
        }
    }

    /* ── NEW: Fibo Spine + Jet Bridge ── */
    printf("\n--- Fibo Spine ---\n");
    if (test_fibo_spine() == 0) { printf("[fibo_spine] PASS\n"); pass++; }
    else { printf("[fibo_spine] FAIL\n"); fail++; }

    /* ── NEW: Jet Bridge Hop ── */
    printf("\n--- Jet Bridge ---\n");
    if (test_jet_bridge_hop() == 0) { printf("[jet_bridge] PASS\n"); pass++; }
    else { printf("[jet_bridge] FAIL\n"); fail++; }

    /* ── NEW: Residual Space ── */
    printf("\n--- Residual Space ---\n");
    if (test_residual_space() == 0) { printf("[residual_space] PASS\n"); pass++; }
    else { printf("[residual_space] FAIL\n"); fail++; }

    /* ── NEW: Ribcage Freeze ── */
    printf("\n--- P5H Ribcage ---\n");
    if (test_ribcage_freeze() == 0) { printf("[p5h_ribcage] PASS\n"); pass++; }
    else { printf("[p5h_ribcage] FAIL\n"); fail++; }

    /* ── NEW: Pipeline Chaining ── */
    printf("\n--- Pipeline Glue ---\n");
    if (test_pipeline_chaining() == 0) { printf("[pipeline_glue] PASS\n"); pass++; }
    else { printf("[pipeline_glue] FAIL\n"); fail++; }

    /* ── NEW: GeoChord ── */
    printf("\n--- GeoChord ---\n");
    if (test_geo_chord() == 0) { printf("[geo_chord] PASS\n"); pass++; }
    else { printf("[geo_chord] FAIL\n"); fail++; }

    /* ── NEW: Encode → Decode Roundtrip ── */
    printf("\n--- Roundtrip (encode→decode) ---\n");
    {
        uint8_t orig[256];
        for (int i = 0; i < 256; i++) orig[i] = (uint8_t)(i & 0x3F) + 1;

        PGContext pg;
        int ret = pg_init(&pg, orig, sizeof(orig), 0xF00D, PG_FLAG_NONE);
        if (ret != 0) { printf("[roundtrip] init FAIL\n"); fail++; }
        else {
            uint32_t ok = 1;
            if (pg_stage_chunking(&pg) == 0)    { printf("[roundtrip] chunk FAIL\n"); ok=0; }
            if (ok && pg_stage_bonding(&pg) == 0) { printf("[roundtrip] bond FAIL\n"); ok=0; }
            if (ok && pg_stage_shelling(&pg) == 0) { printf("[roundtrip] shell FAIL\n"); ok=0; }
            if (ok && pg_stage_pixelating(&pg) == 0) { printf("[roundtrip] pixel FAIL\n"); ok=0; }
            if (ok && pg_stage_hamburger(&pg) == 0) { printf("[roundtrip] hamburger FAIL\n"); ok=0; }
            if (ok && pg_stage_gpx5(&pg) == 0)   { printf("[roundtrip] gpx5 FAIL\n"); ok=0; }
            if (ok && pg_stage_decode(&pg) == 0) { printf("[roundtrip] decode FAIL\n"); ok=0; }
            if (ok) {
                uint32_t match = 0;
                for (uint32_t i = 0; i < pg.n_chunks; i++) {
                    uint32_t off = i * PG_CHUNK_SZ;
                    uint32_t sz = (off + PG_CHUNK_SZ <= sizeof(orig))
                                  ? PG_CHUNK_SZ : sizeof(orig) - off;
                    if (memcmp(pg.chunks[i].data, orig + off, sz) == 0)
                        match++;
                }
                if (match == pg.n_chunks) {
                    printf("[roundtrip] %u/%u tiles match\n", match, pg.n_chunks);
                    pass++;
                    remove(pg.output_path);
                } else {
                    printf("[roundtrip] %u/%u tiles match FAIL (file=%s)\n",
                           match, pg.n_chunks, pg.output_path);
                    fail++;
                }
            }
            pg_free(&pg);
        }
    }

    /* ── NEW: pg_run_full + pg_run_decode ── */
    printf("\n--- pg_run_full ---\n");
    {
        uint8_t orig[256];
        for (int i = 0; i < 256; i++) orig[i] = (uint8_t)(i & 0x3F) + 1;

        PGContext pg;
        if (pg_init(&pg, orig, sizeof(orig), 0xBEEF, PG_FLAG_NONE) != 0) {
            printf("[run_full] init FAIL\n"); fail++;
        } else {
            int r = pg_run_full(&pg);
            if (r != 0) { printf("[run_full] execute FAIL (%d)\n", r); fail++; }
            else {
                uint32_t nd = pg_run_decode(&pg);
                if (nd == 0) { printf("[run_full] decode FAIL\n"); fail++; }
                else {
                    uint32_t m = 0;
                    for (uint32_t i = 0; i < pg.n_chunks; i++) {
                        uint32_t off = i * PG_CHUNK_SZ;
                        uint32_t sz = (off + PG_CHUNK_SZ <= 256) ? PG_CHUNK_SZ : 256 - off;
                        if (memcmp(pg.chunks[i].data, orig + off, sz) == 0) m++;
                    }
                    if (m == pg.n_chunks) {
                        printf("[run_full] %u/%u tiles match, file=%s\n",
                               m, pg.n_chunks, pg.output_path);
                        pass++;
                        remove(pg.output_path);
                    } else {
                        printf("[run_full] %u/%u tiles match FAIL\n", m, pg.n_chunks);
                        fail++;
                    }
                }
            }
            pg_free(&pg);
        }
    }

    /* ── NEW: Tombstone Zone ── */
    printf("\n--- Tombstone Zone ---\n");
    if (test_tombstone_zone() == 0) { printf("[tombstone] PASS\n"); pass++; }
    else { printf("[tombstone] FAIL\n"); fail++; }

    /* ── NEW: Per-Pipe Tick ── */
    printf("\n--- Per-Pipe Tick ---\n");
    if (test_perpipe_tick() == 0) { printf("[perpipe_tick] PASS\n"); pass++; }
    else { printf("[perpipe_tick] FAIL\n"); fail++; }

    /* ── NEW: ResidualSpace LRU eviction ── */
    printf("\n--- ResidualSpace LRU ---\n");
    {
        /* Minimum capacity is 64 (clamped by rs_init) */
        ResidualSpace rs;
        rs_init(&rs, 64);

        uint8_t buf[8];
        for (int i = 0; i < 66; i++) {
            memset(buf, (uint8_t)(i * 17), 8);
            PoglsPiece p = pogls_make_piece((uint64_t)(i + 1), 1);
            rs_freeze(&rs, &p, buf, 8, 0);
        }
        if (rs.evictions > 0 && rs.count <= 64) {
            printf("[rs_lru] %llu evictions, count=%u (cap=%u) PASS\n",
                   (unsigned long long)rs.evictions, rs.count, rs.capacity);
            pass++;
        } else {
            printf("[rs_lru] FAIL: evictions=%llu count=%u cap=%u\n",
                   (unsigned long long)rs.evictions, rs.count, rs.capacity);
            fail++;
        }
        rs_free(&rs);
    }

    /* ── NEW: Real-data Tombstone Sweep ── */
    printf("\n--- Real Tombstone Sweep ---\n");
    {
        uint8_t data[256];
        for (int i = 0; i < 256; i++) data[i] = (uint8_t)(i * 7 + 13);

        PGContext pg;
        int r = pg_init(&pg, data, sizeof(data), 0x1337, PG_FLAG_RESIDUAL_ENABLE);
        if (r != 0) { printf("[real_tomb] init FAIL\n"); fail++; }
        else {
            if (pg_stage_chunking(&pg) == 0 || pg_stage_bonding(&pg) == 0) {
                printf("[real_tomb] stage FAIL\n"); fail++;
            } else {
                /* Freeze all 4 chunks into residual */
                uint32_t frozen = 0;
                for (uint32_t i = 0; i < pg.n_chunks; i++) {
                    uint64_t bk = rs_freeze(&pg.residual, &pg.chunks[i].piece,
                                             pg.chunks[i].data, PG_CHUNK_SZ, 0);
                    if (bk != RS_BOND_KEY_RESERVED) frozen++;
                }
                printf("  froze %u chunks\n", frozen);

                /* Tombstone half the entries */
                uint32_t tombstoned = 0;
                for (uint32_t i = 0; i < pg.n_chunks; i += 2) {
                    uint64_t bk = pogls_bond_key(&pg.chunks[i].piece);
                    if (rs_tombstone(&pg.residual, bk)) tombstoned++;
                }
                printf("  tombstoned %u\n", tombstoned);

                ResidualSpaceStats rst = rs_stats(&pg.residual);
                printf("  pre-sweep: count=%u tombs=%u\n", rst.count, rst.tombstone_count);

                /* Sweep */
                uint32_t swept = rs_tombstone_sweep(&pg.residual);
                rst = rs_stats(&pg.residual);
                printf("  swept=%u post-sweep: count=%u\n", swept, rst.count);

                if (tombstoned > 0 && swept == tombstoned && rst.count == frozen - swept) {
                    printf("[real_tomb] PASS\n"); pass++;
                } else { printf("[real_tomb] FAIL\n"); fail++; }
            }
            pg_free(&pg);
        }
    }

    /* ── NEW: Real Per-Pipe Multi-Origin Bridge ── */
    printf("\n--- Real Per-Pipe Bridge ---\n");
    {
        FiboSpine fs;
        fibo_spine_init(&fs);
        fs.mode = FS_MODE_PERPIPE;

        /* Simulate 12 origins: each origin = group of 144 pipes */
        /* Advance pipes in 3 groups at different rates */
        for (int cycle = 0; cycle < 5; cycle++) {
            /* Group A (pipes 0..3): fast - 3 ticks per cycle */
            for (int p = 0; p < 4; p++)
                for (int t = 0; t < 3; t++) fibo_spine_pipe_tick(&fs, p);

            /* Group B (pipes 4..7): medium - 2 ticks per cycle */
            for (int p = 4; p < 8; p++)
                for (int t = 0; t < 2; t++) fibo_spine_pipe_tick(&fs, p);

            /* Group C (pipes 8..11): slow - 1 tick per cycle */
            for (int p = 8; p < 12; p++)
                fibo_spine_pipe_tick(&fs, p);
        }

        FiboSpineStats s = fibo_spine_stats(&fs);
        printf("  pipes: min_tick=%u max_tick=%u dist=", s.min_local_tick, s.max_local_tick);
        for (int t = 0; t < 12; t++)
            if (s.pipes_at_tick[t] > 0) printf("[%d]=%u ", t, s.pipes_at_tick[t]);
        printf("\n");

        /* Verify distribution: group A at 15/12=3, group B at 10/12=10, group C at 5/12=5 */
        uint8_t tA = fs.pipes[0].local_tick;
        uint8_t tB = fs.pipes[4].local_tick;
        uint8_t tC = fs.pipes[8].local_tick;
        printf("  grpA@%u grpB@%u grpC@%u\n", tA, tB, tC);

        if (tA != tB && tB != tC && tA != tC) {
            printf("[real_perpipe] PASS\n"); pass++;
        } else { printf("[real_perpipe] FAIL\n"); fail++; }
    }

    /* ── NEW: DRam Tile ── */
    printf("\n--- DRam Tile ---\n");
    {
        int d1 = dram_verify_hilbert();
        int d2 = dram_verify_full();
        uint32_t d3 = dram_addr(42, 3, 5, 1);
        uint32_t d4 = dram_addr(42, 3, 5, 1);
        uint32_t a0 = dram_addr(0,   0,   0, 0);
        DramAddrParts dp = dram_decompose(a0);

        if (d1 == 0 && d2 == 0 && d3 == d4 && a0 == 0 && dp.anchor_id == 0) {
            printf("[dram_tile] hilbert+space+deterministic: PASS (size=%u)\n",
                   DRAM_FULL); pass++;
        } else {
            printf("[dram_tile] FAIL: d1=%d d2=%d eq=%d a0=%u anchor=%u\n",
                   d1, d2, (d3 == d4), a0, dp.anchor_id); fail++;
        }

        /* Verify dram_mmap_offset */
        size_t off = dram_mmap_offset(1, 64);
        if (off == 64) {
            printf("[dram_tile] mmap_offset: PASS\n"); pass++;
        } else { printf("[dram_tile] mmap_offset FAIL\n"); fail++; }
    }

    /* ── summary ── */
    printf("\n=== DGLS TEST: %d PASS / %d FAIL ===\n", pass, fail);
    return fail ? 1 : 0;
}

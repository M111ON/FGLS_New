/*
 * test_pogls_hc_geojump.c — GeoJump × Hilbert L-Block Container tests
 *
 * Build:
 *   gcc -O2 -std=c11 -I../../geopixel/include -I. test_pogls_hc_geojump.c -o test_pogls_hc_geojump.exe
 *
 * Run:
 *   .\test_pogls_hc_geojump.exe
 */

#define GEO_JUMP_INLINE
#include "pogls_hc_geojump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass = 0, fail = 0;

#define TEST(name, cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s] line %d\n", name, __LINE__); \
        fail++; \
    } else { \
        pass++; \
    } \
} while(0)

static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

/* ═══════════════════════════════════════════════════════════════════════
   T1: Metatron constants sanity
   ═══════════════════════════════════════════════════════════════════════ */
static void test_constants(void)
{
    TEST("T1a METATRON_COLS=4",   GEO_METATRON_COLS == 4);
    TEST("T1b METATRON_ROWS=4",   GEO_METATRON_ROWS == 4);
    TEST("T1c METATRON_FLOORS=3", GEO_METATRON_FLOORS == 3);
    TEST("T1d METATRON_CELLS=16", GEO_METATRON_CELLS == 16);
    TEST("T1e GEO_BLOCK=48",      GEO_BLOCK == 48);
    TEST("T1f GEO_TOWER=144",     GEO_TOWER == 144);
    TEST("T1g GEO_FULL=20736",    GEO_FULL == 20736);
    TEST("T1h FLOOR_UNIT_SZ=1040", HC_GJ_FLOOR_UNIT_SZ == 1040);
    TEST("T1i BLOCK_UNIT_SZ=3152", HC_GJ_BLOCK_UNIT_SZ == 3152);
    TEST("T1j HEADER_SZ=20",      HC_GJ_HEADER_SZ == 20);
}

/* ═══════════════════════════════════════════════════════════════════════
   T2: Hilbert cell mapping within 4×4 floor
   ═══════════════════════════════════════════════════════════════════════ */
static void test_hilbert_floor(void)
{
    /* Verify Hilbert indices are a permutation of 0..15 */
    uint32_t seen[16] = {0};
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t h = hc_gj_cell_to_hilbert(i);
        TEST("T2a hilbert in range", h < 16);
        if (h < 16) seen[h]++;
    }
    int unique = 1;
    for (int i = 0; i < 16; i++) {
        if (seen[i] != 1) { unique = 0; break; }
    }
    TEST("T2b all 16 cells unique hilbert", unique);

    /* Roundtrip: cell→hilbert→cell */
    int ok = 1;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t h = hc_gj_cell_to_hilbert(i);
        uint32_t c = hc_gj_hilbert_to_cell(h);
        if (c != i) { ok = 0; break; }
    }
    TEST("T2c cell→hilbert→cell roundtrip", ok);
}

/* ═══════════════════════════════════════════════════════════════════════
   T3: geo_jump node decomposition
   ═══════════════════════════════════════════════════════════════════════ */
static void test_node_decompose(void)
{
    /* node 0 → tower=0, block=0, floor=0, cell=0 */
    uint32_t tower, block, floor, cell;
    hc_gj_node_decompose(0, &tower, &block, &floor, &cell);
    TEST("T3a node 0 tower=0", tower == 0);
    TEST("T3b node 0 block=0", block == 0);
    TEST("T3c node 0 floor=0", floor == 0);
    TEST("T3d node 0 cell=0",  cell == 0);

    /* node 48 (tower 0, block 1, floor 0, cell 0) */
    hc_gj_node_decompose(48, &tower, &block, &floor, &cell);
    TEST("T3e node 48 tower=0", tower == 0);
    TEST("T3f node 48 block=1", block == 1);
    TEST("T3g node 48 floor=0", floor == 0);
    TEST("T3h node 48 cell=0",  cell == 0);

    /* node 143 (tower 0, block 2, floor 2, cell 15) */
    hc_gj_node_decompose(143, &tower, &block, &floor, &cell);
    TEST("T3i node 143 tower=0", tower == 0);
    TEST("T3j node 143 block=2", block == 2);
    TEST("T3k node 143 floor=2", floor == 2);
    TEST("T3l node 143 cell=15",  cell == 15);

    /* Compose roundtrip */
    uint32_t n = hc_gj_node_compose(0, 1, 2, 7);
    TEST("T3m compose(0,1,2,7)=87", n == 87);
    hc_gj_node_decompose(n, &tower, &block, &floor, &cell);
    TEST("T3n roundtrip tower=0", tower == 0);
    TEST("T3o roundtrip block=1", block == 1);
    TEST("T3p roundtrip floor=2", floor == 2);
    TEST("T3q roundtrip cell=7",   cell == 7);
}

/* ═══════════════════════════════════════════════════════════════════════
   T4: Writer init + cell write
   ═══════════════════════════════════════════════════════════════════════ */
static void test_writer(void)
{
    HC_GeoJumpWriter w;
    TEST("T4a writer init 3 blocks tower 0",
         hc_gj_writer_init(&w, 3, 0) == 0);
    TEST("T4b block_count=3", w.hdr.block_count == 3);
    TEST("T4c tower_id=0", w.hdr.tower_id == 0);

    /* Write 3 blocks × 48 cells = 144 cells */
    uint8_t cell[64];
    memset(cell, 0xAB, 64);
    for (int i = 0; i < 144; i++) {
        cell[0] = (uint8_t)i;
        int blk = hc_gj_write_cell(&w, cell);
        TEST("T4d write returns block", blk >= 0);
    }

    /* After 144 cells, cur_block should be 3 (all full) */
    TEST("T4e all blocks filled", w.cur_block == 3);

    /* Verify data in block 0, floor 0, cell 0 */
    uint8_t expected[64];
    memset(expected, 0xAB, 64);
    expected[0] = 0;
    const uint8_t *got = w.data;
    TEST("T4f block0/floor0/cell0 data", memcmp(got, expected, 64) == 0);

    /* Verify data in block 1, floor 0, cell 0 (should have cell[0]=48) */
    size_t off = HC_GJ_BLOCK_UNIT_SZ;
    expected[0] = 48;
    TEST("T4g block1/floor0/cell0 data", memcmp(w.data + off, expected, 64) == 0);

    hc_gj_writer_destroy(&w);
}

/* ═══════════════════════════════════════════════════════════════════════
   T5: Floor connector + block connector write
   ═══════════════════════════════════════════════════════════════════════ */
static void test_connectors(void)
{
    HC_GeoJumpWriter w;
    hc_gj_writer_init(&w, 2, 5);

    uint8_t fconn[16];
    uint8_t bconn[32];

    /* Write floor connectors for block 0 */
    memset(fconn, 0xF1, 16);
    hc_gj_write_floor_conn(&w, 0, 0, fconn);
    memset(fconn, 0xF2, 16);
    hc_gj_write_floor_conn(&w, 0, 1, fconn);
    memset(fconn, 0xF3, 16);
    hc_gj_write_floor_conn(&w, 0, 2, fconn);

    /* Write block connector for block 0 */
    memset(bconn, 0xBC, 32);
    hc_gj_write_block_conn(&w, 0, bconn);

    /* Serialize + deserialize roundtrip */
    size_t sz = hc_gj_serialized_size(&w);
    TEST("T5a serialized size", sz == HC_GJ_HEADER_SZ + 2 * HC_GJ_BLOCK_UNIT_SZ);

    uint8_t *buf = (uint8_t *)malloc(sz);
    size_t written = hc_gj_serialize(&w, buf, sz);
    TEST("T5b serialize ok", written == sz);

    HC_GeoJumpReader r;
    TEST("T5c reader init", hc_gj_reader_init(&r, buf, sz) == 0);
    TEST("T5d verify", hc_gj_verify(&r) == 0);
    TEST("T5e tower_id=5", r.hdr.tower_id == 5);

    /* Check floor connector roundtrip */
    memset(fconn, 0xF1, 16);
    const uint8_t *gfconn = hc_gj_get_floor_conn(&r, 0, 0);
    TEST("T5f floor conn block0/floor0", gfconn && memcmp(gfconn, fconn, 16) == 0);

    /* Check block connector roundtrip */
    memset(bconn, 0xBC, 32);
    const uint8_t *gbconn = hc_gj_get_block_conn(&r, 0);
    TEST("T5g block conn roundtrip", gbconn && memcmp(gbconn, bconn, 32) == 0);

    free(buf);
    hc_gj_writer_destroy(&w);
}

/* ═══════════════════════════════════════════════════════════════════════
   T6: File I/O roundtrip
   ═══════════════════════════════════════════════════════════════════════ */
static void test_file_io(void)
{
    HC_GeoJumpWriter w;
    hc_gj_writer_init(&w, 2, 10);

    /* Fill with deterministic data */
    uint32_t seed = 0xCAFEBABE;
    uint8_t cell[64];
    for (int i = 0; i < 96; i++) {  /* 2 blocks × 48 cells */
        for (int b = 0; b < 64; b++)
            cell[b] = (uint8_t)xs32(&seed);
        hc_gj_write_cell(&w, cell);
    }

    /* Write connectors */
    uint8_t fconn[16] = {0xF0};
    uint8_t bconn[32] = {0xB0};
    for (uint32_t b = 0; b < 2; b++) {
        for (uint32_t f = 0; f < 3; f++) {
            fconn[0] = (uint8_t)(0xA0 + b * 3 + f);
            hc_gj_write_floor_conn(&w, b, f, fconn);
        }
        bconn[0] = (uint8_t)(0xD0 + b);
        hc_gj_write_block_conn(&w, b, bconn);
    }

    /* Write to file */
    const char *path = "test_gj_out.bin";
    TEST("T6a write file", hc_gj_write_file(path, &w) == 0);

    /* Read back */
    HC_GeoJumpReader r;
    void *data = NULL;
    size_t data_sz = 0;
    int err = hc_gj_read_file(path, &r, &data, &data_sz);
    TEST("T6b read file", err == 0);
    TEST("T6c verify", hc_gj_verify(&r) == 0);

    /* Check all cells */
    seed = 0xCAFEBABE;
    int all_ok = 1;
    for (int b = 0; b < 2 && all_ok; b++) {
        for (int f = 0; f < 3 && all_ok; f++) {
            for (int c = 0; c < 16 && all_ok; c++) {
                uint8_t expected[64];
                for (int k = 0; k < 64; k++)
                    expected[k] = (uint8_t)xs32(&seed);
                const uint8_t *got = hc_gj_get_cell(&r, (uint32_t)b,
                                                      (uint32_t)f, (uint32_t)c);
                if (!got || memcmp(got, expected, 64) != 0)
                    all_ok = 0;
            }
        }
    }
    TEST("T6d all cells match", all_ok);

    /* Check node-based access */
    seed = 0xCAFEBABE;
    int node_ok = 1;
    for (int i = 0; i < 96 && node_ok; i++) {
        uint8_t expected[64];
        for (int k = 0; k < 64; k++)
            expected[k] = (uint8_t)xs32(&seed);
        /* node_id = tower * GEO_TOWER + i within tower */
        uint32_t node_id = 10 * GEO_TOWER + (uint32_t)i;
        const uint8_t *got = hc_gj_get_node(&r, node_id);
        if (!got || memcmp(got, expected, 64) != 0)
            node_ok = 0;
    }
    TEST("T6e node-based access matches", node_ok);

    free(data);
    hc_gj_writer_destroy(&w);
    remove(path);
}

/* ═══════════════════════════════════════════════════════════════════════
   T7: geo_jump integration — Hilbert walk within container
   ═══════════════════════════════════════════════════════════════════════ */
static void test_geojump_walk(void)
{
    /* Write data to block 0, floor 0, cells 0..15 */
    HC_GeoJumpWriter w;
    hc_gj_writer_init(&w, 1, 0);

    uint8_t cell[64];
    for (int i = 0; i < 16; i++) {
        memset(cell, (uint8_t)(i * 10), 64);
        hc_gj_write_cell(&w, cell);
    }

    size_t sz = hc_gj_serialized_size(&w);
    uint8_t *buf = (uint8_t *)malloc(sz);
    hc_gj_serialize(&w, buf, sz);

    HC_GeoJumpReader r;
    hc_gj_reader_init(&r, buf, sz);

    /* Hilbert walk: use _jump_hilbert to navigate */
    uint32_t base_node = 0;  /* tower 0, block 0, floor 0 */
    int ok = 1;
    for (uint32_t col = 1; col <= 4 && ok; col++) {
        for (uint32_t row = 1; row <= 4 && ok; row++) {
            uint32_t node = _jump_hilbert(base_node, col, row, 1);
            const uint8_t *got = hc_gj_get_node(&r, node);
            /* Expected: hilbert_cell = _hilbert_idx(col-1, row-1, 4) */
            uint32_t hilbert_d = _hilbert_idx(col - 1, row - 1, 4);
            uint8_t expected[64];
            memset(expected, (uint8_t)(hilbert_d * 10), 64);
            if (!got || memcmp(got, expected, 64) != 0) ok = 0;
        }
    }
    TEST("T7 geo_jump Hilbert walk matches container data", ok);

    free(buf);
    hc_gj_writer_destroy(&w);
}

/* ═══════════════════════════════════════════════════════════════════════
   T8: L-block node mapping
   ═══════════════════════════════════════════════════════════════════════ */
static void test_lblock_nodes(void)
{
    uint32_t nodes[3];
    uint32_t conn;

    /* L-block 0 on floor 0: should be cells 0,1,2 + connector 3 */
    hc_gj_lblock_nodes(0, 0, 0, 0, nodes, &conn);
    uint32_t expected_nodes[3];
    /* cell 0 → hilbert_d=0 → _jump_hilbert(node, 1,1,1) */
    expected_nodes[0] = _jump_hilbert(0, 1, 1, 1);  /* col=1,row=1 → hilbert=0 */
    expected_nodes[1] = _jump_hilbert(0, 2, 1, 1);  /* col=2,row=1 → hilbert=1 */
    expected_nodes[2] = _jump_hilbert(0, 2, 2, 1);  /* col=2,row=2 → hilbert=2 */
    uint32_t expected_conn = _jump_hilbert(0, 1, 2, 1);  /* col=1,row=2 → hilbert=3 */

    TEST("T8a lblock0 node0", nodes[0] == expected_nodes[0]);
    TEST("T8b lblock0 node1", nodes[1] == expected_nodes[1]);
    TEST("T8c lblock0 node2", nodes[2] == expected_nodes[2]);
    TEST("T8d lblock0 conn",  conn == expected_conn);

    /* L-block 3 (last in floor): d=12,13,14 + connector d=15 */
    hc_gj_lblock_nodes(0, 0, 0, 3, nodes, &conn);

    /* Verify: floor_base = 0, so nodes should be d=12,13,14,15 */
    TEST("T8e lblock3 node0 d=12", nodes[0] == 12);
    TEST("T8f lblock3 node1 d=13", nodes[1] == 13);
    TEST("T8g lblock3 node2 d=14", nodes[2] == 14);
    TEST("T8h lblock3 conn d=15",  conn == 15);
}

/* ═══════════════════════════════════════════════════════════════════════
   T9: Edge cases
   ═══════════════════════════════════════════════════════════════════════ */
static void test_edge_cases(void)
{
    HC_GeoJumpWriter w;
    TEST("T9a block_count=0 rejected", hc_gj_writer_init(&w, 0, 0) == -1);
    TEST("T9b block_count=145 rejected", hc_gj_writer_init(&w, 145, 0) == -1);

    /* Writer full */
    hc_gj_writer_init(&w, 1, 0);
    uint8_t cell[64] = {0};
    for (int i = 0; i < 48; i++)
        hc_gj_write_cell(&w, cell);
    TEST("T9c write after full returns -1", hc_gj_write_cell(&w, cell) == -1);

    /* Reader with bad magic */
    uint8_t bad[32] = {0};
    HC_GeoJumpReader r;
    TEST("T9d bad magic rejected", hc_gj_reader_init(&r, bad, 32) == -2);

    hc_gj_writer_destroy(&w);
}

/* ═══════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("=== GeoJump × Hilbert L-Block Container Tests ===\n");

    test_constants();
    test_hilbert_floor();
    test_node_decompose();
    test_writer();
    test_connectors();
    test_file_io();
    test_geojump_walk();
    test_lblock_nodes();
    test_edge_cases();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

/*
 * test_pogls_hilbert_container.c — Hilbert L-Block Container test suite
 *
 * Build:
 *   gcc -O2 -std=c11 -I. test_pogls_hilbert_container.c -o test_pogls_hilbert_container.exe
 *
 * Run:
 *   .\test_pogls_hilbert_container.exe
 */

#include "pogls_hilbert_container.h"
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

/* deterministic 32-bit PRNG (xorshift) */
static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

/* ═══════════════════════════════════════════════════════════════════════
   T1: Hilbert curve correctness — order 1 (2×2 = 4 cells)
   ═══════════════════════════════════════════════════════════════════════ */
static void test_hilbert_order1(void)
{
    /* Order 1: 2×2 grid, 4 cells */
    uint32_t seen[4] = {0};
    for (uint32_t y = 0; y < 2; y++) {
        for (uint32_t x = 0; x < 2; x++) {
            uint32_t d = hc_hilbert_xy_to_d(x, y, 1);
            TEST("T1a d in range", d < 4);
            seen[d]++;
        }
    }
    int unique = 1;
    for (int i = 0; i < 4; i++) {
        if (seen[i] != 1) { unique = 0; break; }
    }
    TEST("T1b all 4 cells visited exactly once", unique);

    /* Roundtrip */
    for (uint32_t d = 0; d < 4; d++) {
        uint32_t x, y;
        hc_hilbert_d_to_xy(d, 1, &x, &y);
        uint32_t d2 = hc_hilbert_xy_to_d(x, y, 1);
        TEST("T1c d->xy->d roundtrip", d2 == d);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   T2: Hilbert curve correctness — order 2 (4×4 = 16 cells)
   ═══════════════════════════════════════════════════════════════════════ */
static void test_hilbert_order2(void)
{
    uint32_t seen[16] = {0};
    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            uint32_t d = hc_hilbert_xy_to_d(x, y, 2);
            TEST("T2a d in range", d < 16);
            if (d < 16) seen[d]++;
        }
    }
    int unique = 1;
    for (int i = 0; i < 16; i++) {
        if (seen[i] != 1) { unique = 0; break; }
    }
    TEST("T2b all 16 cells visited exactly once", unique);

    /* Roundtrip */
    int ok = 1;
    for (uint32_t d = 0; d < 16; d++) {
        uint32_t x, y;
        hc_hilbert_d_to_xy(d, 2, &x, &y);
        if (x >= 4 || y >= 4) { ok = 0; break; }
        uint32_t d2 = hc_hilbert_xy_to_d(x, y, 2);
        if (d2 != d) { ok = 0; break; }
    }
    TEST("T2c d->xy->d roundtrip all 16", ok);
}

/* ═══════════════════════════════════════════════════════════════════════
   T3: L-block mapping — block_count and addresses
   ═══════════════════════════════════════════════════════════════════════ */
static void test_lblock_mapping(void)
{
    /* Order 1: 4 cells → 1 L-block */
    TEST("T3a order1 blocks=1", hc_block_count(1) == 1);
    TEST("T3b order1 cells=4", hc_total_cells(1) == 4);

    /* Order 2: 16 cells → 4 L-blocks */
    TEST("T3c order2 blocks=4", hc_block_count(2) == 4);
    TEST("T3d order2 cells=16", hc_total_cells(2) == 16);

    /* Order 3: 64 cells → 16 L-blocks */
    TEST("T3e order3 blocks=16", hc_block_count(3) == 16);
    TEST("T3f order3 cells=64", hc_total_cells(3) == 64);

    /* Block addresses: block[i] → [4i, 4i+1, 4i+2] */
    uint32_t addrs[3];
    hc_block_addrs(0, 2, addrs);
    TEST("T3g block0 addr[0]=0", addrs[0] == 0);
    TEST("T3h block0 addr[1]=1", addrs[1] == 1);
    TEST("T3i block0 addr[2]=2", addrs[2] == 2);

    hc_block_addrs(3, 2, addrs);
    TEST("T3j block3 addr[0]=12", addrs[0] == 12);
    TEST("T3k block3 addr[1]=13", addrs[1] == 13);
    TEST("T3l block3 addr[2]=14", addrs[2] == 14);

    /* Connector address: block[i] → 4i+3 */
    TEST("T3m block0 conn=3", hc_conn_addr(0) == 3);
    TEST("T3n block3 conn=15", hc_conn_addr(3) == 15);
}

/* ═══════════════════════════════════════════════════════════════════════
   T4: Writer init + cell write — fill all cells for order 2
   ═══════════════════════════════════════════════════════════════════════ */
static void test_writer_cells(void)
{
    HilbertContainerWriter w;
    TEST("T4a writer init order 2", hc_writer_init(&w, 2) == 0);
    TEST("T4b block_count=4", w.hdr.block_count == 4);

    /* order 2: 4 blocks × 3 data cells = 12 data cells */
    uint32_t data_cells = hc_block_count(2) * HC_LBLOCK_CELLS;
    TEST("T4b2 data_cells=12", data_cells == 12);

    uint32_t seed = 0xDEADBEEF;
    uint8_t cells[12][64];
    for (int i = 0; i < 12; i++) {
        for (int b = 0; b < 64; b++)
            cells[i][b] = (uint8_t)xs32(&seed);
    }

    /* Write 12 data cells */
    for (int i = 0; i < 12; i++) {
        int blk = hc_write_cell(&w, cells[i]);
        TEST("T4c write returns block index", blk >= 0);
    }

    /* After 12 cells, cur_block should be 4 (all blocks full) */
    TEST("T4d all blocks filled", w.cur_block == 4);

    /* 13th write should fail */
    TEST("T4c2 write after full returns -1", hc_write_cell(&w, cells[0]) == -1);

    /* Verify data in blocks */
    int data_ok = 1;
    for (int i = 0; i < 12; i++) {
        uint32_t bi = (uint32_t)i / HC_LBLOCK_CELLS;
        uint32_t ci = (uint32_t)i % HC_LBLOCK_CELLS;
        if (memcmp(w.blocks[bi].data[ci], cells[i], 64) != 0) {
            data_ok = 0;
            break;
        }
    }
    TEST("T4e cell data matches in blocks", data_ok);

    hc_writer_destroy(&w);
}

/* ═══════════════════════════════════════════════════════════════════════
   T5: Connector write + serialize
   ═══════════════════════════════════════════════════════════════════════ */
static void test_connector_serialize(void)
{
    HilbertContainerWriter w;
    hc_writer_init(&w, 2);

    /* Write 12 data cells (4 blocks × 3 cells) */
    uint8_t cell[64];
    memset(cell, 0x42, 64);
    for (int i = 0; i < 12; i++) {
        hc_write_cell(&w, cell);
        cell[0] = (uint8_t)(i + 1);
    }

    /* Write connectors for each block */
    uint8_t conn[32];
    for (uint32_t i = 0; i < 4; i++) {
        memset(conn, (int)(0xA0 + i), 32);
        hc_write_connector(&w, i, conn);
    }

    TEST("T5a has_conn flag set", (w.hdr.flags & HC_FLAG_HAS_CONN) != 0);

    /* Serialize */
    size_t sz = hc_serialized_size(&w);
    TEST("T5b serialized size", sz == HC_HEADER_SZ + 4 * HC_LBLOCK_UNIT_SZ);

    uint8_t *buf = (uint8_t *)malloc(sz);
    size_t written = hc_serialize(&w, buf, sz);
    TEST("T5c serialize returns sz", written == sz);

    /* Deserialize into reader */
    HilbertContainerReader r;
    TEST("T5d reader init", hc_reader_init(&r, buf, sz) == 0);
    TEST("T5e verify passes", hc_verify(&r) == 0);
    TEST("T5f block_count=4", r.hdr.block_count == 4);

    /* Check cell data roundtrip */
    int cell_ok = 1;
    uint8_t expected[64];
    memset(expected, 0x42, 64);
    const uint8_t *got = hc_get_cell(&r, 0, 0);
    if (!got || memcmp(got, expected, 64) != 0) cell_ok = 0;
    TEST("T5g cell roundtrip block0/cell0", cell_ok);

    /* Check connector roundtrip */
    int conn_ok = 1;
    memset(conn, 0xA0, 32);
    const uint8_t *gconn = hc_get_connector(&r, 0);
    if (!gconn || memcmp(gconn, conn, 32) != 0) conn_ok = 0;
    TEST("T5h connector roundtrip block0", conn_ok);

    free(buf);
    hc_writer_destroy(&w);
}

/* ═══════════════════════════════════════════════════════════════════════
   T6: File I/O roundtrip
   ═══════════════════════════════════════════════════════════════════════ */
static void test_file_io(void)
{
    HilbertContainerWriter w;
    hc_writer_init(&w, 2);

    /* Fill with deterministic data — 12 data cells */
    uint32_t seed = 0xCAFEBABE;
    for (int i = 0; i < 12; i++) {
        uint8_t cell[64];
        for (int b = 0; b < 64; b++)
            cell[b] = (uint8_t)xs32(&seed);
        hc_write_cell(&w, cell);
    }

    /* Write connectors */
    uint8_t conn[32];
    for (uint32_t i = 0; i < 4; i++) {
        memset(conn, (int)(0xB0 + i), 32);
        hc_write_connector(&w, i, conn);
    }

    /* Write to file */
    const char *path = "test_hc_out.bin";
    TEST("T6a write file", hc_write_file(path, &w) == 0);

    /* Read back */
    HilbertContainerReader r;
    void *data = NULL;
    size_t data_sz = 0;
    int err = hc_read_file(path, &r, &data, &data_sz);
    TEST("T6b read file", err == 0);
    TEST("T6c verify", hc_verify(&r) == 0);

    /* Check all cells — 12 data cells */
    seed = 0xCAFEBABE;
    int all_ok = 1;
    for (int i = 0; i < 12; i++) {
        uint8_t expected[64];
        for (int b = 0; b < 64; b++)
            expected[b] = (uint8_t)xs32(&seed);
        uint32_t bi = (uint32_t)i / HC_LBLOCK_CELLS;
        uint32_t ci = (uint32_t)i % HC_LBLOCK_CELLS;
        const uint8_t *got = hc_get_cell(&r, bi, ci);
        if (!got || memcmp(got, expected, 64) != 0) {
            all_ok = 0;
            break;
        }
    }
    TEST("T6d all cells match after file roundtrip", all_ok);

    /* Check connectors */
    int conn_ok = 1;
    for (uint32_t i = 0; i < 4; i++) {
        memset(conn, (int)(0xB0 + i), 32);
        const uint8_t *gconn = hc_get_connector(&r, i);
        if (!gconn || memcmp(gconn, conn, 32) != 0) {
            conn_ok = 0;
            break;
        }
    }
    TEST("T6e all connectors match", conn_ok);

    free(data);
    hc_writer_destroy(&w);
    remove(path);
}

/* ═══════════════════════════════════════════════════════════════════════
   T7: Hilbert spatial locality — adjacent cells in curve are nearby
   ═══════════════════════════════════════════════════════════════════════ */
static void test_spatial_locality(void)
{
    /* For order 3 (8×8), verify that consecutive Hilbert indices
     * map to cells that are Manhattan-distance ≤ 2 apart */
    int ok = 1;
    for (uint32_t d = 0; d < 63; d++) {
        uint32_t x1, y1, x2, y2;
        hc_hilbert_d_to_xy(d, 3, &x1, &y1);
        hc_hilbert_d_to_xy(d + 1, 3, &x2, &y2);
        int dx = (x1 > x2) ? (int)(x1 - x2) : (int)(x2 - x1);
        int dy = (y1 > y2) ? (int)(y1 - y2) : (int)(y2 - y1);
        if (dx + dy > 2) { ok = 0; break; }
    }
    TEST("T7 consecutive Hilbert cells within manhattan dist 2", ok);
}

/* ═══════════════════════════════════════════════════════════════════════
   T8: Spatial lookup — find block from Hilbert address
   ═══════════════════════════════════════════════════════════════════════ */
static void test_spatial_lookup(void)
{
    HilbertContainerWriter w;
    hc_writer_init(&w, 3);  /* order 3: 64 cells, 16 blocks × 3 = 48 data cells */

    uint8_t cell[64] = {0};
    for (int i = 0; i < 48; i++) {
        cell[0] = (uint8_t)i;
        hc_write_cell(&w, cell);
    }

    size_t sz = hc_serialized_size(&w);
    uint8_t *buf = (uint8_t *)malloc(sz);
    hc_serialize(&w, buf, sz);

    HilbertContainerReader r;
    hc_reader_init(&r, buf, sz);

    /* Find block for d=27 (block=6, cell=3 → connector) */
    uint32_t bi, ci;
    int is_conn = hc_find_block(&r, 27, &bi, &ci);
    TEST("T8a d=27 is connector", is_conn == 1);
    TEST("T8b d=27 block=6", bi == 6);
    TEST("T8c d=27 cell=3", ci == 3);

    /* Find block for d=10 (block=2, cell=2) */
    is_conn = hc_find_block(&r, 10, &bi, &ci);
    TEST("T8d d=10 is normal cell", is_conn == 0);
    TEST("T8e d=10 block=2", bi == 2);
    TEST("T8f d=10 cell=2", ci == 2);

    free(buf);
    hc_writer_destroy(&w);
}

/* ═══════════════════════════════════════════════════════════════════════
   T9: Edge cases — writer full, invalid params
   ═══════════════════════════════════════════════════════════════════════ */
static void test_edge_cases(void)
{
    /* Writer init with invalid order */
    HilbertContainerWriter w;
    TEST("T9a order 0 rejected", hc_writer_init(&w, 0) == -1);
    TEST("T9b order 9 rejected", hc_writer_init(&w, 9) == -1);

    /* Write to full writer */
    hc_writer_init(&w, 1);  /* 1 block = 4 cells = 3 data cells */
    uint8_t cell[64] = {0};
    hc_write_cell(&w, cell);
    hc_write_cell(&w, cell);
    hc_write_cell(&w, cell);
    TEST("T9c write after full returns -1", hc_write_cell(&w, cell) == -1);

    /* Reader with bad magic */
    uint8_t bad[32] = {0};
    HilbertContainerReader r;
    TEST("T9d bad magic rejected", hc_reader_init(&r, bad, 32) == -2);

    hc_writer_destroy(&w);
}

/* ═══════════════════════════════════════════════════════════════════════
   T10: L-block unit size check
   ═══════════════════════════════════════════════════════════════════════ */
static void test_unit_size(void)
{
    /* 3 cells × 64B + 32B connector = 224B per L-block unit */
    TEST("T10a lblock data = 192", HC_LBLOCK_DATA_SZ == 192);
    TEST("T10b lblock unit = 224", HC_LBLOCK_UNIT_SZ == 224);
    TEST("T10c header = 20", HC_HEADER_SZ == 20);

    /* HilbertLBlock should be ≥ 236 bytes (224 data + 12 for addr[]) */
    TEST("T10d sizeof(HilbertLBlock) >= 236",
         sizeof(HilbertLBlock) >= 236);
}

/* ═══════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("=== Hilbert L-Block Container Tests ===\n");

    test_hilbert_order1();
    test_hilbert_order2();
    test_lblock_mapping();
    test_writer_cells();
    test_connector_serialize();
    test_file_io();
    test_spatial_locality();
    test_spatial_lookup();
    test_edge_cases();
    test_unit_size();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

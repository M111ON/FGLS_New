/*
 * test_pipeline_integration.c — Pipeline Split+Arrangement+Pull Integration Test
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Tests the full pipeline chain end-to-end:
 *   CHUNKING → BONDING → SHELLING → PIXELATING → ARRANGE → SPLIT → PULL
 *
 * ARRANGE:  sort chunks by Hilbert z-order + attach sequence headers
 * SPLIT:    payload (64B) → DRamTile-like buffer, metadata → CPU schedule
 * PULL:     Gear Lock readback from DRamTile → verify roundtrip
 *
 * This is the CPU-side integration test. The GPU puller (gpu_jet_puller.cu)
 * handles the real GPU-side batch pull via cudaHostRegister.
 *
 * Build:
 *   gcc -O2 -std=c11 -I. -I../diamond/include -I../geo/include -I../bond/include \
 *       -I../.. -I../../core/core -I../../src -I../../../runner \
 *       test_pipeline_integration.c -o test_pipeline_integration.exe
 *
 * Run:
 *   ./test_pipeline_integration.exe
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Pipeline includes ─────────────────────────────────────────── */
#ifndef NOGDI
#define NOGDI              /* suppress GDI Chord conflict with shell_container.h */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "pipeline_glue.h"       /* full pipeline: PGContext, stages, etc */
#include "dramtile_store.h"     /* DRamTile mmap helper                */
#include "gear_lock.h"          /* Gear Lock pull sequence             */

/* ── Hilbert curve for z-order arrangement ────────────────────────
 * 2D Hilbert (Morton) on 4×4 grid (16 positions).
 * Each chunk maps to a position based on bond fingerprint bits.
 */
static const uint8_t hilbert_4x4[16][2] = {
    {0,0},{0,1},{1,1},{1,0},       /* z=0 face */
    {0,2},{0,3},{1,3},{1,2},       /* z=1 face */
    {2,2},{2,3},{3,3},{3,2},       /* z=2 face */
    {2,1},{2,0},{3,0},{3,1}        /* z=3 face */
};

/* Get Hilbert z-index from bond piece's bond_key */
static inline uint32_t chunk_hilbert_z(uint64_t bond_key)
{
    uint8_t hx = (uint8_t)(bond_key >> 0) & 3;
    uint8_t hy = (uint8_t)(bond_key >> 2) & 3;
    /* Scan hilbert table for matching (x,y) */
    for (int i = 0; i < 16; i++)
        if (hilbert_4x4[i][0] == hx && hilbert_4x4[i][1] == hy)
            return (uint32_t)i;
    return bond_key & 0xF;
}

/* ── Arranged chunk header (sequence metadata) ────────────────────
 * Attached before each 64B chunk payload in the DRamTile buffer.
 * Total per entry: 16B header + 64B payload = 80B
 */
typedef struct {
    uint32_t chunk_idx;      /* original index 0..n_chunks-1    */
    uint8_t  z_order;        /* Hilbert z-index 0..15           */
    uint8_t  shell_flag;     /* SHELL_FLAG_FLAT/SPARSE/DENSE   */
    uint8_t  tile_id;        /* tile id from pipeline           */
    uint8_t  pad;            /* 4-byte alignment padding        */
    uint64_t bond_key;       /* bond piece key (geo addr)      */
} ArrangeHeader;

#define ARRANGE_ENTRY_SZ  (sizeof(ArrangeHeader) + 64)  /* 16 + 64 = 80 */

/* ══════════════════════════════════════════════════════════════════
   ARRANGE STAGE
   ══════════════════════════════════════════════════════════════════ */

/*
 * pg_arrange(): sort chunks by Hilbert z-order and build arranged buffer.
 *
 * Input:  PGContext with chunks after PIXELATING stage.
 * Output: arranged buffer — sequence of [ArrangeHeader + 64B payload]
 *         sorted by Hilbert z-order.
 *
 * Returns total arranged bytes, or 0 on error.
 */
static uint32_t pg_arrange(PGContext *pg,
                            ArrangeHeader *headers_out,
                            uint8_t       *payload_out)
{
    if (!pg || !pg->chunks || pg->n_chunks == 0) return 0;

    /* Build index of (hibert_z, chunk_idx) pairs */
    typedef struct {
        uint32_t z_order;
        uint32_t idx;
    } ZIdx;
    ZIdx *zidx = (ZIdx *)malloc(pg->n_chunks * sizeof(ZIdx));
    if (!zidx) { printf("[arrange] alloc FAIL\n"); return 0; }

    for (uint32_t i = 0; i < pg->n_chunks; i++) {
        zidx[i].z_order = chunk_hilbert_z(pg->chunks[i].piece.bond_L);
        zidx[i].idx     = i;
    }

    /* Sort by z-order (simple insertion sort — small N) */
    for (uint32_t i = 1; i < pg->n_chunks; i++) {
        ZIdx tmp = zidx[i];
        int j = (int)i - 1;
        while (j >= 0 && zidx[j].z_order > tmp.z_order) {
            zidx[j + 1] = zidx[j];
            j--;
        }
        zidx[j + 1] = tmp;
    }

    /* Build arranged output buffer */
    uint32_t total = 0;
    for (uint32_t i = 0; i < pg->n_chunks; i++) {
        uint32_t ci = zidx[i].idx;
        PGChunk *ch = &pg->chunks[ci];

        ArrangeHeader h;
        h.chunk_idx  = ch->chunk_idx;
        h.z_order    = (uint8_t)zidx[i].z_order;
        h.shell_flag = ch->shell_flag;
        h.tile_id    = ch->tile_id;
        h.pad        = 0;
        h.bond_key   = ch->piece.bond_L;

        memcpy(&headers_out[i], &h, sizeof(ArrangeHeader));
        memcpy(payload_out + i * 64, ch->data, 64);
        total += ARRANGE_ENTRY_SZ;
    }

    free(zidx);
    return total;
}

/* ══════════════════════════════════════════════════════════════════
   DRAMTILE STORE (simplified — mmap-like buffer for local test)
   ══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  *base;
    size_t    size;
    int       locked;
} DTileBuf;

static inline int dtile_init(DTileBuf *db, size_t sz)
{
    memset(db, 0, sizeof(*db));
    db->base = (uint8_t *)malloc(sz);
    if (!db->base) return -1;
    memset(db->base, 0, sz);
    db->size = sz;
    db->locked = 0;
    return 0;
}

static inline int dtile_lock(DTileBuf *db)
{
    if (db->locked) return 0;
    dt_enable_lock_privilege();
    int ret = dt_lock_pages(db->base, db->size);
    db->locked = (ret == 0);
    return ret;
}

static inline void dtile_free(DTileBuf *db)
{
    if (db->locked)
        dt_unlock_pages(db->base, db->size);
    free(db->base);
    db->base = NULL;
    db->size = 0;
}

/* ══════════════════════════════════════════════════════════════════
   GEAR LOCK PULL SCHEDULE
   ══════════════════════════════════════════════════════════════════ */

/* Simple gear lock: sequential read barrier per chunk */
typedef struct {
    uint32_t n_chunks;       /* total chunks to pull              */
    uint32_t n_pulled;       /* how many pulled so far            */
    uint64_t *pipe_map;      /* per-chunk pipe addr mapping       */
    int       locked;        /* mutex lock state                  */
} GearPullSchedule;

static inline int gps_init(GearPullSchedule *gps, uint32_t n)
{
    memset(gps, 0, sizeof(*gps));
    gps->n_chunks = n;
    gps->pipe_map = (uint64_t *)calloc(n, sizeof(uint64_t));
    if (!gps->pipe_map) return -1;
    /* Map each chunk to a unique pipe address based on its index */
    for (uint32_t i = 0; i < n; i++)
        gps->pipe_map[i] = (uint64_t)(i + 1) * 0x100000ULL;
    return 0;
}

static inline void gps_free(GearPullSchedule *gps)
{
    free(gps->pipe_map);
    memset(gps, 0, sizeof(*gps));
}

static inline int gps_pull_next(GearPullSchedule *gps,
                                 uint32_t *out_pipe_idx,
                                 uint64_t *out_pipe_addr)
{
    if (gps->n_pulled >= gps->n_chunks) return -1;  /* done */
    uint32_t idx = gps->n_pulled;
    *out_pipe_idx  = idx;
    *out_pipe_addr = gps->pipe_map[idx];
    gps->n_pulled++;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
   FULL INTEGRATION TEST
   ══════════════════════════════════════════════════════════════════ */

static int test_pipeline_split_pull(void)
{
    uint32_t pass = 0, fail = 0;

    /* ── 1. Create input data ────────────────────────────────── */
    uint8_t input[256];
    for (int i = 0; i < 256; i++)
        input[i] = (uint8_t)(i & 0x3F) | 0x80;  /* structured data */

    /* ── 2. Run base pipeline (chunking through pixelating) ──── */
    PGContext pg;
    memset(&pg, 0, sizeof(pg));
    int ret = pg_init(&pg, input, sizeof(input), 0xF00D, PG_FLAG_NONE);
    if (ret != 0) { printf("[integ] init FAIL\n"); return -1; }

    if (pg_stage_chunking(&pg)  == 0) { printf("[integ] chunk FAIL\n");  goto err; }
    if (pg_stage_bonding(&pg)   == 0) { printf("[integ] bond FAIL\n");   goto err; }
    if (pg_stage_shelling(&pg)  == 0) { printf("[integ] shell FAIL\n");  goto err; }
    if (pg_stage_pixelating(&pg)== 0) { printf("[integ] pixel FAIL\n");  goto err; }

    uint32_t n_chunks = pg.n_chunks;
    printf("[integ] %u chunks created\n", n_chunks);

    /* ── 3. ARRANGE: Hilbert z-order sort + sequence headers ── */
    ArrangeHeader *headers = (ArrangeHeader *)calloc(n_chunks, sizeof(ArrangeHeader));
    uint8_t       *payload = (uint8_t      *)malloc(n_chunks * 64);
    if (!headers || !payload) { printf("[integ] arr alloc FAIL\n"); goto err; }

    uint32_t arranged_sz = pg_arrange(&pg, headers, payload);
    if (arranged_sz == 0) { printf("[integ] arrange FAIL\n"); goto err; }
    printf("[integ] arrange: %u bytes (%.0f B/chunk)\n",
           arranged_sz, (double)arranged_sz / n_chunks);

    /* Verify z-order is ascending */
    uint32_t z_ok = 1;
    for (uint32_t i = 1; i < n_chunks; i++)
        if (headers[i-1].z_order > headers[i].z_order) z_ok = 0;
    printf("[integ] z-order %s\n", z_ok ? "PASS" : "FAIL");
    if (z_ok) pass++; else fail++;

    /* ── 4. SPLIT: write payload to DRamTile buffer ──────────── */
    DTileBuf dt;
    if (dtile_init(&dt, n_chunks * 64) != 0) {
        printf("[integ] dtile init FAIL\n"); goto err;
    }
    memcpy(dt.base, payload, n_chunks * 64);

    /* Lock pages (simulate GPU-ready pinned memory) */
    if (dtile_lock(&dt) != 0) {
        printf("[integ] dtile lock FAIL (non-fatal, continuing)\n");
    }
    printf("[integ] split: %u bytes to DRamTile\n", (uint32_t)(n_chunks * 64));

    /* ── 5. Gear Lock schedule + PULL ────────────────────────── */
    GearPullSchedule gps;
    if (gps_init(&gps, n_chunks) != 0) {
        printf("[integ] gps init FAIL\n"); goto err;
    }

    uint32_t pull_ok  = 1;
    uint32_t n_pulled = 0;
    uint32_t pipe_idx;
    uint64_t pipe_addr;

    while (gps_pull_next(&gps, &pipe_idx, &pipe_addr) == 0) {
        /* Read chunk from DRamTile at pull order position */
        uint8_t pulled[64];
        memcpy(pulled, dt.base + pipe_idx * 64, 64);

        /* Find which arranged chunk this corresponds to */
        uint32_t ci = pipe_idx;  /* sequential pull for this test */
        PGChunk *orig = &pg.chunks[ci];

        if (memcmp(pulled, orig->data, 64) != 0) {
            printf("[integ] pull MISMATCH at pipe %u\n", pipe_idx);
            pull_ok = 0;
        }
        n_pulled++;
    }

    if (n_pulled == n_chunks) {
        printf("[integ] pull: %u/%u chunks PASS\n", n_pulled, n_chunks);
        if (pull_ok) { printf("[integ] pull PASS\n"); pass++; }
        else         { printf("[integ] pull FAIL\n"); fail++; }
    } else {
        printf("[integ] pull: %u/%u FAIL (incomplete)\n", n_pulled, n_chunks);
        fail++;
    }

    /* ── 6. Verify sequence headers are recoverable ──────────── */
    uint32_t hdr_ok = 1;
    for (uint32_t i = 0; i < n_chunks; i++) {
        ArrangeHeader *h = &headers[i];
        PGChunk *ch = NULL;
        /* Find original chunk by chunk_idx */
        for (uint32_t j = 0; j < n_chunks; j++) {
            if (pg.chunks[j].chunk_idx == h->chunk_idx) {
                ch = &pg.chunks[j];
                break;
            }
        }
        if (!ch || h->shell_flag != ch->shell_flag ||
            h->tile_id != ch->tile_id ||
            h->bond_key != ch->piece.bond_L) {
            printf("[integ] hdr MISMATCH at z-order %u\n", h->z_order);
            hdr_ok = 0;
        }
    }
    printf("[integ] headers %s\n", hdr_ok ? "PASS" : "FAIL");
    if (hdr_ok) pass++; else fail++;

    /* ── Cleanup ─────────────────────────────────────────────── */
    free(headers);
    free(payload);
    dtile_free(&dt);
    gps_free(&gps);
    pg_free(&pg);

    printf("[integ] %u PASS / %u FAIL\n", pass, fail);
    return (fail == 0) ? 0 : -1;

err:
    pg_free(&pg);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("═══ Pipeline Split+Arrange+Pull Integration Test ═══\n\n");
    int ret = test_pipeline_split_pull();
    printf("\n═══════════════════════════════════════════════════\n");
    if (ret == 0) {
        printf("ALL PASS\n");
        return 0;
    } else {
        printf("SOME FAILS\n");
        return 1;
    }
}

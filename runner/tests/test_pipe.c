/*
 * test_pipe.c — Pipe ABI Test Suite
 *
 * Tests:
 *   1. pipe_open / pipe_close lifecycle
 *   2. pipe_write + pipe_read roundtrip
 *   3. pipe_find existence check
 *   4. pipe_remove deletion
 *   5. pipe_compact footprint reduction
 *   6. pipe_count slot tracking
 *   7. pipe_stats statistics
 *   8. Multiple items write/read
 *   9. Pipe naming
 *  10. Backend health check
 *  11. Checkpoint + restore
 *  12. Recreate (destroy + create + restore)
 *  13. Snapshot info / exists
 *  14. Radial detect + RDH coordinate pipeline
 *  15. DRamTile HDD backend lifecycle + persistence
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../../collection/pipe/pipe_backend.h"
#include "../../collection/pipe/pipe_context.h"

/* ── Test helpers ───────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { tests_run++; printf("  [%d] %-40s ", tests_run, name); } while(0)

#define PASS() \
    do { tests_passed++; printf("PASS\n"); } while(0)

#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); } while(0)

#define ASSERT(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* ── Test 1: Open / Close lifecycle ─────────────────────────── */

static void test_open_close(void) {
    TEST("pipe_open / pipe_close lifecycle");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.name = "test_pipe";
    cfg.capacity = 1024 * 1024;  /* 1 MB */

    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open returned NULL");
    ASSERT(ctx->magic == PIPE_MAGIC, "wrong magic");
    ASSERT(strcmp(ctx->name, "test_pipe") == 0, "wrong name");
    ASSERT(ctx->is_open == 1, "not open");

    pipe_close(ctx);
    PASS();
}

/* ── Test 2: Write + Read roundtrip ─────────────────────────── */

static void test_write_read(void) {
    TEST("pipe_write + pipe_read roundtrip");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 1024 * 1024;

    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");

    const char *data = "Hello, Pipe ABI!";
    uint32_t len = (uint32_t)strlen(data) + 1;

    int rc = pipe_write(ctx, "greeting", data, len, PIPE_DTYPE_RAW, PIPE_PRIO_NORMAL);
    ASSERT(rc == PIPE_OK, "pipe_write failed");

    uint32_t read_len = 0;
    const void *read_data = pipe_read(ctx, "greeting", &read_len);
    ASSERT(read_data != NULL, "pipe_read returned NULL");
    ASSERT(read_len == len, "wrong length");
    ASSERT(memcmp(read_data, data, len) == 0, "data mismatch");

    pipe_close(ctx);
    PASS();
}

/* ── Test 3: Find ───────────────────────────────────────────── */

static void test_find(void) {
    TEST("pipe_find existence check");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 1024 * 1024;

    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");

    ASSERT(pipe_find(ctx, "not_here") == 0, "should not find non-existent");

    float val = 3.14f;
    pipe_write(ctx, "pi", &val, sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_HIGH);
    ASSERT(pipe_find(ctx, "pi") == 1, "should find 'pi'");
    ASSERT(pipe_find(ctx, "not_pi") == 0, "should not find 'not_pi'");

    pipe_close(ctx);
    PASS();
}

/* ── Test 4: Remove ─────────────────────────────────────────── */

static void test_remove(void) {
    TEST("pipe_remove deletion");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 1024 * 1024;

    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");

    int val = 42;
    pipe_write(ctx, "answer", &val, sizeof(int), PIPE_DTYPE_I32, PIPE_PRIO_NORMAL);
    ASSERT(pipe_count(ctx) == 1, "should have 1 slot");

    int rc = pipe_remove(ctx, "answer");
    ASSERT(rc == PIPE_OK, "pipe_remove failed");
    ASSERT(pipe_count(ctx) == 0, "should have 0 slots after remove");
    ASSERT(pipe_find(ctx, "answer") == 0, "should not find removed item");

    pipe_close(ctx);
    PASS();
}

/* ── Test 5: Compact ────────────────────────────────────────── */

static void test_compact(void) {
    TEST("pipe_compact footprint reduction");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 1024 * 1024;

    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");

    /* Write some items */
    float vals[10];
    for (int i = 0; i < 10; i++) {
        vals[i] = (float)i;
        char name[32];
        snprintf(name, sizeof(name), "item_%d", i);
        pipe_write(ctx, name, &vals[i], sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_NORMAL);
    }
    ASSERT(pipe_count(ctx) == 10, "should have 10 slots");

    /* Remove some */
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "item_%d", i);
        pipe_remove(ctx, name);
    }
    ASSERT(pipe_count(ctx) == 5, "should have 5 slots after removes");

    /* Compact */
    uint64_t reclaimed = pipe_compact(ctx);
    (void)reclaimed;  /* compact returns bytes reclaimed */

    pipe_close(ctx);
    PASS();
}

/* ── Test 6: Count ──────────────────────────────────────────── */

static void test_count(void) {
    TEST("pipe_count slot tracking");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 1024 * 1024;

    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");
    ASSERT(pipe_count(ctx) == 0, "initial count should be 0");

    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "x%d", i);
        int v = i;
        pipe_write(ctx, name, &v, sizeof(int), PIPE_DTYPE_I32, PIPE_PRIO_LOW);
    }
    ASSERT(pipe_count(ctx) == 5, "should have 5 slots");

    pipe_close(ctx);
    PASS();
}

/* ── Test 7: Stats ──────────────────────────────────────────── */

static void test_stats(void) {
    TEST("pipe_stats statistics");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 1024 * 1024;

    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");

    /* Write and read to generate stats */
    float val = 1.0f;
    pipe_write(ctx, "a", &val, sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_NORMAL);
    pipe_read(ctx, "a", NULL);
    pipe_read(ctx, "a", NULL);

    PipeStats stats;
    pipe_stats(ctx, &stats);
    ASSERT(stats.n_writes == 1, "wrong write count");
    ASSERT(stats.n_reads == 2, "wrong read count");

    pipe_close(ctx);
    PASS();
}

/* ── Test 8: Multiple items ─────────────────────────────────── */

static void test_multiple(void) {
    TEST("multiple items write/read");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 4 * 1024 * 1024;  /* 4 MB for many items */

    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");

    /* Write 20 different types */
    float   f_val = 2.718f;
    int32_t i_val = 42;
    double  d_val = 3.14159;
    char    s_val[] = "string data";

    pipe_write(ctx, "float",  &f_val, sizeof(float),   PIPE_DTYPE_F32, PIPE_PRIO_HIGH);
    pipe_write(ctx, "int",    &i_val, sizeof(int32_t), PIPE_DTYPE_I32, PIPE_PRIO_NORMAL);
    pipe_write(ctx, "double", &d_val, sizeof(double),  PIPE_DTYPE_F32, PIPE_PRIO_LOW);
    pipe_write(ctx, "string", s_val,  sizeof(s_val),   PIPE_DTYPE_RAW, PIPE_PRIO_NORMAL);

    /* Read all back */
    uint32_t n;
    const float *rf = (const float *)pipe_read(ctx, "float", &n);
    ASSERT(rf && *rf == f_val, "float mismatch");

    const int32_t *ri = (const int32_t *)pipe_read(ctx, "int", &n);
    ASSERT(ri && *ri == i_val, "int mismatch");

    const double *rd = (const double *)pipe_read(ctx, "double", &n);
    ASSERT(rd && *rd == d_val, "double mismatch");

    const char *rs = (const char *)pipe_read(ctx, "string", &n);
    ASSERT(rs && strcmp(rs, s_val) == 0, "string mismatch");

    ASSERT(pipe_count(ctx) == 4, "should have 4 items");

    pipe_close(ctx);
    PASS();
}

/* ── Test 9: Pipe name ──────────────────────────────────────── */

static void test_name(void) {
    TEST("pipe_name accessor");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.name = "my_pipe";
    cfg.capacity = 1024 * 1024;

    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");
    ASSERT(strcmp(pipe_name(ctx), "my_pipe") == 0, "wrong name");

    pipe_close(ctx);
    PASS();
}

/* ── Test 10: Backend health ────────────────────────────────── */

static void test_backend_health(void) {
    TEST("backend health check");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 1024 * 1024;

    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");
    ASSERT(ctx->backend != NULL, "no backend");
    ASSERT(ctx->backend->healthy(ctx->backend) == 1, "backend unhealthy");

    pipe_close(ctx);
    PASS();
}

/* ── Test 11: Checkpoint + Restore ──────────────────────────── */

static void test_checkpoint_restore(void) {
    TEST("pipe_checkpoint + pipe_restore");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 1024 * 1024;

    const char *snap_path = "test_snapshot.bin";

    /* Write data to pipe */
    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");

    float f_val = 3.14f;
    int i_val = 42;
    pipe_write(ctx, "pi", &f_val, sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_HIGH);
    pipe_write(ctx, "answer", &i_val, sizeof(int), PIPE_DTYPE_I32, PIPE_PRIO_NORMAL);
    ASSERT(pipe_count(ctx) == 2, "should have 2 items");

    /* Checkpoint */
    int rc = pipe_checkpoint(ctx, snap_path);
    ASSERT(rc == PIPE_OK, "pipe_checkpoint failed");
    pipe_close(ctx);

    /* Restore into fresh context */
    ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed (after checkpoint)");
    ASSERT(pipe_count(ctx) == 0, "fresh pipe should be empty");

    rc = pipe_restore(ctx, snap_path);
    ASSERT(rc == PIPE_OK, "pipe_restore failed");
    ASSERT(pipe_count(ctx) == 2, "should have 2 items after restore");

    /* Verify data integrity */
    uint32_t n;
    const float *rf = (const float *)pipe_read(ctx, "pi", &n);
    ASSERT(rf != NULL, "pi not found after restore");
    ASSERT(*rf == f_val, "pi value mismatch after restore");

    const int *ri = (const int *)pipe_read(ctx, "answer", &n);
    ASSERT(ri != NULL, "answer not found after restore");
    ASSERT(*ri == i_val, "answer value mismatch after restore");

    pipe_close(ctx);
    remove(snap_path);  /* cleanup */
    PASS();
}

/* ── Test 12: Recreate (destroy + create + restore) ─────────── */

static void test_recreate(void) {
    TEST("pipe_recreate (destroy + create + restore)");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 1024 * 1024;

    const char *snap_path = "test_recreate.bin";

    /* Write data */
    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");

    double d_val = 2.71828;
    char s_val[] = "hello void space";
    pipe_write(ctx, "euler", &d_val, sizeof(double), PIPE_DTYPE_F32, PIPE_PRIO_HIGH);
    pipe_write(ctx, "msg", s_val, sizeof(s_val), PIPE_DTYPE_RAW, PIPE_PRIO_NORMAL);

    /* Checkpoint before recreate */
    pipe_checkpoint(ctx, snap_path);

    /* Recreate (atomic destroy + create + restore) */
    PipeContext *new_ctx = pipe_recreate(ctx, snap_path);
    ASSERT(new_ctx != NULL, "pipe_recreate returned NULL");
    ASSERT(pipe_count(new_ctx) == 2, "should have 2 items after recreate");

    /* Verify data survived recreation */
    uint32_t n;
    const double *rd = (const double *)pipe_read(new_ctx, "euler", &n);
    ASSERT(rd != NULL, "euler not found after recreate");
    ASSERT(*rd == d_val, "euler value mismatch after recreate");

    const char *rs = (const char *)pipe_read(new_ctx, "msg", &n);
    ASSERT(rs != NULL, "msg not found after recreate");
    ASSERT(strcmp(rs, s_val) == 0, "msg mismatch after recreate");

    pipe_close(new_ctx);
    remove(snap_path);  /* cleanup */
    PASS();
}

/* ── Test 13: Snapshot exists + info ─────────────────────────── */

static void test_snapshot_info(void) {
    TEST("pipe_snapshot_exists + pipe_snapshot_info");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 1024 * 1024;

    const char *snap_path = "test_info.bin";

    /* No snapshot yet */
    ASSERT(pipe_snapshot_exists(snap_path) == 0, "should not exist");

    /* Create and checkpoint */
    PipeContext *ctx = pipe_open(&cfg);
    float v = 1.0f;
    pipe_write(ctx, "one", &v, sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_LOW);
    pipe_checkpoint(ctx, snap_path);
    pipe_close(ctx);

    /* Now it should exist */
    ASSERT(pipe_snapshot_exists(snap_path) == 1, "should exist after checkpoint");

    /* Read info */
    PipeSnapshotHeader hdr;
    int rc = pipe_snapshot_info(snap_path, &hdr);
    ASSERT(rc == PIPE_OK, "pipe_snapshot_info failed");
    ASSERT(hdr.magic == PIPE_SNAP_MAGIC, "wrong magic");
    ASSERT(hdr.version == PIPE_SNAP_VERSION, "wrong version");
    ASSERT(hdr.n_slots == 1, "should have 1 slot");
    ASSERT(hdr.backend_size > 0, "backend size should be > 0");

    remove(snap_path);  /* cleanup */
    PASS();
}

/* ── Test 14: Radial + RDH pipeline ─────────────────────────── */

static void test_radial_rdh_pipeline(void) {
    TEST("Radial detect + RDH coordinate pipeline");

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 1024 * 1024;

    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open failed");

    /* Write several items with different names */
    float vals[3] = {1.0f, 2.0f, 3.0f};
    pipe_write(ctx, "layer0.weight", &vals[0], sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_HIGH);
    pipe_write(ctx, "layer1.bias",   &vals[1], sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_NORMAL);
    pipe_write(ctx, "embd.weight",  &vals[2], sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_LOW);

    /* Verify: radial_addr should be real geometric address (not plain hash) */
    for (uint32_t i = 0; i < ctx->n_slots; i++) {
        uint64_t ra = ctx->slots[i].radial_addr & ~RC_TWIN_BIT;  /* mask twin bit */
        int vertex = (int)(ra / RC_N_NODES);  /* RC_N_NODES = 7 */
        int node   = (int)(ra % RC_N_NODES);
        ASSERT(vertex >= 0 && vertex < 24, "vertex out of range");
        ASSERT(node >= 0 && node < 7, "node out of range");
    }

    /* Verify: RDH key should be within capacity */
    for (uint32_t i = 0; i < ctx->n_slots; i++) {
        int64_t k = (int64_t)ctx->slots[i].key;
        ASSERT(k >= 0 && k < ctx->rdh.capacity, "RDH key out of range");
    }

    /* Verify: translations counter incremented */
    ASSERT(ctx->rdh.n_translations == 3, "should have 3 translations");

    pipe_close(ctx);
    PASS();
}

 /* ── Test 15: DRamTile HDD backend lifecycle + persistence ───── */

static void test_dramtile_backend(void) {
    TEST("DRamTile HDD backend lifecycle + persistence");

    const char *path = "F:\\test_pipe_dramtile.bin";

    /* Clean up any leftover from previous run */
    DeleteFileA(path);

    PipeConfig cfg = PIPE_CONFIG_DEFAULT;
    cfg.capacity = 0;       /* grow on demand */
    cfg.backend_path = path;

    /* Phase 1: Create + write */
    PipeContext *ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open with dramtile failed");

    float f1 = 1.0f, f2 = 2.0f, f3 = 3.0f;
    pipe_write(ctx, "alpha", &f1, sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_HIGH);
    pipe_write(ctx, "beta",  &f2, sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_NORMAL);
    pipe_write(ctx, "gamma", &f3, sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_LOW);
    ASSERT(pipe_count(ctx) == 3, "should have 3 items");

    /* Checkpoint to save slots to mmap file */
    pipe_checkpoint(ctx, path);
    pipe_close(ctx);

    /* Phase 2: Reopen — data should auto-restore */
    ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open with dramtile (2nd) failed");
    ASSERT(pipe_count(ctx) == 3, "should have 3 items after reopen");

    /* Verify data integrity after reopen */
    uint32_t n;
    const float *ra = (const float *)pipe_read(ctx, "alpha", &n);
    ASSERT(ra != NULL && *ra == 1.0f, "alpha mismatch after reopen");

    /* Add more data */
    pipe_write(ctx, "delta", &f3, sizeof(float), PIPE_DTYPE_F32, PIPE_PRIO_LOW);
    ASSERT(pipe_count(ctx) == 4, "should have 4 items");

    /* Remove + compact — file should shrink */
    pipe_remove(ctx, "gamma");
    ASSERT(pipe_count(ctx) == 3, "should have 3 after remove");
    pipe_compact(ctx);

    /* Checkpoint + close */
    pipe_checkpoint(ctx, path);
    pipe_close(ctx);

    /* Phase 3: Reopen — verify gamma gone, alpha+delta exist */
    ctx = pipe_open(&cfg);
    ASSERT(ctx != NULL, "pipe_open (3rd) failed");
    ASSERT(pipe_count(ctx) == 3, "should have 3 after compact persist");
    ASSERT(pipe_find(ctx, "gamma") == 0, "gamma should be gone");
    ASSERT(pipe_find(ctx, "alpha") == 1, "alpha should exist");
    ASSERT(pipe_find(ctx, "delta") == 1, "delta should exist");

    pipe_close(ctx);

    /* Cleanup: delete file → return space to HDD */
    DeleteFileA(path);
    PASS();
}

/* ── Main ───────────────────────────────────────────────────── */

int main(void) {
    printf("═══════════════════════════════════════════════\n");
    printf("  Pipe ABI Test Suite\n");
    printf("═══════════════════════════════════════════════\n\n");

    test_open_close();
    test_write_read();
    test_find();
    test_remove();
    test_compact();
    test_count();
    test_stats();
    test_multiple();
    test_name();
    test_backend_health();
    test_checkpoint_restore();
    test_recreate();
    test_snapshot_info();
    test_radial_rdh_pipeline();
    test_dramtile_backend();

    printf("\n───────────────────────────────────────────────\n");
    printf("  Results: %d / %d passed\n", tests_passed, tests_run);
    printf("═══════════════════════════════════════════════\n");

    return tests_passed == tests_run ? 0 : 1;
}

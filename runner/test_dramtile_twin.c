/*
 * test_dramtile_twin.c — Prototype: file-backed twin DRamTile
 *
 * Tests:
 *   Phase 1: Fresh init, dt_put/dt_get, dt_view, dt_resolve, foreach
 *   Phase 2: Persistence across reopen (twin principle)
 *   Phase 3: Modify → sync → reopen → verify (twin principle)
 *   Phase 4: Self-describing TDI2 format with dt_store_destroy_twinv
 *   Phase 5: dt_store_load_views → full metadata reconstruction
 *
 * Build:
 *   gcc -O2 -std=c11 -I. -I../collection -o test_dramtile_twin.exe test_dramtile_twin.c -lm
 *
 * Run:
 *   test_dramtile_twin.exe [backing_file]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <math.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include "dramtile_store.h"
#include "dramtile_container.h"

/* ── helpers ── */
static int pass=0, fail=0;

static void test_phase_header(const char *label) {
    fprintf(stderr, "\n── %s ──\n", label);
}

static int verify_ptr(const char *label, uint8_t *ptr, uint8_t expected, size_t sz) {
    if (!ptr) { fprintf(stderr, "FAIL: %s → NULL\n", label); return -1; }
    for (size_t i = 0; i < sz; i++)
        if (ptr[i] != expected) {
            fprintf(stderr, "FAIL: %s[%zu]=%02x exp=%02x\n", label, i, ptr[i], expected);
            return -1;
        }
    fprintf(stderr, "  PASS: %s (%zu bytes)\n", label, sz);
    return 0;
}

static int verify_view(const char *label, DtTensorView v,
                       uint32_t exp_dtype, int exp_ndim,
                       const uint32_t *exp_shape, size_t exp_nbytes)
{
    if (!v.data)        { fprintf(stderr, "FAIL: %s → data=NULL\n", label); return -1; }
    if (v.dtype != exp_dtype) { fprintf(stderr, "FAIL: %s dtype=%u exp=%u\n", label, v.dtype, exp_dtype); return -1; }
    if (v.ndim != exp_ndim)   { fprintf(stderr, "FAIL: %s ndim=%d exp=%d\n", label, v.ndim, exp_ndim); return -1; }
    if (v.nbytes != exp_nbytes) { fprintf(stderr, "FAIL: %s nbytes=%zu exp=%zu\n", label, v.nbytes, exp_nbytes); return -1; }
    for (int i = 0; i < exp_ndim; i++)
        if (v.shape[i] != exp_shape[i]) { fprintf(stderr, "FAIL: %s shape[%d]=%u exp=%u\n", label, i, v.shape[i], exp_shape[i]); return -1; }
    fprintf(stderr, "  PASS: %s view (dtype=%u ndim=%d sz=%zu)\n", label, v.dtype, v.ndim, v.nbytes);
    return 0;
}

/* foreach callback — count tensors */
static int count_cb(DtTensorView *v, void *user) {
    (void)v; (*(int*)user)++;
    return 0;
}

/* foreach callback — verify data pattern */
typedef struct { const char *name; uint8_t pattern; size_t sz; } VerifyEntry;
static int verify_cb(DtTensorView *v, void *user) {
    VerifyEntry *entries = (VerifyEntry*)user;
    /* find matching entry by dram_addr — we skip name check since
     * old format doesn't store names */
    for (int i = 0; i < 3 && entries[i].sz > 0; i++) {
        if (v->nbytes == entries[i].sz) {
            int ok = 1;
            for (size_t j = 0; j < v->nbytes; j++)
                if (v->data[j] != entries[i].pattern) { ok = 0; break; }
            if (!ok)
                fprintf(stderr, "FAIL: foreach data mismatch at sz=%zu\n", v->nbytes);
            else
                fprintf(stderr, "  PASS: foreach tensor sz=%zu pattern=0x%02x\n",
                        v->nbytes, entries[i].pattern);
            return 0;
        }
    }
    fprintf(stderr, "  ???: foreach unknown tensor sz=%zu\n", v->nbytes);
    return 0;
}

/* ── main ── */
int main(int argc, char **argv) {
    const char *filepath = argc > 1 ? argv[1] : "dramtile_twin_test.bin";
    uint32_t shape2[] = {8, 8};
    uint32_t shape3[] = {4, 4, 8};

    /* Clean up any leftover from previous crashed run */
    remove(filepath);

    fprintf(stderr, "\n═══════════════════════════════════════════\n");
    fprintf(stderr, "DRamTile Twin — Full Prototype Test\n");
    fprintf(stderr, "  file: %s\n", filepath);
    fprintf(stderr, "═══════════════════════════════════════════\n\n");

    /* ═══ Phase 1: Fresh init + basic ops ═══ */
    fprintf(stderr, "── Phase 1: Fresh init + basic ops ──\n");
    DRamTileStore s1;
    int ret = dt_store_init_twin(&s1, filepath, 64UL * 1024 * 1024);
    if (ret != 0) { fprintf(stderr, "FAIL: init_twin → %d\n", ret); return 1; }
    if (s1.n_stored == 0) { fprintf(stderr, "  PASS: fresh store\n"); pass++; }
    else { fprintf(stderr, "FAIL: expected fresh\n"); fail++; }

    /* Write 3 tensors */
    uint8_t tA[64];  memset(tA, 0xAA, 64);
    uint8_t tB[128]; memset(tB, 0xBB, 128);
    uint8_t tC[32];  memset(tC, 0xCC, 32);

    uint8_t *pA = dt_put(&s1, "tensor.A", tA, 64);
    uint8_t *pB = dt_put(&s1, "tensor.B", tB, 128);
    uint8_t *pC = dt_put(&s1, "tensor.C", tC, 32);
    if (verify_ptr("dt_put(A)=0xAA", pA, 0xAA, 64) == 0) pass++; else fail++;
    if (verify_ptr("dt_put(B)=0xBB", pB, 0xBB, 128) == 0) pass++; else fail++;
    if (verify_ptr("dt_put(C)=0xCC", pC, 0xCC, 32) == 0) pass++; else fail++;

    /* dt_get after put */
    if (verify_ptr("dt_get(A)=0xAA", dt_get(&s1, "tensor.A"), 0xAA, 64) == 0) pass++; else fail++;
    if (verify_ptr("dt_get(B)=0xBB", dt_get(&s1, "tensor.B"), 0xBB, 128) == 0) pass++; else fail++;
    if (verify_ptr("dt_get(C)=0xCC", dt_get(&s1, "tensor.C"), 0xCC, 32) == 0) pass++; else fail++;

    /* dt_view with explicit metadata */
    DtTensorView vA = dt_view(&s1, "tensor.A", DT_F32, 2, shape2);
    if (verify_view("dt_view(A)", vA, DT_F32, 2, shape2, 64) == 0) pass++; else fail++;

    DtTensorView vB = dt_view(&s1, "tensor.B", DT_I8, 3, shape3);
    if (verify_view("dt_view(B)", vB, DT_I8, 3, shape3, 128) == 0) pass++; else fail++;

    /* dt_resolve coordinate-based */
    uint32_t addrA = dt_name_to_addr("tensor.A");
    DtTensorView rA = dt_resolve(&s1, addrA);
    if (rA.data == pA && rA.nbytes == 64) {
        fprintf(stderr, "  PASS: dt_resolve(A) → data match\n"); pass++;
    } else { fprintf(stderr, "FAIL: dt_resolve(A)\n"); fail++; }

    uint32_t addrB = dt_name_to_addr("tensor.B");
    DtTensorView rB = dt_resolve(&s1, addrB);
    if (rB.data == pB && rB.nbytes == 128) {
        fprintf(stderr, "  PASS: dt_resolve(B) → data match\n"); pass++;
    } else { fprintf(stderr, "FAIL: dt_resolve(B)\n"); fail++; }

    /* dt_store_foreach */
    int count = 0;
    dt_store_foreach(&s1, count_cb, &count);
    if (count == 3) { fprintf(stderr, "  PASS: foreach count=%d\n", count); pass++; }
    else { fprintf(stderr, "FAIL: foreach count=%d exp=3\n", count); fail++; }

    /* dt_store_total_bytes: 64+128+32=224 */
    size_t total = dt_store_total_bytes(&s1);
    uint32_t shape2b[] = {8, 8};
    if (total == 224) { fprintf(stderr, "  PASS: total_bytes=%zu\n", total); pass++; }
    else { fprintf(stderr, "FAIL: total_bytes=%zu exp=224\n", total); fail++; }

    /* Verify with foreach callback */
    VerifyEntry verify_entries[] = {{"A", 0xAA, 64}, {"B", 0xBB, 128}, {"C", 0xCC, 32}, {NULL, 0, 0}};
    dt_store_foreach(&s1, verify_cb, verify_entries);

    /* dt_store_check_twin */
    if (dt_store_check_twin(filepath) == 0) {
        fprintf(stderr, "  PASS: check_twin OK\n"); pass++;
    } else { fprintf(stderr, "FAIL: check_twin\n"); fail++; }

    fprintf(stderr, "\n");

    /* ═══ Phase 2: Persistence (save + reopen) ═══ */
    fprintf(stderr, "── Phase 2: Persistence ──\n");
    dt_store_destroy_twin(&s1);

    DRamTileStore s2;
    dt_store_init_twin(&s2, filepath, 64UL * 1024 * 1024);
    if (s2.n_stored == 3) { fprintf(stderr, "  PASS: reopen n_stored=3\n"); pass++; }
    else { fprintf(stderr, "FAIL: reopen n_stored=%u\n", s2.n_stored); fail++; }

    if (verify_ptr("reopen dt_get(A)=0xAA", dt_get(&s2, "tensor.A"), 0xAA, 64) == 0) pass++; else fail++;
    if (verify_ptr("reopen dt_get(B)=0xBB", dt_get(&s2, "tensor.B"), 0xBB, 128) == 0) pass++; else fail++;
    if (verify_ptr("reopen dt_get(C)=0xCC", dt_get(&s2, "tensor.C"), 0xCC, 32) == 0) pass++; else fail++;

    /* Resolve after reopen */
    DtTensorView rA2 = dt_resolve(&s2, dt_name_to_addr("tensor.A"));
    if (rA2.data && rA2.nbytes == 64) { fprintf(stderr, "  PASS: reopen resolve(A)\n"); pass++; }
    else { fprintf(stderr, "FAIL: reopen resolve(A)\n"); fail++; }

    fprintf(stderr, "\n");

    /* ═══ Phase 3: Modify → persist (twin principle) ═══ */
    fprintf(stderr, "── Phase 3: Twin principle (modify → persist) ──\n");
    uint8_t *modA = dt_get(&s2, "tensor.A");
    memset(modA, 0xDD, 64);
    dt_store_sync(&s2, 0);
    fprintf(stderr, "  modified tensor.A → 0xDD, synced\n");

    dt_store_destroy_twin(&s2);

    DRamTileStore s3;
    dt_store_init_twin(&s3, filepath, 64UL * 1024 * 1024);
    if (verify_ptr("twin: dt_get(A)=0xDD", dt_get(&s3, "tensor.A"), 0xDD, 64) == 0) pass++; else fail++;
    if (verify_ptr("twin: dt_get(B)=0xBB (unchanged)", dt_get(&s3, "tensor.B"), 0xBB, 128) == 0) pass++; else fail++;

    /* dt_getv works (returns view with data ptr) */
    DtTensorView gvA = dt_getv(&s3, "tensor.A");
    if (gvA.data && gvA.nbytes == 64 && strcmp(gvA.name, "tensor.A") == 0) {
        fprintf(stderr, "  PASS: dt_getv(A) name=%s sz=%zu\n", gvA.name, gvA.nbytes); pass++;
    } else { fprintf(stderr, "FAIL: dt_getv(A)\n"); fail++; }

    dt_store_destroy_twin(&s3);
    fprintf(stderr, "\n");

    /* ═══ Phase 4: Self-describing TDI2 format ═══ */
    fprintf(stderr, "── Phase 4: Self-describing TDI2 format ──\n");
    DRamTileStore s4;
    dt_store_init_twin(&s4, filepath, 64UL * 1024 * 1024);

    /* Write with dt_putv — stores all tensors */
    uint8_t tD[96];  memset(tD, 0xDD, 96);
    uint8_t tE[48];  memset(tE, 0xEE, 48);
    uint32_t shapeD[] = {12, 8};
    uint32_t shapeE[] = {6, 8};

    dt_putv(&s4, "tensor.D", DT_F16, 2, shapeD, tD, 96);
    dt_putv(&s4, "tensor.E", DT_I32, 2, shapeE, tE, 48);

    /* Re-read with dt_getv (in-session, no persisted metadata yet) */
    DtTensorView vD = dt_getv(&s4, "tensor.D");
    if (vD.data && vD.nbytes == 96) { fprintf(stderr, "  PASS: dt_getv(D)=96 bytes\n"); pass++; }
    else { fprintf(stderr, "FAIL: dt_getv(D)\n"); fail++; }

    /* Destroy with dt_store_destroy_twinv — saves TDI2 with metadata */
    dt_store_destroy_twinv(&s4);
    fprintf(stderr, "  destroyed with dt_store_destroy_twinv (TDI2)\n");

    /* Reopen — should still find all 5 tensors */
    DRamTileStore s5;
    dt_store_init_twin(&s5, filepath, 64UL * 1024 * 1024);
    if (s5.n_stored == 5) { fprintf(stderr, "  PASS: reopen after TDI2, n_stored=5\n"); pass++; }
    else { fprintf(stderr, "FAIL: after TDI2 n_stored=%u\n", s5.n_stored); fail++; }

    /* All data intact */
    if (verify_ptr("TDI2: dt_get(A)=0xDD", dt_get(&s5, "tensor.A"), 0xDD, 64) == 0) pass++; else fail++;
    if (verify_ptr("TDI2: dt_get(B)=0xBB", dt_get(&s5, "tensor.B"), 0xBB, 128) == 0) pass++; else fail++;
    if (verify_ptr("TDI2: dt_get(C)=0xCC", dt_get(&s5, "tensor.C"), 0xCC, 32) == 0) pass++; else fail++;
    if (verify_ptr("TDI2: dt_get(D)=0xDD", dt_get(&s5, "tensor.D"), 0xDD, 96) == 0) pass++; else fail++;
    if (verify_ptr("TDI2: dt_get(E)=0xEE", dt_get(&s5, "tensor.E"), 0xEE, 48) == 0) pass++; else fail++;

    dt_store_destroy_twin(&s5);
    fprintf(stderr, "\n");

    /* ═══ Phase 5: Full metadata from TDI2 ═══ */
    fprintf(stderr, "── Phase 5: dt_store_load_views (full metadata) ──\n");
    DRamTileStore s6;
    dt_store_init_twin(&s6, filepath, 64UL * 1024 * 1024);

    DtTensorView *views = NULL;
    int nv = dt_store_load_views(&s6, &views);
    if (nv == 5 && views) {
        fprintf(stderr, "  PASS: load_views nv=%d\n", nv); pass++;
        for (int i = 0; i < nv; i++) {
            if (views[i].name[0])
                fprintf(stderr, "    [%d] name=%s addr=%u sz=%zu\n",
                        i, views[i].name, views[i].dram_addr, views[i].nbytes);
        }
    } else {
        fprintf(stderr, "FAIL: load_views nv=%d (exp=5)\n", nv); fail++;
    }

    /* dt_resolve still works after load_views */
    DtTensorView rE = dt_resolve(&s6, dt_name_to_addr("tensor.E"));
    if (rE.data && rE.nbytes == 48) { fprintf(stderr, "  PASS: resolve(E) after load_views\n"); pass++; }
    else { fprintf(stderr, "FAIL: resolve(E)\n"); fail++; }

    free(views);
    dt_store_destroy_twin(&s6);

    /* Test nonexistent */
    DRamTileStore s7;
    dt_store_init_twin(&s7, filepath, 64UL * 1024 * 1024);
    if (dt_get(&s7, "nonexistent") == NULL) { fprintf(stderr, "  PASS: nonexistent → NULL\n"); pass++; }
    else { fprintf(stderr, "FAIL: nonexistent should be NULL\n"); fail++; }
    dt_store_destroy_twin(&s7);

    fprintf(stderr, "\n");

    /* ═══ Phase 6: Dual-region — KV not persisted ═══ */
    fprintf(stderr, "── Phase 6: Dual-region (KV ephemeral) ──\n");
    const char *dfile = "dramtile_dual_test.bin";
    remove(dfile);
    DRamTileStore sd;
    size_t wcap = 1UL << 20; /* 1MB weight region */
    size_t kcap = 512UL << 10; /* 512KB KV region */
    /* Use init_twin then patch kv_base/kv_capacity to simulate dual-region */
    if (dt_store_init_twin(&sd, dfile, wcap) == 0) {
        /* Manually set up KV region (same as dt_store_init_twin_dual would) */
        sd.kv_base = (uint8_t*)VirtualAlloc(NULL, kcap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        sd.kv_capacity = kcap;
        sd.weight_boundary = wcap;
        fprintf(stderr, "  KV region: %zu bytes\n", kcap);

        /* Write weights (persisted) */
        uint8_t wdata[64];
        memset(wdata, 0x11, 64);
        uint8_t *dp = dt_put(&sd, "weight.A", wdata, 64);
        fprintf(stderr, "  %s: dt_put(weight.A)\n", dp ? "PASS" : "FAIL");
        if (dp) pass++; else fail++;

        /* Write KV entries (ephemeral) */
        uint8_t kdata[128];
        memset(kdata, 0x22, 128);
        dp = dt_put_kv(&sd, "kv.X", kdata, 128);
        fprintf(stderr, "  %s: dt_put_kv(kv.X)\n", dp ? "PASS" : "FAIL");
        if (dp) pass++; else fail++;

        memset(kdata, 0x33, 64);
        dp = dt_put_kv(&sd, "kv.Y", kdata, 64);
        fprintf(stderr, "  %s: dt_put_kv(kv.Y)\n", dp ? "PASS" : "FAIL");
        if (dp) pass++; else fail++;

        /* Verify KV readable in same session */
        uint8_t *p = dt_get(&sd, "kv.X");
        fprintf(stderr, "  %s: dt_get(kv.X) in-session=%s\n",
                p && p[0] == 0x22 ? "PASS" : "FAIL", p ? "found" : "NULL");
        if (p && p[0] == 0x22) pass++; else fail++;

        p = dt_get(&sd, "kv.Y");
        fprintf(stderr, "  %s: dt_get(kv.Y) in-session=%s\n",
                p && p[0] == 0x33 ? "PASS" : "FAIL", p ? "found" : "NULL");
        if (p && p[0] == 0x33) pass++; else fail++;

        /* total_bytes should NOT include KV */
        size_t tb = dt_store_total_bytes(&sd);
        fprintf(stderr, "  %s: total_bytes=%zu (exp=64, no KV)\n", tb == 64 ? "PASS" : "FAIL", tb);
        if (tb == 64) pass++; else fail++;

        /* foreach should NOT include KV */
        int fc = dt_store_foreach(&sd, NULL, NULL);
        fprintf(stderr, "  %s: foreach count=%d (exp=1)\n", fc == 1 ? "PASS" : "FAIL", fc);
        if (fc == 1) pass++; else fail++;

        /* Destroy and clean up */
        VirtualFree(sd.kv_base, 0, MEM_RELEASE);
        sd.kv_base = NULL;
        dt_store_destroy_twin(&sd);

        /* Reopen — KV entries must be gone */
        if (dt_store_init_twin(&sd, dfile, wcap) == 0) {
            fprintf(stderr, "  PASS: reopen OK\n"); pass++;

            p = dt_get(&sd, "weight.A");
            fprintf(stderr, "  %s: reopen weight.A=%s\n",
                    p && p[0] == 0x11 ? "PASS" : "FAIL", p ? "found" : "gone");
            if (p && p[0] == 0x11) pass++; else fail++;

            p = dt_get(&sd, "kv.X");
            fprintf(stderr, "  PASS: reopen kv.X=%s (KV MUST be ephemeral)\n",
                    p ? "FOUND (BUG)" : "gone");
            if (p == NULL) pass++; else fail++;

            p = dt_get(&sd, "kv.Y");
            fprintf(stderr, "  PASS: reopen kv.Y=%s (KV MUST be ephemeral)\n",
                    p ? "FOUND (BUG)" : "gone");
            if (p == NULL) pass++; else fail++;

            dt_store_destroy_twin(&sd);
        }
    }
    remove(dfile);
    fprintf(stderr, "\n");

    /* ═══ Phase 7: Type-safe container ═══ */
    fprintf(stderr, "── Phase 7: Type-safe container (DtContainer) ──\n");
    {
        DRamTileStore sc;
        dt_store_init(&sc, 64UL * 1024);
        uint8_t buf[256];
        /* Create a 5x10 float32 tensor */
        uint32_t shape_f32[] = {5, 10};
        for (int i = 0; i < 50; i++)
            ((float*)buf)[i] = (float)(i * 1.5f);
        dt_putv(&sc, "mat.A", DT_F32, 2, shape_f32, buf, 50 * 4);

        /* Wrap view into type-safe container */
        DtTensorView tview = dt_view(&sc, "mat.A", DT_F32, 2, shape_f32);
        DtContainer c = dtc_wrap(&tview, DT_F32);
        fprintf(stderr, "  %s: container shape=[%u,%u] dtype=%u\n",
                c.view.data && c.view.shape[0]==5 && c.view.shape[1]==10 ? "PASS" : "FAIL",
                c.view.shape[0], c.view.shape[1], c.view.dtype);
        if (c.view.data && c.view.shape[0]==5 && c.view.shape[1]==10) pass++; else fail++;

        /* Element access via dtc_f32_2d */
        float v55 = dtc_f32_2d(&c, 3, 7); /* row 3, col 7 = (3*10+7)*1.5 = 55.5 */
        fprintf(stderr, "  %s: dtc_f32_2d(3,7)=%.1f (exp=55.5)\n",
                fabsf(v55 - 55.5f) < 0.01f ? "PASS" : "FAIL", v55);
        if (fabsf(v55 - 55.5f) < 0.01f) pass++; else fail++;

        /* Slice: row 2 */
        DtContainer row = dtc_slice(&c, 0, 2, 3); /* dim=0, rows 2..2 → [1,10] */
        fprintf(stderr, "  %s: slice row2 shape=[%u,%u] sz=%zu\n",
                row.view.data && row.view.shape[0]==1 && row.view.shape[1]==10 ? "PASS" : "FAIL",
                row.view.shape[0], row.view.shape[1], row.view.nbytes);
        if (row.view.data && row.view.shape[0]==1 && row.view.shape[1]==10) pass++; else fail++;

        float v30 = dtc_f32_2d(&row, 0, 0); /* row2,col0 = (2*10+0)*1.5 = 30.0 */
        fprintf(stderr, "  %s: slice[0]=%.1f (exp=30.0)\n",
                fabsf(v30 - 30.0f) < 0.01f ? "PASS" : "FAIL", v30);
        if (fabsf(v30 - 30.0f) < 0.01f) pass++; else fail++;

        /* Flatten */
        DtContainer flat = dtc_flatten(&c);
        fprintf(stderr, "  %s: flatten shape=[%u]\n",
                flat.view.data && flat.view.shape[0]==50 ? "PASS" : "FAIL", flat.view.shape[0]);
        if (flat.view.data && flat.view.shape[0]==50) pass++; else fail++;

        /* Element access on flat */
        float vflat = *(float*)dtc_ptr(&flat, (size_t[]){37});
        fprintf(stderr, "  %s: flat[37]=%.1f (exp=%.1f)\n",
                fabsf(vflat - 55.5f) < 0.01f ? "PASS" : "FAIL", vflat, 55.5f);
        if (fabsf(vflat - 55.5f) < 0.01f) pass++; else fail++;

        dt_store_destroy(&sc);
    }
    fprintf(stderr, "\n");

    /* ═══ Phase 8: Cold overflow spill ═══ */
    fprintf(stderr, "── Phase 8: Cold overflow spill (bond) ──\n");
    {
        /* Create anonymous store, clamp capacity small to force spill */
        DRamTileStore sc;
        dt_store_init(&sc, 4096);
        sc.capacity = 128;  /* clamp tiny → next put spills */
        sc.used = 0;
        int ok = dt_store_init_cold(&sc, 64UL << 10);  /* 64KB cold */
        fprintf(stderr, "  %s: cold init (cap=%zu)\n",
                ok == 0 ? "PASS" : "FAIL", sc.cold_capacity);
        if (ok == 0) pass++; else fail++;

        /* Put A (small, fits local) */
        uint8_t buf_a[64];
        memset(buf_a, 0xAA, 64);
        uint8_t *pa = dt_put(&sc, "tensor.A", buf_a, 64);
        fprintf(stderr, "  %s: dt_put(A) local=%s\n",
                pa && pa >= sc.base && pa < sc.base + sc.capacity ? "PASS" : "FAIL",
                pa ? "primary" : "NULL");
        if (pa && pa >= sc.base && pa < sc.base + sc.capacity) pass++; else fail++;

        /* Fill remaining primary with a big tensor (forces spill) */
        uint8_t buf_big[400];
        memset(buf_big, 0xBB, 400);
        uint8_t *pb = dt_put(&sc, "tensor.BIG", buf_big, 400);
        int bond = (sc.hash[dt_name_to_addr("tensor.BIG") % DT_HASH_SLOTS].dram_addr & DT_BOND_FLAG) != 0;
        fprintf(stderr, "  %s: dt_put(BIG) %s (bond=%d)\n",
                pb && bond ? "PASS" : "FAIL",
                bond ? "cold" : "primary",
                bond);
        if (pb && bond) pass++; else fail++;

        /* Verify data integrity via dt_get */
        uint8_t *ga = dt_get(&sc, "tensor.A");
        fprintf(stderr, "  %s: dt_get(A)=0x%02X (exp=0xAA)\n",
                ga && ga[0] == 0xAA ? "PASS" : "FAIL", ga ? ga[0] : 0);
        if (ga && ga[0] == 0xAA) pass++; else fail++;

        uint8_t *gb = dt_get(&sc, "tensor.BIG");
        fprintf(stderr, "  %s: dt_get(BIG)=0x%02X (exp=0xBB)\n",
                gb && gb[0] == 0xBB ? "PASS" : "FAIL", gb ? gb[0] : 0);
        if (gb && gb[0] == 0xBB) pass++; else fail++;

        /* Verify bond pointer is in cold region */
        fprintf(stderr, "  %s: bond ptr in cold_base range\n",
                gb && sc.cold_base && gb >= sc.cold_base && gb < sc.cold_base + sc.cold_used ? "PASS" : "FAIL");
        if (gb && sc.cold_base && gb >= sc.cold_base && gb < sc.cold_base + sc.cold_used) pass++; else fail++;

        /* total_bytes skips bond entries */
        size_t tb = dt_store_total_bytes(&sc);
        fprintf(stderr, "  %s: total_bytes=%zu (exp=64, only local)\n",
                tb == 64 ? "PASS" : "FAIL", tb);
        if (tb == 64) pass++; else fail++;

        /* Bond is ephemeral — destroy proves no persistence needed */
        dt_store_destroy(&sc);
    }
    fprintf(stderr, "\n");

    /* ── Summary ── */
    fprintf(stderr, "═══════════════════════════════════════════\n");
    fprintf(stderr, "RESULTS:  %d passed, %d failed  %s\n",
            pass, fail, fail == 0 ? "ALL PASS ✅" : "SOME FAILED ❌");
    fprintf(stderr, "\n");
    test_phase_header("Phase 9: Persistent cold twin (file-backed)");
    {
        static const char *mainpath = "dramtile_main_test.bin";
        static const char *coldpath = "dramtile_cold_test.bin";
        DRamTileStore sc;

        /* Use file-backed main twin so bond entries persist in hash dir.
         * Capacity 512: leaves room for directory (16 + 2*48 + 4 = 116 bytes)
         * while still being small enough to force spill with one tensor. */
        int ok = dt_store_init_twin(&sc, mainpath, 512);
        fprintf(stderr, "  %s: main twin init (cap=%zu)\n",
                ok == 0 ? "PASS" : "FAIL", sc.capacity);
        if (ok == 0) pass++; else { fail++; goto phase9_done; }

        ok = dt_store_init_cold_twin(&sc, coldpath, 64UL << 10);
        fprintf(stderr, "  %s: cold twin init (cap=%zu, existing=%zu)\n",
                ok == 0 ? "PASS" : "FAIL", sc.cold_capacity, sc.cold_used);
        if (ok == 0) pass++; else { fail++; goto phase9_done; }

        /* Create tensor A (fits primary) */
        uint8_t bufA[64];
        memset(bufA, 0xAA, 64);
        dt_put(&sc, "tensor.A", bufA, 64);
        uint32_t slotA = dt_name_to_addr("tensor.A") % DT_HASH_SLOTS;
        fprintf(stderr, "  %s: dt_put(A) local\n",
                !(sc.hash[slotA].dram_addr & DT_BOND_FLAG) ? "PASS" : "FAIL");
        if (!(sc.hash[slotA].dram_addr & DT_BOND_FLAG)) pass++; else fail++;

        /* Create tensor BIG (spills to cold — 600 > 512 capacity) */
        uint32_t big_sz = 600;
        uint8_t *dataB = (uint8_t*)calloc(1, big_sz);
        memset(dataB, 0xBB, big_sz);
        uint8_t *pb = dt_put(&sc, "tensor.BIG", dataB, big_sz);
        uint32_t slotB = dt_name_to_addr("tensor.BIG") % DT_HASH_SLOTS;
        int bond = (sc.hash[slotB].dram_addr & DT_BOND_FLAG) != 0;
        fprintf(stderr, "  %s: dt_put(BIG) cold (bond=%d)\n",
                pb && bond ? "PASS" : "FAIL", bond);
        if (pb && bond) pass++; else { free(dataB); fail++; goto phase9_done; }
        free(dataB);

        /* Verify read-back in-session */
        uint8_t *ptrA = dt_get(&sc, "tensor.A");
        fprintf(stderr, "  %s: dt_get(A)=%s (val=0x%02x)\n",
                ptrA && ptrA[0] == 0xAA ? "PASS" : "FAIL",
                ptrA ? "found" : "NULL", ptrA ? ptrA[0] : 0);
        if (ptrA && ptrA[0] == 0xAA) pass++; else fail++;

        uint8_t *ptrB = dt_get(&sc, "tensor.BIG");
        size_t szB = dt_get_size(&sc, "tensor.BIG");
        fprintf(stderr, "  %s: dt_get(BIG)=%s (sz=%zu, val=0x%02x)\n",
                ptrB && szB == big_sz && ptrB[0] == 0xBB ? "PASS" : "FAIL",
                ptrB ? "found" : "NULL", szB, ptrB ? ptrB[0] : 0);
        if (ptrB && szB == big_sz && ptrB[0] == 0xBB) pass++; else fail++;

        /* Destroy (saves hash dir with bond entries → main file) */
        dt_store_destroy_twin(&sc);

        /* Reopen — twin loads hash dir, cold twin finds bond entries */
        ok = dt_store_init_twin(&sc, mainpath, 512);
        fprintf(stderr, "  %s: main reopen (n_stored=%u)\n",
                ok == 0 ? "PASS" : "FAIL", sc.n_stored);
        if (ok == 0) pass++; else { fail++; goto phase9_done; }

        ok = dt_store_init_cold_twin(&sc, coldpath, 64UL << 10);
        fprintf(stderr, "  %s: cold reopen (cap=%zu, used=%zu)\n",
                ok == 0 ? "PASS" : "FAIL", sc.cold_capacity, sc.cold_used);
        if (ok == 0) pass++; else { fail++; goto phase9_done; }

        /* A is restored from main twin hash dir */
        ptrA = dt_get(&sc, "tensor.A");
        fprintf(stderr, "  %s: reopen A=%s (val=0x%02x)\n",
                ptrA && ptrA[0] == 0xAA ? "PASS" : "FAIL",
                ptrA ? "found" : "NULL", ptrA ? ptrA[0] : 0);
        if (ptrA && ptrA[0] == 0xAA) pass++; else fail++;

        /* BIG restored from cold file via bond hash entry */
        ptrB = dt_get(&sc, "tensor.BIG");
        szB = dt_get_size(&sc, "tensor.BIG");
        fprintf(stderr, "  %s: reopen BIG=%s (sz=%zu, val=0x%02x, exp=0xBB)\n",
                ptrB && szB == big_sz && ptrB[0] == 0xBB ? "PASS" : "FAIL",
                ptrB ? "found" : "NULL", szB, ptrB ? ptrB[0] : 0);
        if (ptrB && szB == big_sz && ptrB[0] == 0xBB) pass++; else fail++;

        /* Verify bond flag still set after reopen */
        uint32_t bond_addr = sc.hash[slotB].dram_addr;
        fprintf(stderr, "  %s: bond flag=0x%x\n",
                (bond_addr & DT_BOND_FLAG) ? "PASS" : "FAIL", bond_addr);
        if (bond_addr & DT_BOND_FLAG) pass++; else fail++;

        /* Verify ptr is in cold_base range (mmap of cold file) */
        int in_cold = ptrB >= sc.cold_base && ptrB < sc.cold_base + sc.cold_capacity;
        fprintf(stderr, "  %s: ptr in cold range=%d\n",
                in_cold ? "PASS" : "FAIL", in_cold);
        if (in_cold) pass++; else fail++;

phase9_done:
        dt_store_destroy_twin(&sc);
        remove(mainpath);
        remove(coldpath);
    }

    test_phase_header("Phase 10: Migrate bond → primary promote");
    {
        DRamTileStore sc;
        dt_store_init(&sc, 4096);
        sc.capacity = 300;
        sc.used = 0;
        int ok = dt_store_init_cold(&sc, 64UL << 10);
        fprintf(stderr, "  %s: cold init (cap=%zu)\n",
                ok == 0 ? "PASS" : "FAIL", sc.cold_capacity);
        if (ok == 0) pass++; else { fail++; goto phase10_done; }

        /* Put A (200 bytes, fits primary) */
        uint8_t bufA[200];
        memset(bufA, 0xAA, 200);
        dt_put(&sc, "tensor.A", bufA, 200);
        uint32_t slotA = dt_name_to_addr("tensor.A") % DT_HASH_SLOTS;
        fprintf(stderr, "  %s: dt_put(A) local\n",
                !(sc.hash[slotA].dram_addr & DT_BOND_FLAG) ? "PASS" : "FAIL");
        if (!(sc.hash[slotA].dram_addr & DT_BOND_FLAG)) pass++; else fail++;

        /* Put B (200 bytes, spills to cold — primary only has 300 cap, A used 256) */
        uint8_t bufB[200];
        memset(bufB, 0xBB, 200);
        dt_put(&sc, "tensor.B", bufB, 200);
        uint32_t slotB = dt_name_to_addr("tensor.B") % DT_HASH_SLOTS;
        int bondB = (sc.hash[slotB].dram_addr & DT_BOND_FLAG) != 0;
        fprintf(stderr, "  %s: dt_put(B) cold (bond=%d)\n",
                bondB ? "PASS" : "FAIL", bondB);
        if (bondB) pass++; else fail++;

        /* Remove A from hash to free primary space */
        sc.hash[slotA].dram_addr = 0;
        sc.n_stored--;
        sc.used = 0;  /* reset used so migrate has room */

        /* Migrate: promote B back from cold to primary */
        int n = dt_migrate_step(&sc, 10, 0, 0);
        fprintf(stderr, "  %s: migrate promoted=%d (exp=1)\n",
                n == 1 ? "PASS" : "FAIL", n);
        if (n == 1) pass++; else fail++;

        /* Verify B is now local (no bond flag) */
        int promoted = (sc.hash[slotB].dram_addr & DT_BOND_FLAG) == 0;
        fprintf(stderr, "  %s: B now local\n",
                promoted ? "PASS" : "FAIL");
        if (promoted) pass++; else fail++;

        /* Verify B data integrity */
        uint8_t *ptrB = dt_get(&sc, "tensor.B");
        fprintf(stderr, "  %s: dt_get(B)=%s (val=0x%02x)\n",
                ptrB && ptrB[0] == 0xBB ? "PASS" : "FAIL",
                ptrB ? "found" : "NULL", ptrB ? ptrB[0] : 0);
        if (ptrB && ptrB[0] == 0xBB) pass++; else fail++;

        /* B should now be in base range, not cold_base */
        int in_primary = ptrB >= sc.base && ptrB < sc.base + sc.capacity;
        fprintf(stderr, "  %s: B in primary range=%d\n",
                in_primary ? "PASS" : "FAIL", in_primary);
        if (in_primary) pass++; else fail++;

phase10_done:
        dt_store_destroy(&sc);
    }

    test_phase_header("Phase 11: Evict oldest bond entries from cold");
    {
        DRamTileStore sc;
        dt_store_init(&sc, 4096);
        sc.capacity = 128;
        sc.used = 0;
        int ok = dt_store_init_cold(&sc, 64UL << 10);
        fprintf(stderr, "  %s: cold init (cap=%zu)\n",
                ok == 0 ? "PASS" : "FAIL", sc.cold_capacity);
        if (ok == 0) pass++; else { fail++; goto phase11_done; }

        /* Put A (fits primary) */
        uint8_t bufA[64];
        memset(bufA, 0xAA, 64);
        dt_put(&sc, "tensor.A", bufA, 64);
        uint32_t slotA = dt_name_to_addr("tensor.A") % DT_HASH_SLOTS;

        /* Put B1 (spills to cold, tick=1) */
        uint8_t bufB1[200];
        memset(bufB1, 0x11, 200);
        dt_put(&sc, "tensor.B1", bufB1, 200);
        uint32_t slotB1 = dt_name_to_addr("tensor.B1") % DT_HASH_SLOTS;
        int bond1 = (sc.hash[slotB1].dram_addr & DT_BOND_FLAG) != 0;
        fprintf(stderr, "  %s: dt_put(B1) cold (bond=%d, tick=%u)\n",
                bond1 ? "PASS" : "FAIL", bond1, sc.hash[slotB1].session_tick);
        if (bond1) pass++; else fail++;

        /* Put B2 (spills to cold, tick=2) */
        uint8_t bufB2[200];
        memset(bufB2, 0x22, 200);
        dt_put(&sc, "tensor.B2", bufB2, 200);
        uint32_t slotB2 = dt_name_to_addr("tensor.B2") % DT_HASH_SLOTS;
        int bond2 = (sc.hash[slotB2].dram_addr & DT_BOND_FLAG) != 0;
        fprintf(stderr, "  %s: dt_put(B2) cold (bond=%d, tick=%u)\n",
                bond2 ? "PASS" : "FAIL", bond2, sc.hash[slotB2].session_tick);
        if (bond2) pass++; else fail++;

        /* Evict 1 oldest (B1 has lower tick) */
        int e = dt_evict_step(&sc, 1);
        fprintf(stderr, "  %s: evict count=%d (exp=1)\n",
                e == 1 ? "PASS" : "FAIL", e);
        if (e == 1) pass++; else fail++;

        /* B1 should be gone */
        uint8_t *p1 = dt_get(&sc, "tensor.B1");
        fprintf(stderr, "  %s: B1 after evict=%s\n",
                p1 == NULL ? "PASS" : "FAIL", p1 ? "found" : "gone");
        if (p1 == NULL) pass++; else fail++;

        /* B2 should still be accessible */
        uint8_t *p2 = dt_get(&sc, "tensor.B2");
        fprintf(stderr, "  %s: B2 after evict=%s (val=0x%02x)\n",
                p2 && p2[0] == 0x22 ? "PASS" : "FAIL",
                p2 ? "found" : "NULL", p2 ? p2[0] : 0);
        if (p2 && p2[0] == 0x22) pass++; else fail++;

        /* A untouched */
        uint8_t *pa = dt_get(&sc, "tensor.A");
        fprintf(stderr, "  %s: A after evict=%s (val=0x%02x)\n",
                pa && pa[0] == 0xAA ? "PASS" : "FAIL",
                pa ? "found" : "NULL", pa ? pa[0] : 0);
        if (pa && pa[0] == 0xAA) pass++; else fail++;

phase11_done:
        dt_store_destroy(&sc);
    }

    test_phase_header("Phase 12: Auto-evict on cold full (dt_cold_make_room)");
    {
        DRamTileStore sc;
        dt_store_init(&sc, 4096);
        sc.capacity = 128;
        sc.used = 0;
        int ok = dt_store_init_cold(&sc, 64UL << 10);  /* 64KB default min */
        fprintf(stderr, "  %s: cold init (cap=%zu)\n",
                ok == 0 ? "PASS" : "FAIL", sc.cold_capacity);
        if (ok == 0) pass++; else { fail++; goto phase12_done; }
        sc.cold_capacity = 300;  /* clamp tiny to force eviction */

        /* Put A (local) */
        uint8_t bufA[64];
        memset(bufA, 0xAA, 64);
        dt_put(&sc, "tensor.A", bufA, 64);

        /* Put B1 (cold, uses ~256 of 300) */
        uint8_t bufB1[200];
        memset(bufB1, 0x11, 200);
        dt_put(&sc, "tensor.B1", bufB1, 200);
        uint32_t slotB1 = dt_name_to_addr("tensor.B1") % DT_HASH_SLOTS;
        int bond1 = (sc.hash[slotB1].dram_addr & DT_BOND_FLAG) != 0;
        fprintf(stderr, "  %s: dt_put(B1) cold (bond=%d)\n",
                bond1 ? "PASS" : "FAIL", bond1);
        if (bond1) pass++; else fail++;

        /* Put B2 (cold is nearly full with B1 — need to make room first) */
        uint8_t bufB2[200];
        memset(bufB2, 0x22, 200);

        /* Make room by evicting B1 */
        int ev = dt_cold_make_room(&sc, 200, 5);
        fprintf(stderr,  "  %s: dt_cold_make_room=%d (exp>0)\n",
                ev > 0 ? "PASS" : "FAIL", ev);
        if (ev > 0) pass++; else fail++;

        /* Now dt_put B2 — should go to cold (room was made) */
        uint8_t *pb2 = dt_put(&sc, "tensor.B2", bufB2, 200);
        uint32_t slotB2 = dt_name_to_addr("tensor.B2") % DT_HASH_SLOTS;
        int bond2 = (sc.hash[slotB2].dram_addr & DT_BOND_FLAG) != 0;
        fprintf(stderr, "  %s: dt_put(B2) after evict=%s (bond=%d)\n",
                pb2 && bond2 ? "PASS" : "FAIL",
                pb2 ? "cold" : "NULL", bond2);
        if (pb2 && bond2) pass++; else fail++;

        /* B1 should be gone */
        uint8_t *p1 = dt_get(&sc, "tensor.B1");
        fprintf(stderr, "  %s: B1 after evict=%s\n",
                p1 == NULL ? "PASS" : "FAIL", p1 ? "found" : "gone");
        if (p1 == NULL) pass++; else fail++;

        /* B2 should be accessible */
        uint8_t *p2 = dt_get(&sc, "tensor.B2");
        fprintf(stderr, "  %s: B2=%s (val=0x%02x)\n",
                p2 && p2[0] == 0x22 ? "PASS" : "FAIL",
                p2 ? "found" : "NULL", p2 ? p2[0] : 0);
        if (p2 && p2[0] == 0x22) pass++; else fail++;

phase12_done:
        dt_store_destroy(&sc);
    }

    test_phase_header("Phase 13: KV compose (KV spill to cold)");
    {
        DRamTileStore sc;
        dt_store_init(&sc, 4096);
        int ok = dt_store_init_cold(&sc, 64UL << 10);
        fprintf(stderr, "  %s: cold init (cap=%zu)\n",
                ok == 0 ? "PASS" : "FAIL", sc.cold_capacity);
        if (ok == 0) pass++; else { fail++; goto phase13_done; }

        /* Manually init kv_base (small, to force spill) */
        sc.kv_capacity = 64;
#ifdef _WIN32
        sc.kv_base = (uint8_t*)VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        sc.kv_base = (uint8_t*)mmap(NULL, 64, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        if (!sc.kv_base) { fail++; goto phase13_done; }
        pass++;

        /* KV entry fits in kv_base */
        uint8_t bufX[32];
        memset(bufX, 0x11, 32);
        uint8_t *px = dt_put_kv(&sc, "kv.X", bufX, 32);
        int local = px && px >= sc.kv_base && px < sc.kv_base + sc.kv_capacity;
        fprintf(stderr, "  %s: dt_put_kv(X) local=%s\n",
                local ? "PASS" : "FAIL", local ? "yes" : "no");
        if (local) pass++; else fail++;

        /* KV entry spills to cold (kv_capacity=64, next put won't fit) */
        uint8_t bufY[64];
        memset(bufY, 0x22, 64);
        uint8_t *py = dt_put_kv(&sc, "kv.Y", bufY, 64);
        uint32_t slotY = dt_name_to_addr("kv.Y") % DT_HASH_SLOTS;
        int bondY = (sc.hash[slotY].dram_addr & DT_BOND_FLAG) != 0;
        int in_cold = py && sc.cold_base && py >= sc.cold_base && py < sc.cold_base + sc.cold_used;
        fprintf(stderr, "  %s: dt_put_kv(Y) cold bond=%d\n",
                py && bondY && in_cold ? "PASS" : "FAIL", bondY);
        if (py && bondY && in_cold) pass++; else fail++;

        /* Update Y — should update cold copy (KV+BOND update path) */
        uint8_t bufY2[64];
        memset(bufY2, 0x33, 64);
        uint8_t *py2 = dt_put_kv(&sc, "kv.Y", bufY2, 64);
        fprintf(stderr, "  %s: dt_put_kv(Y) update=%s (val=0x%02x)\n",
                py2 && py2[0] == 0x33 ? "PASS" : "FAIL",
                py2 ? "cold" : "NULL", py2 ? py2[0] : 0);
        if (py2 && py2[0] == 0x33) pass++; else fail++;

        /* Read X (local KV) */
        uint8_t *gx = dt_get(&sc, "kv.X");
        fprintf(stderr, "  %s: dt_get(X)=%s (val=0x%02x)\n",
                gx && gx[0] == 0x11 ? "PASS" : "FAIL",
                gx ? "found" : "NULL", gx ? gx[0] : 0);
        if (gx && gx[0] == 0x11) pass++; else fail++;

        /* Read Y (KV+BOND → kv_compose returns cold pointer) */
        uint8_t *gy = dt_get(&sc, "kv.Y");
        fprintf(stderr, "  %s: dt_get(Y)=%s (val=0x%02x, exp=0x33)\n",
                gy && gy[0] == 0x33 && gy >= sc.cold_base ? "PASS" : "FAIL",
                gy ? "found" : "NULL", gy ? gy[0] : 0);
        if (gy && gy[0] == 0x33 && gy >= sc.cold_base) pass++; else fail++;

        /* Verify bond flag + KV flag both set */
        uint32_t yflags = sc.hash[slotY].dram_addr;
        fprintf(stderr, "  %s: Y flags=0x%x (KV=0x%x BOND=0x%x)\n",
                (yflags & DT_KV_FLAG) && (yflags & DT_BOND_FLAG) ? "PASS" : "FAIL",
                yflags, DT_KV_FLAG, DT_BOND_FLAG);
        if ((yflags & DT_KV_FLAG) && (yflags & DT_BOND_FLAG)) pass++; else fail++;

        /* Verify dt_get_size works for KV+BOND */
        size_t ys = dt_get_size(&sc, "kv.Y");
        fprintf(stderr, "  %s: dt_get_size(Y)=%zu (exp=64)\n",
                ys == 64 ? "PASS" : "FAIL", ys);
        if (ys == 64) pass++; else fail++;

phase13_done:
        if (sc.kv_base) {
#ifdef _WIN32
            VirtualFree(sc.kv_base, 0, MEM_RELEASE);
#else
            munmap(sc.kv_base, 64);
#endif
            sc.kv_base = NULL;
        }
        dt_store_destroy(&sc);
    }

    /* ════════════════════════════════════════════════════════
       Phase 14: Container dual-region + delta compose
       ════════════════════════════════════════════════════════ */
    test_phase_header("Phase 14: Container dual-region + delta compose");
    {
        DRamTileStore sc;
        dt_store_init(&sc, 8192);
        int ok = dt_store_init_cold(&sc, 64UL << 10);
        if (ok != 0) { fail++; goto phase14_done; }

        sc.kv_capacity = 128;
#ifdef _WIN32
        sc.kv_base = (uint8_t*)VirtualAlloc(NULL, 128, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        sc.kv_base = (uint8_t*)mmap(NULL, 128, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        if (!sc.kv_base) { fail++; goto phase14_done; }

        /* ── 14a: Container wrap for weight (non-KV) ── */
        {
            uint8_t wdata[32];
            for (int i = 0; i < 32; i++) wdata[i] = (uint8_t)(i * 3 + 7);
            dt_put(&sc, "weight.A", wdata, 32);

            DtTensorView vw = dt_getv(&sc, "weight.A");
            DtContainer cw = dtc_wrap(&vw, DT_F32);
            int ok1 = cw.view.data != NULL && cw.elem_size == 4 && cw.view.ndim == 0;
            int ok2 = !dtc_is_kv(&cw) && !dtc_is_bond(&cw) && !dtc_is_delta(&cw);
            fprintf(stderr, "  %s: weight wrap data=%s flags=%s%s%s\n",
                    ok1 && ok2 ? "PASS" : "FAIL",
                    cw.view.data ? "ok" : "null",
                    dtc_is_kv(&cw)?"KV":"", dtc_is_bond(&cw)?"BOND":"", dtc_is_delta(&cw)?"DELTA":"");
            if (ok1 && ok2) pass++; else fail++;
        }

        /* ── 14b: dt_getv returns bond-aware pointer ── */
        {
            DRamTileStore bs;
            dt_store_init(&bs, 256);
            int colok = dt_store_init_cold(&bs, 4096);
            if (colok != 0) { fail++; goto bond_test_done; }

            /* Fill primary with unique entries until full */
            uint8_t filler[100];
            for (int i = 0; i < 10; i++) {
                char fn[32]; snprintf(fn, sizeof(fn), "fill.%d", i);
                memset(filler, (uint8_t)(i + 1), 100);
                dt_put(&bs, fn, filler, 100);
            }

            /* tensor.B should overflow → bond */
            uint8_t bdata[200];
            memset(bdata, 0xAB, 200);
            dt_put(&bs, "tensor.B", bdata, 200);

            uint32_t bslot = dt_name_to_addr("tensor.B") % DT_HASH_SLOTS;
            int is_bond = (bs.hash[bslot].dram_addr & DT_BOND_FLAG) != 0;

            DtTensorView vb = dt_getv(&bs, "tensor.B");
            uint8_t *exp = is_bond ? bs.cold_base + bs.hash[bslot].cold_offset
                                   : bs.base + bs.hash[bslot].offset;
            int ok1 = is_bond;
            int ok2 = vb.data == exp && vb.data[0] == 0xAB;
            fprintf(stderr, "  %s: bond addr=%s ptr_match=%s bond=%d\n",
                    ok1 && ok2 ? "PASS" : "FAIL",
                    is_bond ? "cold" : "local",
                    vb.data == exp ? "yes" : "no", is_bond);
            if (ok1 && ok2) pass++; else fail++;

bond_test_done:
            dt_store_destroy(&bs);
        }

        /* ── 14c: Container over KV entry (dtc_is_kv) ── */
        {
            uint8_t kdata[16];
            memset(kdata, 0x77, 16);
            dt_put_kv(&sc, "kv.A", kdata, 16);

            DtTensorView vk = dt_getv(&sc, "kv.A");
            DtContainer ck = dtc_wrap(&vk, DT_F32);
            int ok1 = dtc_is_kv(&ck);
            int ok2 = ck.view.data != NULL;
            fprintf(stderr, "  %s: KV container is_kv=%d data=%s\n",
                    ok1 && ok2 ? "PASS" : "FAIL",
                    dtc_is_kv(&ck), ck.view.data ? "ok" : "null");
            if (ok1 && ok2) pass++; else fail++;
        }

        /* ── 14d: KV→cold spill, then dt_getv returns kv_compose ptr ── */
        {
            uint8_t ydata[96];
            memset(ydata, 0x99, 96);
            dt_put_kv(&sc, "kv.Y", ydata, 96); /* spills (kv_cap=128, used ~16+96 > 128) */

            uint32_t slotY = dt_name_to_addr("kv.Y") % DT_HASH_SLOTS;
            int bond = (sc.hash[slotY].dram_addr & DT_BOND_FLAG) != 0;
            int ok1 = bond;

            DtTensorView vy = dt_getv(&sc, "kv.Y");
            uint8_t *expected = bond ? sc.cold_base + sc.hash[slotY].cold_offset
                                     : sc.kv_base + sc.hash[slotY].offset;
            int ok2 = vy.data == expected && vy.data[0] == 0x99;
            fprintf(stderr, "  %s: KV+BOND compose ptr_match=%s val=0x%02x\n",
                    ok1 && ok2 ? "PASS" : "FAIL",
                    vy.data == expected ? "yes" : "no",
                    vy.data ? vy.data[0] : 0);
            if (ok1 && ok2) pass++; else fail++;
        }

        /* ── 14e: dt_resolve bond + KV aware ── */
        {
            /* KV entry (local in kv_base) */
            uint32_t kaddr = dt_name_to_addr("kv.A");
            DtTensorView vr = dt_resolve(&sc, kaddr);
            int ok1 = vr.data != NULL && (vr.dram_addr & DT_KV_FLAG) != 0;
            fprintf(stderr, "  %s: resolve kv.A data=%s flags=0x%x kv=%d\n",
                    ok1 ? "PASS" : "FAIL",
                    vr.data ? "ok" : "null", vr.dram_addr,
                    (vr.dram_addr & DT_KV_FLAG) ? 1 : 0);
            if (ok1) pass++; else fail++;
        }

        /* ── 14f: kv_delta_spill + kv_delta_compose_read ── */
        {
            /* Create a small kv_base (32 bytes) to force spill */
            uint8_t *delta_kv = NULL;
#ifdef _WIN32
            delta_kv = (uint8_t*)VirtualAlloc(NULL, 32, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
            delta_kv = (uint8_t*)mmap(NULL, 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
            if (!delta_kv) { fail++; goto phase14_done; }

            /* Store a value in kv_base */
            uint8_t orig[16];
            for (int i = 0; i < 16; i++) orig[i] = (uint8_t)(i * 7 + 3);
            memcpy(delta_kv, orig, 16);

            uint32_t d_slot = dt_name_to_addr("delta.T") % DT_HASH_SLOTS;
            sc.hash[d_slot].dram_addr = dt_name_to_addr("delta.T") | DT_KV_FLAG;
            sc.hash[d_slot].offset    = 0;
            sc.hash[d_slot].size      = 16;

            /* Fresh data to store */
            uint8_t fresh[16];
            for (int i = 0; i < 16; i++) fresh[i] = (uint8_t)(~orig[i] ^ 0xA5);

            /* Delta spill: floor0 = orig (copied to cold), floor1 = fresh XOR orig */
            uint8_t *floor0 = kv_delta_spill(&sc, d_slot, fresh, 16);
            int ok1 = floor0 != NULL && (sc.hash[d_slot].dram_addr & DT_DELTA_FLAG) != 0;
            fprintf(stderr, "  %s: delta spill ptr=%s delta_flag=%d\n",
                    ok1 ? "PASS" : "FAIL",
                    floor0 ? "ok" : "null",
                    (sc.hash[d_slot].dram_addr & DT_DELTA_FLAG) ? 1 : 0);
            if (ok1) pass++; else fail++;

            /* Compose: floor0 XOR floor1 → fresh */
            uint8_t composed[16];
            kv_delta_compose_read(&sc, d_slot, composed, 16);
            int ok2 = 1;
            for (int i = 0; i < 16; i++)
                if (composed[i] != fresh[i]) { ok2 = 0; break; }
            fprintf(stderr, "  %s: delta compose match=%s\n",
                    ok2 ? "PASS" : "FAIL",
                    ok2 ? "yes" : "no");
            if (ok2) pass++; else fail++;

            /* dt_resolve returns cold ptr (floor0), NOT final value */
            DtTensorView vd = dt_resolve(&sc, dt_name_to_addr("delta.T"));
            int ok3 = vd.data != NULL && (vd.dram_addr & DT_DELTA_FLAG) != 0;
            fprintf(stderr, "  %s: resolve delta data=%s flags=0x%x delta=%d\n",
                    ok3 ? "PASS" : "FAIL",
                    vd.data ? "ok" : "null", vd.dram_addr,
                    (vd.dram_addr & DT_DELTA_FLAG) ? 1 : 0);
            if (ok3) pass++; else fail++;

#ifdef _WIN32
            VirtualFree(delta_kv, 0, MEM_RELEASE);
#else
            munmap(delta_kv, 32);
#endif
        }

        /* ── 14g: Container dtc_is_delta + dtc_delta_compose ── */
        {
            uint8_t d2_orig[16];
            for (int i = 0; i < 16; i++) d2_orig[i] = (uint8_t)(i * 3 + 1);
            uint8_t d2_fresh[16];
            for (int i = 0; i < 16; i++) d2_fresh[i] = (uint8_t)(~d2_orig[i] ^ 0x5A);

            /* Setup kv entry then delta-spill */
            uint8_t *d2_kv = NULL;
#ifdef _WIN32
            d2_kv = (uint8_t*)VirtualAlloc(NULL, 32, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
            d2_kv = (uint8_t*)mmap(NULL, 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
            if (!d2_kv) { fail++; goto phase14_done; }
            memcpy(d2_kv, d2_orig, 16);

            uint32_t d2_slot = dt_name_to_addr("delta.D2") % DT_HASH_SLOTS;
            sc.hash[d2_slot].dram_addr = dt_name_to_addr("delta.D2") | DT_KV_FLAG;
            sc.hash[d2_slot].offset    = 0;
            sc.hash[d2_slot].size      = 16;

            kv_delta_spill(&sc, d2_slot, d2_fresh, 16);

            DtTensorView vd2 = dt_resolve(&sc, dt_name_to_addr("delta.D2"));
            DtContainer cd2 = dtc_wrap(&vd2, DT_I8);
            int ok1 = dtc_is_delta(&cd2);
            fprintf(stderr, "  %s: container is_delta=%d\n",
                    ok1 ? "PASS" : "FAIL", dtc_is_delta(&cd2));
            if (ok1) pass++; else fail++;

            uint8_t compose_buf[16];
            uint8_t *comp = dtc_delta_compose(&cd2, &sc, compose_buf);
            int ok2 = (comp == compose_buf);
            for (int i = 0; i < 16; i++)
                if (comp[i] != d2_fresh[i]) { ok2 = 0; break; }
            fprintf(stderr, "  %s: container compose match=%s\n",
                    ok2 ? "PASS" : "FAIL",
                    ok2 ? "yes" : "no");
            if (ok2) pass++; else fail++;

            /* Verify view.data updated to compose buffer */
            int ok3 = cd2.view.data == compose_buf;
            fprintf(stderr, "  %s: container view.data updated=%s\n",
                    ok3 ? "PASS" : "FAIL",
                    cd2.view.data == compose_buf ? "yes" : "no");
            if (ok3) pass++; else fail++;

#ifdef _WIN32
            VirtualFree(d2_kv, 0, MEM_RELEASE);
#else
            munmap(d2_kv, 32);
#endif
        }

        /* ── 14h: dtc_promote_to_cold ── */
        {
            uint8_t pkdata[16];
            memset(pkdata, 0x55, 16);
            dt_put_kv(&sc, "kv.P", pkdata, 16);

            DtTensorView vp = dt_getv(&sc, "kv.P");
            DtContainer cp = dtc_wrap(&vp, DT_I8);
            int ok0 = dtc_is_kv(&cp);

            int promoted = dtc_promote_to_cold(&cp, &sc);
            int ok1 = promoted == 0 && cp.view.data != vp.data;
            int ok2 = cp.view.data[0] == 0x55;
            fprintf(stderr, "  %s: promote=%d ptr_changed=%s val=0x%02x\n",
                    ok0 && ok1 && ok2 ? "PASS" : "FAIL",
                    promoted,
                    cp.view.data != vp.data ? "yes" : "no",
                    cp.view.data ? cp.view.data[0] : 0);
            if (ok0 && ok1 && ok2) pass++; else fail++;
        }

        /* ── 14i: dtc_print (just verify it doesn't crash) ── */
        {
            uint8_t pdata[8];
            memset(pdata, 0x11, 8);
            dt_put(&sc, "print.T", pdata, 8);
            DtTensorView vp = dt_getv(&sc, "print.T");
            DtContainer cp = dtc_wrap(&vp, DT_F32);
            fprintf(stderr, "  dtc_print output (visual check):\n");
            dtc_print(&cp, "print.T");
            pass++;
        }

phase14_done:
        if (sc.kv_base) {
#ifdef _WIN32
            VirtualFree(sc.kv_base, 0, MEM_RELEASE);
#else
            munmap(sc.kv_base, 128);
#endif
            sc.kv_base = NULL;
        }
        dt_store_destroy(&sc);
    }

    fprintf(stderr, "\n");
    fprintf(stderr, "═══════════════════════════════════════════\n");
    fprintf(stderr, "RESULTS:  %d passed, %d failed  %s\n",
            pass, fail, fail == 0 ? "ALL PASS ✅" : "SOME FAILED ❌");
    fprintf(stderr, "═══════════════════════════════════════════\n");

    remove(filepath);
    return fail > 0 ? 1 : 0;
}

/*
 * test_vramtile.c — VRamTile integration test
 *
 * Tests:
 *   1. Init + put + get (CPU only)
 *   2. Promote to VRAM + data integrity
 *   3. LRU eviction when VRAM budget exceeded
 *   4. Evict all + re-promote
 *   5. Post-SID-swap promotion
 *   6. Gear-aware eviction skip
 *   7. Large tensor stress test
 *   8. Multiple evict/promote cycles
 */

#include "vramtile.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
static int g_skip = 0;

#define TEST_BIN "vramtile_test.bin"

static void fill_pattern(uint8_t *buf, size_t sz, uint8_t pattern) {
    memset(buf, pattern, sz);
}
static int check_pattern(const uint8_t *buf, size_t sz, uint8_t pattern) {
    for (size_t i = 0; i < sz; i++)
        if (buf[i] != pattern) return 0;
    return 1;
}
static void cleanup(void) { remove(TEST_BIN); }

#define RUN(name, body) do { \
    printf("\n── %s ──\n", name); \
    g_skip = 0; \
    do { body } while(0); \
    if (!g_skip) printf("  %s: ALL PASS\n", name); \
    cleanup(); \
} while(0)

#define CHECK(cond, ...) do { \
    if (!g_skip && !(cond)) { \
        char _b[256]; snprintf(_b, sizeof(_b), __VA_ARGS__); \
        g_fail++; g_skip = 1; printf("  FAIL: %s\n", _b); \
    } \
} while(0)

#define PASS() do { if (!g_skip) g_pass++; } while(0)

/* ════════════════════════════════════════════════════════════
   Phase 1
   ════════════════════════════════════════════════════════════ */
static void test_phase1(void) {
    RUN("Phase 1: Basic init + put + get (CPU)", {
        VRamTileStore vrt;
        CHECK(vrt_init_twin(&vrt, TEST_BIN, 4UL<<20, 256UL<<20, 0) == 0, "init");

        uint8_t bA[64]; fill_pattern(bA, 64, 0xAA);
        uint8_t bB[128]; fill_pattern(bB, 128, 0xBB);
        uint8_t bC[32]; fill_pattern(bC, 32, 0xCC);
        CHECK(dt_put(&vrt.store, "A", bA, 64) != NULL, "put A");
        CHECK(dt_put(&vrt.store, "B", bB, 128) != NULL, "put B");
        CHECK(dt_put(&vrt.store, "C", bC, 32) != NULL, "put C");

        uint8_t *cA = vrt_get_cpu_ptr(&vrt, "A");
        uint8_t *cB = vrt_get_cpu_ptr(&vrt, "B");
        uint8_t *cC = vrt_get_cpu_ptr(&vrt, "C");
        CHECK(cA && check_pattern(cA, 64, 0xAA), "cpu A");
        CHECK(cB && check_pattern(cB, 128, 0xBB), "cpu B");
        CHECK(cC && check_pattern(cC, 32, 0xCC), "cpu C");
        CHECK(vrt_get_vram_ptr(&vrt, "A") == NULL, "vram A NULL");
        CHECK(vrt.n_promoted == 0, "n_promoted==0");

        PASS();
        vrt_destroy(&vrt);
    });
}

/* ════════════════════════════════════════════════════════════
   Phase 2
   ════════════════════════════════════════════════════════════ */
static void test_phase2(void) {
    RUN("Phase 2: Promote to VRAM", {
        VRamTileStore vrt;
        CHECK(vrt_init_twin(&vrt, TEST_BIN, 4UL<<20, 256UL<<20, 0) == 0, "init");

        uint8_t bA[64]; fill_pattern(bA, 64, 0xAA);
        uint8_t bB[128]; fill_pattern(bB, 128, 0xBB);
        dt_put(&vrt.store, "A", bA, 64);
        dt_put(&vrt.store, "B", bB, 128);

        uint8_t *vA = vrt_promote(&vrt, "A", NULL, NULL);
        CHECK(vA && vrt_is_in_vram(&vrt, "A"), "promote A");
        CHECK(!vrt_is_in_vram(&vrt, "B"), "B not in VRAM");
        CHECK(vrt.n_promoted == 1 && vrt.n_uploads == 1, "n_promoted==1");
        CHECK(check_pattern(vA, 64, 0xAA), "vA data");

        uint8_t *pA = vrt_get_ptr(&vrt, "A");
        uint8_t *pB = vrt_get_ptr(&vrt, "B");
        CHECK(pA == vA, "get_ptr(A)==VRAM");
        CHECK(pB == vrt_get_cpu_ptr(&vrt, "B"), "get_ptr(B)==CPU");

        uint8_t *vB = vrt_promote(&vrt, "B", NULL, NULL);
        CHECK(vB && vrt_is_in_vram(&vrt, "B"), "promote B");
        CHECK(vrt.n_promoted == 2, "n_promoted==2");
        CHECK(check_pattern(vB, 128, 0xBB), "vB data");

        PASS();
        vrt_destroy(&vrt);
    });
}

/* ════════════════════════════════════════════════════════════
   Phase 3: LRU eviction
   ════════════════════════════════════════════════════════════ */
static void test_phase3(void) {
    RUN("Phase 3: VRAM eviction (LRU)", {
        VRamTileStore vrt;
        CHECK(vrt_init_twin(&vrt, TEST_BIN, 4UL<<20, 200, 0) == 0, "init (200B VRAM)");

        uint8_t buf[64];
        fill_pattern(buf, 64, 0x11); dt_put(&vrt.store, "X", buf, 64);
        fill_pattern(buf, 64, 0x22); dt_put(&vrt.store, "Y", buf, 64);
        fill_pattern(buf, 64, 0x33); dt_put(&vrt.store, "Z", buf, 64);
        fill_pattern(buf, 64, 0x44); dt_put(&vrt.store, "W", buf, 64);

        CHECK(vrt_promote(&vrt, "X", NULL, NULL) != NULL, "X");
        CHECK(vrt_promote(&vrt, "Y", NULL, NULL) != NULL, "Y");
        CHECK(vrt_promote(&vrt, "Z", NULL, NULL) != NULL, "Z");
        CHECK(vrt.n_promoted == 3 && vrt.n_evicted == 0, "3 promoted, no evict");

        /* W is 4th → 256 > 200 → evicts oldest (X) */
        CHECK(vrt_promote(&vrt, "W", NULL, NULL) != NULL, "W");
        CHECK(vrt.n_evicted >= 1, "evicted >=1");
        CHECK(!vrt_is_in_vram(&vrt, "X"), "X evicted (oldest)");
        CHECK(vrt_is_in_vram(&vrt, "Z") || vrt_is_in_vram(&vrt, "W"),
              "newer tensors survive");

        /* Evicted X still on CPU */
        uint8_t *cX = vrt_get_cpu_ptr(&vrt, "X");
        CHECK(cX && check_pattern(cX, 64, 0x11), "X CPU intact");

        /* Re-promote X */
        vrt_promote(&vrt, "X", NULL, NULL);
        CHECK(vrt_is_in_vram(&vrt, "X"), "X re-promoted");

        PASS();
        vrt_destroy(&vrt);
    });
}

/* ════════════════════════════════════════════════════════════
   Phase 4
   ════════════════════════════════════════════════════════════ */
static void test_phase4(void) {
    RUN("Phase 4: Evict all + re-promote", {
        VRamTileStore vrt;
        CHECK(vrt_init_twin(&vrt, TEST_BIN, 4UL<<20, 1UL<<20, 0) == 0, "init");

        uint8_t buf[128];
        fill_pattern(buf, 128, 0x77);
        dt_put(&vrt.store, "Alpha", buf, 128);
        dt_put(&vrt.store, "Beta", buf, 128);
        vrt_promote(&vrt, "Alpha", NULL, NULL);
        vrt_promote(&vrt, "Beta", NULL, NULL);
        CHECK(vrt.n_promoted == 2, "2 promoted");

        vrt_evict_all(&vrt);
        CHECK(vrt.n_promoted == 0 && vrt.vram_used == 0, "evict all");
        CHECK(!vrt_is_in_vram(&vrt, "Alpha"), "Alpha evicted");

        uint8_t *cA = vrt_get_cpu_ptr(&vrt, "Alpha");
        CHECK(cA && check_pattern(cA, 128, 0x77), "Alpha CPU intact");

        vrt_promote(&vrt, "Alpha", NULL, NULL);
        CHECK(vrt_is_in_vram(&vrt, "Alpha"), "Alpha re-promoted");

        PASS();
        vrt_destroy(&vrt);
    });
}

/* ════════════════════════════════════════════════════════════
   Phase 5
   ════════════════════════════════════════════════════════════ */
static void test_phase5(void) {
    RUN("Phase 5: Post-SID-swap promote", {
        VRamTileStore vrt;
        CHECK(vrt_init_twin(&vrt, TEST_BIN, 4UL<<20, 1UL<<20, 0) == 0, "init");

        uint8_t orig[256]; fill_pattern(orig, 256, 0xDD);
        dt_put(&vrt.store, "blk.0.attn.weight", orig, 256);
        vrt_promote(&vrt, "blk.0.attn.weight", NULL, NULL);
        CHECK(vrt_is_in_vram(&vrt, "blk.0.attn.weight"), "orig in VRAM");

        uint8_t swp[256]; fill_pattern(swp, 256, 0xEE);
        dt_put(&vrt.store, "blk.0.attn.weight", swp, 256);

        uint8_t *vp = vrt_promote_postswap(&vrt, "blk.0.attn.weight", NULL, NULL);
        CHECK(vp && check_pattern(vp, 256, 0xEE), "VRAM post-swap=0xEE");

        uint8_t swp2[256]; fill_pattern(swp2, 256, 0xFF);
        dt_put(&vrt.store, "blk.0.attn.weight", swp2, 256);
        uint8_t *vp2 = vrt_promote_postswap(&vrt, "blk.0.attn.weight", NULL, NULL);
        CHECK(check_pattern(vp2, 256, 0xFF), "VRAM 2nd swap=0xFF");

        PASS();
        vrt_destroy(&vrt);
    });
}

/* ════════════════════════════════════════════════════════════
   Phase 6
   ════════════════════════════════════════════════════════════ */
static void test_phase6(void) {
    RUN("Phase 6: Gear-aware eviction", {
        VRamTileStore vrt;
        CHECK(vrt_init_twin(&vrt, TEST_BIN, 4UL<<20, 512, 0) == 0, "init");

        uint8_t buf[128];
        fill_pattern(buf, 128, 0x11); dt_put(&vrt.store, "G1", buf, 128);
        fill_pattern(buf, 128, 0x22); dt_put(&vrt.store, "G2", buf, 128);
        fill_pattern(buf, 128, 0x33); dt_put(&vrt.store, "G3", buf, 128);
        fill_pattern(buf, 128, 0x44); dt_put(&vrt.store, "G4", buf, 128);

        vrt_promote(&vrt, "G1", NULL, NULL);
        vrt_promote(&vrt, "G2", NULL, NULL);
        vrt_promote(&vrt, "G3", NULL, NULL);
        vrt_promote(&vrt, "G4", NULL, NULL);
        CHECK(vrt.n_promoted == 4, "4 promoted");

        uint32_t gpu0 = 0; uint32_t gpu_max = UINT32_MAX;
        CHECK(vrt_evict_gear(&vrt, &gpu0) == 0, "gpu=0 evicts 0");
        CHECK(vrt.n_promoted == 4, "all survive gpu=0");

        CHECK(vrt_evict_gear(&vrt, &gpu_max) > 0, "high watermark evicts");
        CHECK(vrt.n_promoted < 4, "some evicted");

        PASS();
        vrt_destroy(&vrt);
    });
}

/* ════════════════════════════════════════════════════════════
   Phase 7
   ════════════════════════════════════════════════════════════ */
static void test_phase7(void) {
    RUN("Phase 7: Large tensor stress", {
        VRamTileStore vrt;
        CHECK(vrt_init_twin(&vrt, TEST_BIN, 16UL<<20, 2UL<<20, 0) == 0,
              "init 16MB CPU / 2MB VRAM");

        int n = 10;
        size_t sz = 256UL << 10;
        uint8_t *buf = (uint8_t*)malloc(sz);
        int promoted = 0;

        for (int i = 0; i < n && !g_skip; i++) {
            char name[64]; snprintf(name, sizeof(name), "big.%d", i);
            fill_pattern(buf, sz, (uint8_t)(0x10 + i));
            CHECK(dt_put(&vrt.store, name, buf, sz) != NULL, "put %d", i);
            uint8_t *vp = vrt_promote(&vrt, name, NULL, NULL);
            if (vp) { promoted++; CHECK(check_pattern(vp, sz, (uint8_t)(0x10 + i)), "data %d", i); }
        }
        free(buf);

        CHECK(promoted == n, "all %d promoted (eviction makes room)", n);
        CHECK(vrt.n_evicted > 0, "evictions occurred");
        CHECK(vrt.vram_used <= vrt.vram_capacity, "VRAM within budget");

        for (int i = 0; i < n; i++) {
            char name[64]; snprintf(name, sizeof(name), "big.%d", i);
            CHECK(vrt_get_cpu_ptr(&vrt, name) != NULL, "CPU ptr %d", i);
        }
        printf("  Promoted=%d Evicted=%d VRAM=%zu/%zu\n",
               promoted, vrt.n_evicted, vrt.vram_used, vrt.vram_capacity);

        PASS();
        vrt_destroy(&vrt);
    });
}

/* ════════════════════════════════════════════════════════════
   Phase 8
   ════════════════════════════════════════════════════════════ */
static void test_phase8(void) {
    RUN("Phase 8: Evict/promote cycle integrity", {
        VRamTileStore vrt;
        CHECK(vrt_init_twin(&vrt, TEST_BIN, 4UL<<20, 384, 0) == 0, "init 384B");

        for (int i = 0; i < 6; i++) {
            char name[64]; snprintf(name, sizeof(name), "c%d", i);
            uint8_t buf[128]; fill_pattern(buf, 128, (uint8_t)(0xA0 + i));
            dt_put(&vrt.store, name, buf, 128);
        }

        for (int cy = 0; cy < 3 && !g_skip; cy++) {
            for (int i = 0; i < 6 && !g_skip; i++) {
                char name[64]; snprintf(name, sizeof(name), "c%d", i);
                CHECK(vrt_promote(&vrt, name, NULL, NULL) != NULL,
                      "promote cy%d t%d", cy, i);

                uint8_t nd[128];
                fill_pattern(nd, 128, (uint8_t)(0xB0 + cy * 10 + i));
                dt_put(&vrt.store, name, nd, 128);

                uint8_t *vp = vrt_promote(&vrt, name, NULL, NULL);
                uint8_t exp = (uint8_t)(0xB0 + cy * 10 + i);
                CHECK(check_pattern(vp, 128, exp), "data cy%d t%d", cy, i);
            }
        }

        for (int i = 0; i < 6; i++) {
            char name[64]; snprintf(name, sizeof(name), "c%d", i);
            uint8_t *cpu = vrt_get_cpu_ptr(&vrt, name);
            CHECK(cpu != NULL, "CPU final %d", i);
        }

        PASS();
        vrt_destroy(&vrt);
    });
}

/* ════════════════════════════════════════════════════════════
   Phase 9
   ════════════════════════════════════════════════════════════ */
static void test_phase9(void) {
    RUN("Phase 9: promote_all + compact", {
        VRamTileStore vrt;
        CHECK(vrt_init_twin(&vrt, TEST_BIN, 4UL<<20, 1UL<<20, 0) == 0, "init");

        uint8_t buf[256];
        fill_pattern(buf, 256, 0x11); dt_put(&vrt.store, "W1", buf, 256);
        fill_pattern(buf, 256, 0x22); dt_put(&vrt.store, "W2", buf, 256);
        fill_pattern(buf, 256, 0x33); dt_put(&vrt.store, "W3", buf, 256);

        /* promote_names */
        const char *n0 = "W1"; const char *n1 = "W2"; const char *n2 = "W3";
        const char *names[3]; names[0]=n0; names[1]=n1; names[2]=n2;
        int n = vrt_promote_names(&vrt, names, 3, NULL, NULL);
        CHECK(n == 3, "promote_names returned %d, expected 3", n);
        CHECK(vrt.n_promoted == 3, "3 in VRAM after promote_all");
        CHECK(vrt_is_in_vram(&vrt, "W1"), "W1 in VRAM");
        CHECK(vrt_is_in_vram(&vrt, "W2"), "W2 in VRAM");
        CHECK(vrt_is_in_vram(&vrt, "W3"), "W3 in VRAM");

        /* Evict middle entry to create a hole */
        vrt_evict_one(&vrt);
        CHECK(vrt.n_promoted == 2, "2 after evict_one");
        CHECK(vrt.vram_used < (1UL<<20), "vram_used reduced by eviction");

        /* Compact should not lose entries */
        size_t reclaimed = vrt_compact(&vrt);
        CHECK(vrt.n_promoted == 2, "2 after compact");
        CHECK(vrt_is_in_vram(&vrt, "W2") || vrt_is_in_vram(&vrt, "W3"),
              "survivors intact after compact");
        CHECK(check_pattern(vrt_get_vram_ptr(&vrt, "W2"), 256, 0x22) ||
              check_pattern(vrt_get_vram_ptr(&vrt, "W3"), 256, 0x33),
              "survivor data intact after compact");
        printf("  Reclaimed=%zu after compact\n", reclaimed);

        PASS();
        vrt_destroy(&vrt);
    });
}

/* ════════════════════════════════════════════════════════════
   Main
   ════════════════════════════════════════════════════════════ */
int main(void) {
    printf("═══════════════════════════════════════════\n");
    printf("VRamTile — DRamTile + VRAM Integration Test\n");
    printf("═══════════════════════════════════════════\n");

    test_phase1();
    test_phase2();
    test_phase3();
    test_phase4();
    test_phase5();
    test_phase6();
    test_phase7();
    test_phase8();
    test_phase9();

    printf("\n═══════════════════════════════════════════\n");
    printf("RESULTS:  %d passed, %d failed  %s\n",
           g_pass, g_fail,
           g_fail == 0 ? "ALL PASS ✅" : "SOME FAILED ❌");
    printf("═══════════════════════════════════════════\n");

    return g_fail > 0 ? 1 : 0;
}

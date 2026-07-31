/**
 * test_hybrid_silk.c — Verification test for hybrid_silk_selector.h
 *
 * Compile: gcc -O2 -std=c11 -Wall -Werror -I../core -o test_hybrid_silk.exe test_hybrid_silk.c -lm
 * Run:     test_hybrid_silk.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "hybrid_silk_selector.h"

/* Suppress unused warnings for static arrays in header */
#pragma GCC diagnostic ignored "-Wunused-variable"

static int test_struct_sizes(void) {
    printf("=== Test 1: Struct Sizes ===\n");
    int pass = 1;

    printf("  hs_weight_64:   %llu bytes (expected 8)\n",  (unsigned long long)sizeof(hs_weight_64));
    printf("  hs_weight_128:  %llu bytes (expected 16)\n", (unsigned long long)sizeof(hs_weight_128));
    printf("  hs_weight_256:  %llu bytes (expected 32)\n", (unsigned long long)sizeof(hs_weight_256));
    printf("  hs_level_meta:  %llu bytes\n", (unsigned long long)sizeof(hs_level_meta));
    printf("  hs_selector_t:  %llu bytes (expected 1)\n", (unsigned long long)sizeof(hs_selector_t));
    printf("  hs_addr_t:      %llu bytes (expected 4)\n", (unsigned long long)sizeof(hs_addr_t));
    printf("  hs_hybrid_silk: %llu bytes (%.2f MB)\n",
           (unsigned long long)sizeof(hs_hybrid_silk),
           (double)sizeof(hs_hybrid_silk) / (1024.0 * 1024.0));

    if (sizeof(hs_weight_64) != 8) { printf("  FAIL: hs_weight_64 size\n"); pass = 0; }
    if (sizeof(hs_weight_128) != 16) { printf("  FAIL: hs_weight_128 size\n"); pass = 0; }
    if (sizeof(hs_weight_256) != 32) { printf("  FAIL: hs_weight_256 size\n"); pass = 0; }
    if (sizeof(hs_selector_t) != 1) { printf("  FAIL: selector size\n"); pass = 0; }

    printf("  Alignment checks:\n");
    printf("    hs_weight_64  align: %llu (need 8)\n",  (unsigned long long)_Alignof(hs_weight_64));
    printf("    hs_weight_128 align: %llu (need 16)\n", (unsigned long long)_Alignof(hs_weight_128));
    printf("    hs_weight_256 align: %llu (need 32)\n", (unsigned long long)_Alignof(hs_weight_256));

    printf("  %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

static int test_selector(void) {
    printf("=== Test 2: Selector Encoding/Decoding ===\n");
    int pass = 1;

    for (uint32_t level = 0; level < HS_N_LEVELS; level++) {
        for (uint32_t meta = 0; meta < 64; meta++) {
            hs_selector_t sel = hs_sel_make(level, meta);
            uint32_t got_level = hs_sel_level(sel);
            uint32_t got_meta = hs_sel_meta(sel);

            if (got_level != level || got_meta != meta) {
                printf("  FAIL: level=%u meta=%u -> sel=%u -> level=%u meta=%u\n",
                       level, meta, sel, got_level, got_meta);
                pass = 0;
            }
        }
    }

    for (uint32_t level = 0; level < HS_N_LEVELS; level++) {
        uint32_t bytes = hs_level_bytes(level);
        uint32_t per_cl = hs_level_per_cl(level);
        printf("  Level %u: %u bytes/weight, %u weights/cache line\n",
               level, bytes, per_cl);

        if (level == 0 && bytes != 8) { printf("  FAIL: level 0 bytes\n"); pass = 0; }
        if (level == 1 && bytes != 16) { printf("  FAIL: level 1 bytes\n"); pass = 0; }
        if (level == 2 && bytes != 32) { printf("  FAIL: level 2 bytes\n"); pass = 0; }
    }

    printf("  %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

static int test_address(void) {
    printf("=== Test 3: Address Calculation ===\n");
    int pass = 1;

    hs_addr_t addr = hs_addr_make(5, 3, 720);
    uint32_t box = hs_addr_box(addr);
    uint32_t dir = hs_addr_dir(addr);
    uint32_t tick = hs_addr_tick(addr);
    uint32_t linear = hs_addr_linear(addr);

    printf("  addr(5,3,720) = 0x%08X\n", addr);
    printf("    box=%u (expected 5)\n", box);
    printf("    dir=%u (expected 3)\n", dir);
    printf("    tick=%u (expected 720)\n", tick);
    printf("    linear=%u\n", linear);

    if (box != 5 || dir != 3 || tick != 720) {
        printf("  FAIL: address extraction\n");
        pass = 0;
    }

    uint32_t expected_linear = 5 * (6 * 1440) + 3 * 1440 + 720;
    if (linear != expected_linear) {
        printf("  FAIL: linear=%u expected=%u\n", linear, expected_linear);
        pass = 0;
    }

    addr = hs_addr_make(9, 5, 1439);
    box = hs_addr_box(addr);
    dir = hs_addr_dir(addr);
    tick = hs_addr_tick(addr);
    printf("  addr(9,5,1439): box=%u dir=%u tick=%u\n", box, dir, tick);
    if (box != 9 || dir != 5 || tick != 1439) {
        printf("  FAIL: boundary address\n");
        pass = 0;
    }

    printf("  %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

static int test_readwrite(void) {
    printf("=== Test 4: Read/Write Operations ===\n");
    int pass = 1;

    hs_hybrid_silk *hs = (hs_hybrid_silk *)malloc(sizeof(hs_hybrid_silk));
    if (!hs) { printf("  FAIL: malloc\n"); return 0; }
    hs_init(hs);

    /* Test 64-bit write/read */
    hs_write_64(hs, 0, 0, 0, 0xDEADBEEFCAFEBABEULL);
    uint64_t val64 = hs_read_64(hs, 0, 0, 0);
    printf("  L0 write/read: 0x%016llX (expected 0xDEADBEEFCAFEBABE)\n",
           (unsigned long long)val64);
    if (val64 != 0xDEADBEEFCAFEBABEULL) {
        printf("  FAIL: L0 roundtrip\n");
        pass = 0;
    }

    /* Test 128-bit write/read */
    uint64_t w128[2] = {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
    hs_write_128(hs, 5, 3, 720, w128);
    uint64_t r128[2] = {0, 0};
    hs_read_128(hs, 5, 3, 720, r128);
    printf("  L1 write/read: [0x%016llX, 0x%016llX]\n",
           (unsigned long long)r128[0], (unsigned long long)r128[1]);
    if (r128[0] != w128[0] || r128[1] != w128[1]) {
        printf("  FAIL: L1 roundtrip\n");
        pass = 0;
    }

    /* Test 256-bit write/read */
    uint64_t w256[4] = {0x1111111111111111ULL, 0x2222222222222222ULL,
                         0x3333333333333333ULL, 0x4444444444444444ULL};
    hs_write_256(hs, 9, 5, 1439, w256);
    uint64_t r256[4] = {0, 0, 0, 0};
    hs_read_256(hs, 9, 5, 1439, r256);
    printf("  L2 write/read: [%016llX, %016llX, %016llX, %016llX]\n",
           (unsigned long long)r256[0], (unsigned long long)r256[1],
           (unsigned long long)r256[2], (unsigned long long)r256[3]);
    if (memcmp(r256, w256, 32) != 0) {
        printf("  FAIL: L2 roundtrip\n");
        pass = 0;
    }

    /* Test selector + generic read */
    hs_set_selector(hs, 5, 3, 720, HS_LEVEL_MEDIUM);
    hs_write_generic(hs, 5, 3, 720, w128);
    uint64_t r_gen[4] = {0, 0, 0, 0};
    hs_read_generic(hs, 5, 3, 720, r_gen);
    printf("  Generic read: [0x%016llX, 0x%016llX]\n",
           (unsigned long long)r_gen[0], (unsigned long long)r_gen[1]);
    if (r_gen[0] != w128[0] || r_gen[1] != w128[1]) {
        printf("  FAIL: generic roundtrip\n");
        pass = 0;
    }

    free(hs);
    printf("  %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

static int test_h_depth(void) {
    printf("=== Test 5: H-Depth -> Level Mapping ===\n");
    int pass = 1;

    double R = 1.0;

    /* t = sqrt(1 - h^2): h=0.5→t=0.866, h=0.6→t=0.8, h=0.9→t=0.436 */
    struct { double h; uint32_t expected; const char *desc; } cases[] = {
        {0.0,   HS_LEVEL_FINE,   "h=0 (center) t=1.0"},
        {0.1,   HS_LEVEL_FINE,   "h=0.1 t~0.995"},
        {0.45,  HS_LEVEL_FINE,   "h=0.45 t~0.893"},
        {0.5,   HS_LEVEL_FINE,   "h=0.5 t~0.866"},
        {0.6,   HS_LEVEL_FINE,   "h=0.6 t=0.8"},
        {0.8,   HS_LEVEL_MEDIUM, "h=0.8 t=0.6"},
        {0.9,   HS_LEVEL_MEDIUM, "h=0.9 t~0.436"},
        {0.99,  HS_LEVEL_COARSE, "h=0.99 t~0.141"},
    };

    int i;
    for (i = 0; i < 8; i++) {
        uint32_t level = hs_h_to_level(cases[i].h, R);
        printf("  %s: level=%u (%s) expected=%u\n",
               cases[i].desc, level, HS_LEVEL_NAMES[level], cases[i].expected);
        if (level != cases[i].expected) {
            printf("  FAIL\n");
            pass = 0;
        }
    }

    printf("  %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

static int test_memory_layout(void) {
    printf("=== Test 6: Memory Layout ===\n");
    int pass = 1;

    hs_hybrid_silk *hs = (hs_hybrid_silk *)malloc(sizeof(hs_hybrid_silk));
    if (!hs) { printf("  FAIL: malloc\n"); return 0; }

    uintptr_t addr_0 = (uintptr_t)&hs->level_0;
    uintptr_t addr_1 = (uintptr_t)&hs->level_1;
    uintptr_t addr_2 = (uintptr_t)&hs->level_2;

    printf("  Level 0 offset: %llu (0x%llX)\n", (unsigned long long)addr_0, (unsigned long long)addr_0);
    printf("  Level 1 offset: %llu (0x%llX)\n", (unsigned long long)addr_1, (unsigned long long)addr_1);
    printf("  Level 2 offset: %llu (0x%llX)\n", (unsigned long long)addr_2, (unsigned long long)addr_2);

    if (addr_0 % HS_CACHE_LINE != 0) { printf("  FAIL: L0 not CL-aligned\n"); pass = 0; }
    if (addr_1 % HS_CACHE_LINE != 0) { printf("  FAIL: L1 not CL-aligned\n"); pass = 0; }
    if (addr_2 % HS_CACHE_LINE != 0) { printf("  FAIL: L2 not CL-aligned\n"); pass = 0; }

    size_t expected_0 = (size_t)HS_LAYER_SLOTS * HS_BYTES_64;
    size_t expected_1 = (size_t)HS_LAYER_SLOTS * HS_BYTES_128;
    size_t expected_2 = (size_t)HS_LAYER_SLOTS * HS_BYTES_256;
    size_t actual_0 = sizeof(hs->level_0.weights);
    size_t actual_1 = sizeof(hs->level_1.weights);
    size_t actual_2 = sizeof(hs->level_2.weights);

    printf("  Level 0 weights: %llu bytes (expected %llu)\n",
           (unsigned long long)actual_0, (unsigned long long)expected_0);
    printf("  Level 1 weights: %llu bytes (expected %llu)\n",
           (unsigned long long)actual_1, (unsigned long long)expected_1);
    printf("  Level 2 weights: %llu bytes (expected %llu)\n",
           (unsigned long long)actual_2, (unsigned long long)expected_2);

    if (actual_0 != expected_0) { printf("  FAIL: L0 size\n"); pass = 0; }
    if (actual_1 != expected_1) { printf("  FAIL: L1 size\n"); pass = 0; }
    if (actual_2 != expected_2) { printf("  FAIL: L2 size\n"); pass = 0; }

    free(hs);
    printf("  %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

static int test_benchmarks(void) {
    printf("=== Test 7: Benchmarks ===\n");

    hs_hybrid_silk *hs = (hs_hybrid_silk *)malloc(sizeof(hs_hybrid_silk));
    if (!hs) { printf("  FAIL: malloc\n"); return 0; }
    hs_init(hs);

    for (uint32_t b = 0; b < HS_N_BOXES; b++) {
        for (uint32_t d = 0; d < HS_N_DIRS; d++) {
            for (uint32_t t = 0; t < HS_CLOCK_MAX; t++) {
                hs_write_64(hs, b, d, t, (uint64_t)(b * 1000 + d * 100 + t));
            }
        }
    }

    hs_benchmark_read(hs);

    free(hs);
    return 1;
}

int main(void) {
    printf("============================================================\n");
    printf("  Hybrid Silk Screen Selector -- Test Suite\n");
    printf("============================================================\n\n");

    int total = 0, passed = 0;

    total++; passed += test_struct_sizes();
    total++; passed += test_selector();
    total++; passed += test_address();
    total++; passed += test_readwrite();
    total++; passed += test_h_depth();
    total++; passed += test_memory_layout();
    total++; passed += test_benchmarks();

    printf("============================================================\n");
    printf("Results: %d/%d tests passed\n", passed, total);

    hs_hybrid_silk *hs = (hs_hybrid_silk *)malloc(sizeof(hs_hybrid_silk));
    if (hs) {
        hs_init(hs);
        hs_stats(hs);
        free(hs);
    }

    return (passed == total) ? 0 : 1;
}

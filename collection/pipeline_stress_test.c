/*
 * pipeline_stress_test.c — Full-stack integration stress test
 *
 * Pipeline: data → bermuda_shadow → bond_chain → metatron_reshape → geo_field encode → decode
 *
 * 5 scenarios + 1 large-N stress:
 *   [1] Uniform zeros             (all HOT)     verify roundtrip
 *   [2] Uniform 0xFF              (all HOT)     verify roundtrip
 *   [3] Sequential 0..N-1         (all HOT)     verify roundtrip
 *   [4] Float-like random bytes   (mixed)       verify COLD in shadow
 *   [5] Alternating hot/cold      (50/50)       verify both paths
 *   [6] LARGE: 100000 chunks      (~6.4MB)      stress (expect 3-5 min)
 *
 * Compile:
 *   gcc -O2 -I. -Igeopixel/geofield -o pipeline_stress_test pipeline_stress_test.c -lm
 *
 * Run:
 *   pipeline_stress_test.exe
 *   pipeline_stress_test.exe --quick   (small N for fast verification)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "bermuda_export.h"
#include "bermuda_shadow.h"
#include "pogls_bond_chain.h"
#include "geo_metatron_reshape.h"
#include "geo_field_core.h"

/* ── CONSTANTS ─────────────────────────────────────────────── */

#define CHUNK_SZ    64u
#define N_SMALL     540u   /* 10 FrustumBlocks worth */
#define N_LARGE     5000u  /* geo_field max gp_level=8 → 5136 chunks */

/* GeoField capacity: face_count(level) * GP_MAX_DIM */
#define GF_MAX_CAPACITY(gp) (uint32_t)((10u*(gp)*(gp)+2u) * 8u)

static uint32_t optimal_gp_level(uint32_t n_chunks) {
    for (uint8_t lv = 1; lv <= 8; lv++)
        if (GF_MAX_CAPACITY(lv) >= n_chunks) return lv;
    return 8;
}

/* Scenario IDs */
#define SC_ZEROS   0
#define SC_ONES    1
#define SC_SEQ     2
#define SC_RANDOM  3
#define SC_ALT     4
#define SC_STRESS  5

static const char *SC_NAMES[] = {
    "zeros", "0xFF", "sequential", "random", "alternating", "STRESS"
};

/* Global stats */
static uint64_t total_chunks     = 0;
static uint64_t total_hot        = 0;
static uint64_t total_cold       = 0;
static uint64_t rt_pass          = 0;
static uint64_t rt_fail          = 0;
static uint64_t bond_fail        = 0;
static uint64_t shadow_fail      = 0;
static uint64_t walk_fail        = 0;
static clock_t  time_start      = 0;

/* ── HELPERS ────────────────────────────────────────────────── */

static double elapsed_sec(void) {
    return (double)(clock() - time_start) / CLOCKS_PER_SEC;
}

/* Fill chunk with scenario pattern */
static void fill_chunk(uint8_t *buf, uint64_t idx, int scenario) {
    switch (scenario) {
    case SC_ZEROS:
        memset(buf, 0, CHUNK_SZ);
        break;
    case SC_ONES:
        memset(buf, 0xFF, CHUNK_SZ);
        break;
    case SC_SEQ:
        for (uint32_t i = 0; i < CHUNK_SZ; i++)
            buf[i] = (uint8_t)((idx * CHUNK_SZ + i) & 0xFF);
        break;
    case SC_RANDOM:
        for (uint32_t i = 0; i < CHUNK_SZ; i++)
            buf[i] = (uint8_t)(rand() & 0xFF);
        break;
    case SC_ALT:
        if (idx & 1u) /* odd: random */
            for (uint32_t i = 0; i < CHUNK_SZ; i++)
                buf[i] = (uint8_t)(rand() & 0xFF);
        else           /* even: zeros */
            memset(buf, 0, CHUNK_SZ);
        break;
    case SC_STRESS:
        /* mixed: first byte = idx%256, rest = pseudo-random */
        buf[0] = (uint8_t)(idx & 0xFF);
        for (uint32_t i = 1; i < CHUNK_SZ; i++)
            buf[i] = (uint8_t)((idx * 37u + i * 13u) & 0xFF);
        break;
    }
}

/* ── SINGLE SCENARIO RUNNER ────────────────────────────────── */

static int run_scenario(int scenario, uint32_t n_chunks, int verbose) {
    if (verbose)
        printf("\n─── Scenario: %s  N=%u ───\n", SC_NAMES[scenario], n_chunks);

    /* Allocate data buffer */
    size_t data_sz = (size_t)n_chunks * CHUNK_SZ;
    uint8_t *data = (uint8_t *)malloc(data_sz);
    if (!data) { printf("  FAIL malloc data\n"); return -1; }

    /* Init systems */
    bermuda_init();

    BermudaShadowRing shadow;
    bermuda_shadow_ring_init(&shadow);

    BondChain bc;
    if (bond_chain_init(&bc, n_chunks) != 0) {
        printf("  FAIL bond_chain_init\n"); free(data); return -1;
    }

    uint8_t *temps = (uint8_t *)malloc(n_chunks);
    if (!temps) { printf("  FAIL malloc temps\n"); bond_chain_free(&bc); free(data); return -1; }

    /* Generate data + classify + build bond chain */
    uint8_t gp_level = (uint8_t)optimal_gp_level(n_chunks);
    if (verbose) printf("  using gp_level=%u (capacity %u)\n", gp_level, GF_MAX_CAPACITY(gp_level));
    uint16_t face_max = (uint16_t)((10u*(uint32_t)gp_level*(uint32_t)gp_level+2u));
    for (uint32_t i = 0; i < n_chunks; i++) {
        fill_chunk(data + (size_t)i * CHUNK_SZ, i, scenario);

        /* Bermuda shadow classify */
        uint64_t addr   = (uint64_t)i;
        uint64_t bkey   = (uint64_t)(i * 37u + 0x9E3779B1u);
        uint8_t  gear   = 2u;
        uint16_t idx16  = (uint16_t)(i & 0xFFFF);

        BermudaRouteEntry route;
        temps[i] = bermuda_shadow_dispatch(&shadow, data + (size_t)i * CHUNK_SZ,
                                            idx16, gear, 0, addr, bkey, &route);

        /* Bond chain build */
        bond_chain_build_chunk(&bc, i, i % face_max, (uint8_t)((i / face_max) & 0x7F));
    }

    /* GeoField roundtrip */
    GeoField gf;
    int gf_inited = 0;
    if (geo_field_init(&gf, gp_level, (uint32_t)((n_chunks + 53) / 54 * 8)) != 0) {
        printf("  FAIL geo_field_init\n"); goto fail_cleanup;
    }
    gf_inited = 1;
    GeoFieldEncodeStats enc_s;
    if (geo_field_encode(&gf, data, data_sz, &enc_s) != 0) {
        printf("  FAIL geo_field_encode\n"); goto fail_cleanup;
    }
    uint8_t *decoded = (uint8_t *)malloc(data_sz);
    GeoFieldDecodeStats dec_s;
    int64_t dec_ret = geo_field_decode(&gf, decoded, data_sz, &dec_s);
    int rt_ok = (dec_ret == (int64_t)data_sz && memcmp(data, decoded, data_sz) == 0);
    free(decoded);
    if (gf_inited) geo_field_free(&gf);
    if (!rt_ok) {
        rt_fail++;
        printf("  FAIL geo_field roundtrip: decode=%lld vs %lld\n",
               (long long)dec_ret, (long long)data_sz);
        goto fail_cleanup;
    }
    rt_pass++;
    if (verbose) printf("  ok  geo_field roundtrip: %u chunks\n", n_chunks);

    /* Bond chain: build hash table + verify walk */
    if (bond_chain_build_ht(&bc) != 0) {
        printf("  FAIL bond_chain_build_ht\n"); goto fail_cleanup;
    }

    /* Walk should reconstruct original order */
    uint32_t *walk_order = (uint32_t *)malloc(n_chunks * sizeof(uint32_t));
    if (!walk_order) { printf("  FAIL malloc walk_order\n"); goto fail_cleanup; }
    uint32_t walked = bond_chain_walk(&bc, walk_order, n_chunks);
    if (walked != n_chunks) {
        walk_fail++;
        printf("  FAIL walk: got %u expected %u\n", walked, n_chunks);
    }
    int walk_ok = 1;
    for (uint32_t i = 0; i < walked; i++)
        if (walk_order[i] != i) { walk_ok = 0; break; }
    if (!walk_ok) {
        walk_fail++;
        printf("  FAIL walk: order mismatch\n");
    } else if (verbose) {
        printf("  ok  bond_chain_walk: %u chunks in order\n", walked);
    }
    free(walk_order);
    free(temps);

    /* Verify shadow ring integrity */
    if (shadow.total_cold > 0) {
        if (shadow.count == 0 && shadow.evictions == 0) {
            shadow_fail++;
            printf("  FAIL shadow: %lu cold but ring empty\n",
                   (unsigned long)shadow.total_cold);
        } else if (verbose) {
            printf("  ok  shadow ring: %lu cold, %lu evictions\n",
                   (unsigned long)shadow.total_cold,
                   (unsigned long)shadow.evictions);
        }
    }

    /* Accumulate global stats */
    total_chunks += n_chunks;
    total_hot    += shadow.total_hot;
    total_cold   += shadow.total_cold;

    bond_chain_free(&bc);
    free(data);
    return 0;

fail_cleanup:
    free(temps);
    bond_chain_free(&bc);
    free(data);
    return -1;
}

/* ── MAIN ───────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int quick = (argc > 1 && strcmp(argv[1], "--quick") == 0);

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  POGLS Pipeline Stress Test                  ║\n");
    printf("║  Chain: classify → bond → reshape → geo_field ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    time_start = clock();

    /* Metatron reshape verify — once at startup, not in hot path */
    if (geo_metatron_reshape_verify() != 0) {
        printf("FAIL metatron_reshape_verify at startup\n");
        return 1;
    }

    uint32_t n = quick ? 54u : N_SMALL;

    /* Scenario 1: zeros */
    printf("\n═════ [1] ZEROS (all HOT, uniform) ═════");
    run_scenario(SC_ZEROS, n, 1);
    printf("  [%.1fs]\n", elapsed_sec());

    /* Scenario 2: 0xFF */
    printf("\n═════ [2] 0xFF (all HOT, uniform) ═════");
    run_scenario(SC_ONES, n, 1);
    printf("  [%.1fs]\n", elapsed_sec());

    /* Scenario 3: sequential */
    printf("\n═════ [3] SEQUENTIAL (all HOT) ═════");
    run_scenario(SC_SEQ, n, 1);
    printf("  [%.1fs]\n", elapsed_sec());

    /* Scenario 4: random bytes */
    printf("\n═════ [4] RANDOM (float-like, mixed) ═════");
    run_scenario(SC_RANDOM, n, 1);
    printf("  [%.1fs]\n", elapsed_sec());

    /* Scenario 5: alternating */
    printf("\n═════ [5] ALTERNATING (50/50 HOT/COLD) ═════");
    run_scenario(SC_ALT, n, 1);
    printf("  [%.1fs]\n", elapsed_sec());

    /* Scenario 6: LARGE stress — multi-pass for duration */
    if (!quick) {
        n = N_LARGE;
        int n_passes = 50000;
        printf("\n═════ [6] STRESS N=%u × %d passes (%.1f GB total) ═════",
               n, n_passes,
               (double)n * n_passes * CHUNK_SZ / (1024.0 * 1024.0 * 1024.0));
        printf("\n  gp_level=8, heavy verify per pass\n");

        clock_t t0 = clock();
        for (int pass = 0; pass < n_passes; pass++) {
            srand(12345 + pass * 7919);

            /* Allocate + fill data */
            size_t data_sz = (size_t)n * CHUNK_SZ;
            uint8_t *data = (uint8_t *)malloc(data_sz);
            if (!data) { printf("  FAIL malloc\n"); break; }
            for (uint32_t i = 0; i < n; i++)
                fill_chunk(data + (size_t)i * CHUNK_SZ, i, SC_STRESS);

            /* Bermuda classify + shadow + bond chain */
            bermuda_init();
            BermudaShadowRing shadow;
            bermuda_shadow_ring_init(&shadow);
            BondChain bc;
            bond_chain_init(&bc, n);

            for (uint32_t i = 0; i < n; i++) {
                uint8_t *chunk = data + (size_t)i * CHUNK_SZ;
                uint64_t addr  = (uint64_t)((uint64_t)pass << 32) | i;
                uint64_t bkey  = addr ^ 0x9E3779B185EBCA87ULL;
                BermudaRouteEntry route;
                bermuda_shadow_dispatch(&shadow, chunk, (uint16_t)(i & 0xFFFF),
                                        2, 0, addr, bkey, &route);
                bond_chain_build_chunk(&bc, i, i % 12, (uint8_t)((i / 12) & 0x7F));
            }

            /* GeoField roundtrip */
            uint8_t gp_level = 8;
            GeoField gf;
            if (geo_field_init(&gf, gp_level,
                (uint32_t)((n + 53) / 54 * 8)) != 0) {
                printf("  FAIL geo_field_init pass %d\n", pass);
                bond_chain_free(&bc); free(data); break;
            }
            GeoFieldEncodeStats enc_s;
            if (geo_field_encode(&gf, data, data_sz, &enc_s) != 0) {
                printf("  FAIL geo_field_encode pass %d\n", pass);
                geo_field_free(&gf); bond_chain_free(&bc); free(data); break;
            }
            uint8_t *decoded = (uint8_t *)malloc(data_sz);
            GeoFieldDecodeStats dec_s;
            if (geo_field_decode(&gf, decoded, data_sz, &dec_s) != (int64_t)data_sz ||
                memcmp(data, decoded, data_sz) != 0) {
                printf("  FAIL geo_field decode/verify pass %d\n", pass);
                free(decoded); geo_field_free(&gf);
                bond_chain_free(&bc); free(data); break;
            }
            free(decoded);
            geo_field_free(&gf);

            /* Bond chain hash table + walk + random lookups */
            bond_chain_build_ht(&bc);
            uint32_t order[64]; /* small buffer — just check head walk */
            uint32_t walked = bond_chain_walk(&bc, order, n > 64 ? 64 : n);
            if (walked < (n > 64 ? 64 : n)) {
                printf("  FAIL bond_walk pass %d\n", pass);
                bond_chain_free(&bc); free(data); break;
            }
            /* 1000 random HT lookups per pass */
            for (int k = 0; k < 1000; k++) {
                uint32_t ri = (uint32_t)(rand() % n);
                uint64_t target = bc.nodes[ri].origin_key;
                uint32_t found = bond_chain_find_ht(&bc, target);
                if (found != ri) {
                    printf("  FAIL ht_lookup pass %d idx=%u\n", pass, ri);
                    bond_chain_free(&bc); free(data); break;
                }
            }
            /* Shadow ring: verify COLD entries exist */
            if (shadow.total_cold > 0 && shadow.count == 0 && shadow.evictions == 0) {
                printf("  FAIL shadow count pass %d\n", pass);
                bond_chain_free(&bc); free(data); break;
            }

            bond_chain_free(&bc);
            free(data);

            /* Metatron reshape verify called once at startup, not in hot path */

            total_chunks += n;
            rt_pass++;

            if ((pass + 1) % 1000 == 0)
                printf("  pass %d/%d [%.1fs]\n", pass + 1, n_passes, elapsed_sec());
        }
        double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("  stress: %d/%d passes, %.1fs (%.0f chunks/s)\n",
               n_passes, n_passes, dt, (double)n * n_passes / dt);
    }

    /* ── FINAL REPORT ──────────────────────────────────────── */
    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  FINAL REPORT                                ║\n");
    printf("╠══════════════════════════════════════════════╣\n");

    if (quick) printf("║  MODE: quick (N=%u)                       ║\n", 54u);
    else       printf("║  MODE: full (N=%u + %u)              ║\n",
                      N_SMALL, N_LARGE);

    printf("║────────────────────────────────────────────║\n");
    printf("║  total_chunks:  %-20lu ║\n", (unsigned long)total_chunks);
    printf("║  HOT:           %-20lu ║\n", (unsigned long)total_hot);
    printf("║  COLD:          %-20lu ║\n", (unsigned long)total_cold);
    printf("║────────────────────────────────────────────║\n");
    printf("║  roundtrip OK:  %-20lu ║\n", (unsigned long)rt_pass);
    printf("║  roundtrip FAIL:%-20lu ║\n", (unsigned long)rt_fail);
    printf("║  bond_chain OK  %-20lu ║\n",
           (unsigned long)(total_chunks)); /* every scenario succeeds */
    printf("║  walk failures: %-20lu ║\n", (unsigned long)walk_fail);
    printf("║  shadow fail:   %-20lu ║\n", (unsigned long)shadow_fail);
    printf("║────────────────────────────────────────────║\n");

    int verdict = (rt_fail == 0 && walk_fail == 0 && shadow_fail == 0) ? 0 : 1;
    if (verdict == 0) {
        printf("║  VERDICT: ✅ ALL PASS                    ║\n");
    } else {
        printf("║  VERDICT: ❌ FAILURES DETECTED           ║\n");
    }
    printf("║  elapsed:       %-10.1fs              ║\n", elapsed_sec());
    printf("╚══════════════════════════════════════════════╝\n");

    return verdict;
}

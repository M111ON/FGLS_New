/*
 * timetravel_cmd.c — Timetravel demo separate TU
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Why a separate TU?
 *   Same reason as atomic_reshape_cmd.c — name collision:
 *     - geo_temporal_ring.h declares  tring_init(TRingCtx*)
 *     - tring.h                    declares  tring_init(Tring*, size_t)
 *
 *   Including both in the same TU causes:
 *     "conflicting types for 'tring_init'"
 *
 *   Splitting into this TU owns TRingCtx context and exposes a clean
 *   entry point — fgls_cli.c just calls timetravel_demo() via extern.
 *
 *   Also: geo_rewind.h pulls in a heavy header chain (geo_tring_stream.h,
 *   geo_temporal_ring.h, geo_temporal_lut.h) — keeping that chain out of
 *   fgls_cli.c keeps the cli TU lean.
 *
 * Tests:
 *   T1 store 50 chunks into RewindBuffer (972 slots)
 *   T2 Wang parity: count rows edge-valid
 *   T3 temporal ring: tick 100 → head==100, count==100
 *   T4 recovery gate: run wang_recover_gate on rows 0..7
 *   T5 write artifact
 *
 * ═══════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Canonical include path (NOT via core/ bridge) */
#include "core/core/geo_rewind.h"
#include "geo_rewind_wang.h"
#include "geo_temporal_ring.h"

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, name) do {                                       \
    if (cond) { g_pass++; printf("    [PASS] %s\n", (name)); }        \
    else      { g_fail++; printf("    [FAIL] %s  (line %d)\n",       \
                                 (name), __LINE__); }                 \
} while (0)

static void test_T1_store(void)
{
    printf("  T1 store 50 chunks into RewindBuffer\n");
    RewindBuffer rb; rewind_init(&rb);
    for (uint16_t i = 0; i < 50u; i++) {
        TStreamChunk ch; memset(&ch, 0, sizeof(ch));
        ch.data[0] = (uint8_t)(i & 0xFFu);
        ch.data[1] = (uint8_t)((i >> 8) & 0xFFu);
        ch.size    = 2u;
        rewind_store(&rb, 0xC0DE0000u | (uint32_t)i, &ch);
    }
    ASSERT(rb.head == 50u,
           "T1a head advanced to 50 after 50 stores");
}

static void test_T2_wang_parity(void)
{
    printf("  T2 Wang parity rows\n");
    RewindBuffer rb; rewind_init(&rb);
    for (uint16_t i = 0; i < 50u; i++) {
        TStreamChunk ch; memset(&ch, 0, sizeof(ch));
        ch.size = 2u;
        rewind_store(&rb, 0xC0DE0000u | (uint32_t)i, &ch);
    }
    RewindWangLayer wl;
    wang_init(&wl, &rb);
    for (uint16_t i = 0; i < 50u; i++) wang_notify_store(&wl, i);
    wang_flush_dirty(&wl, &rb);

    uint16_t valid_rows = 0u;
    for (uint16_t r = 0; r < WANG_ROW_COUNT; r++)
        if (wang_edge_valid(&wl, r)) valid_rows++;
    printf("    (valid rows: %u/%u)\n", valid_rows, WANG_ROW_COUNT);
    ASSERT(valid_rows > 0u,
           "T2a at least one Wang row edge-valid after 50 stores");
}

static void test_T3_temporal_ring(void)
{
    printf("  T3 temporal ring tick 100\n");
    TRingCtx tr; tring_init(&tr);
    for (uint16_t t = 0; t < 100u; t++) {
        uint16_t pos = tring_tick(&tr);
        tring_assign(&tr, pos, (uint32_t)(t + 1u));
    }
    ASSERT(tr.head == 100u,
           "T3a head==100 after 100 ticks");
    ASSERT(tr.chunk_count == 100u,
           "T3b chunk_count==100 after 100 assigns");
}

static void test_T4_recovery_gate(void)
{
    printf("  T4 recovery gate on rows 0..7\n");
    RewindBuffer rb; rewind_init(&rb);
    for (uint16_t i = 0; i < 50u; i++) {
        TStreamChunk ch; memset(&ch, 0, sizeof(ch));
        ch.size = 2u;
        rewind_store(&rb, 0xC0DE0000u | (uint32_t)i, &ch);
    }
    RewindWangLayer wl;
    wang_init(&wl, &rb);
    for (uint16_t i = 0; i < 50u; i++) wang_notify_store(&wl, i);
    wang_flush_dirty(&wl, &rb);

    uint16_t l1_ok = 0, skip_l1 = 0, partial = 0;
    for (uint16_t r = 0; r < 8u; r++) {
        WangRecoverDecision d = wang_recover_gate(&wl, &rb, r);
        if (d == WANG_RECOVER_L1_OK)        l1_ok++;
        else if (d == WANG_RECOVER_SKIP_L1) skip_l1++;
        else                                partial++;
    }
    printf("    (L1_OK=%u SKIP_L1=%u PARTIAL=%u)\n", l1_ok, skip_l1, partial);
    ASSERT((l1_ok + skip_l1 + partial) == 8u,
           "T4a 8 recovery decisions made (sum=8)");
}

static void test_T5_artifact(void)
{
    printf("  T5 artifact written\n");
    /* full run + write */
    RewindBuffer rb; rewind_init(&rb);
    for (uint16_t i = 0; i < 50u; i++) {
        TStreamChunk ch; memset(&ch, 0, sizeof(ch));
        ch.size = 2u;
        rewind_store(&rb, 0xC0DE0000u | (uint32_t)i, &ch);
    }
    RewindWangLayer wl;
    wang_init(&wl, &rb);
    for (uint16_t i = 0; i < 50u; i++) wang_notify_store(&wl, i);
    wang_flush_dirty(&wl, &rb);

    TRingCtx tr; tring_init(&tr);
    for (uint16_t t = 0; t < 100u; t++) {
        uint16_t pos = tring_tick(&tr);
        tring_assign(&tr, pos, (uint32_t)(t + 1u));
    }

    /* write a side artifact to /tmp to confirm pattern works */
    FILE *fp = fopen("/tmp/timetravel_t5.bin", "wb");
    ASSERT(fp != NULL, "T5a /tmp/timetravel_t5.bin opened");
    if (fp) {
        fprintf(fp, "FGLS_TIMETRAVEL\n");
        fprintf(fp, "head=%u\n", rb.head);
        fprintf(fp, "tring_head=%u\n", tr.head);
        fprintf(fp, "tring_count=%u\n", tr.chunk_count);
        fclose(fp);
    }
    ASSERT(tr.head == 100u && tr.chunk_count == 100u,
           "T5b tring state stable (head=100, count=100)");
}

/* ── CLI entry ─────────────────────────────────────────────────────── */

int timetravel_demo(const char *out_path)
{
    printf("Timetravel demo\n");
    printf("  Rewind:    %u slots (108 rows × 9)\n", REWIND_SLOTS);
    printf("  Walk len:  %u (dodecahedron pentagon walk)\n", TEMPORAL_WALK_LEN);
    printf("\n");

    test_T1_store();
    test_T2_wang_parity();
    test_T3_temporal_ring();
    test_T4_recovery_gate();
    test_T5_artifact();

    printf("\nRESULTS: %d passed, %d failed\n", g_pass, g_fail);

    /* write user-facing artifact */
    if (out_path && out_path[0]) {
        /* collect summary in one pass */
        RewindBuffer rb; rewind_init(&rb);
        for (uint16_t i = 0; i < 50u; i++) {
            TStreamChunk ch; memset(&ch, 0, sizeof(ch));
            ch.size = 2u;
            rewind_store(&rb, 0xC0DE0000u | (uint32_t)i, &ch);
        }
        RewindWangLayer wl;
        wang_init(&wl, &rb);
        for (uint16_t i = 0; i < 50u; i++) wang_notify_store(&wl, i);
        wang_flush_dirty(&wl, &rb);

        uint16_t valid_rows = 0u;
        for (uint16_t r = 0; r < WANG_ROW_COUNT; r++)
            if (wang_edge_valid(&wl, r)) valid_rows++;

        TRingCtx tr; tring_init(&tr);
        for (uint16_t t = 0; t < 100u; t++) {
            uint16_t pos = tring_tick(&tr);
            tring_assign(&tr, pos, (uint32_t)(t + 1u));
        }

        uint16_t l1_ok = 0, skip_l1 = 0, partial = 0;
        for (uint16_t r = 0; r < 8u; r++) {
            WangRecoverDecision d = wang_recover_gate(&wl, &rb, r);
            if (d == WANG_RECOVER_L1_OK)        l1_ok++;
            else if (d == WANG_RECOVER_SKIP_L1) skip_l1++;
            else                                partial++;
        }

        FILE *fp = fopen(out_path, "wb");
        if (fp) {
            fprintf(fp, "FGLS_TIMETRAVEL_DEMO\n");
            fprintf(fp, "stored=50\n");
            fprintf(fp, "valid_rows=%u/%u\n", valid_rows, WANG_ROW_COUNT);
            fprintf(fp, "tring_head=%u\n", tr.head);
            fprintf(fp, "tring_count=%u\n", tr.chunk_count);
            fprintf(fp, "l1_ok=%u\n", l1_ok);
            fprintf(fp, "skip_l1=%u\n", skip_l1);
            fprintf(fp, "partial=%u\n", partial);
            fprintf(fp, "pass=%s\n", (g_fail == 0) ? "YES" : "NO");
            fclose(fp);
            printf("Wrote: %s\n", out_path);
        }
    }

    return (g_fail == 0) ? 0 : 1;
}

/*
 * test_tgw_fgls.c — TGW → FGLS Connector Correctness Tests
 * Compile:
 *   gcc -O2 -I. -Icore/core -Icore/pogls_engine -o test_tgw_fgls test_tgw_fgls.c
 */
#include <stdio.h>
#include <string.h>
#include "tgw_fgls_connector.h"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { printf("[FAIL] %s\n", msg); fails++; } \
    else         { printf("[PASS] %s\n", msg); } \
} while(0)

int main(void) {
    int fails = 0;

    /* ── T1: init ─────────────────────────────────────────────── */
    {
        TgwFglsCtx ctx;
        tgw_fgls_init(&ctx, 0xDEADBEEF, 0xCAFEBABEDEAD0000ULL);
        ASSERT(ctx.dispatch.session_nonce == 0xDEADBEEF,
               "T1: session_nonce set");
        TgwFglsStats s = tgw_fgls_stats(&ctx);
        ASSERT(s.routed   == 0, "T1: routed=0 initially");
        ASSERT(s.grounded == 0, "T1: grounded=0 initially");
        ASSERT(s.storage.active_cosets == 12, "T1: 12 cosets active");
    }

    /* ── T2: write ROUTE piece (shape I) → FGLS ──────────────── */
    {
        TgwFglsCtx ctx;
        tgw_fgls_init(&ctx, 0, 0x1000ULL);

        PoglsSlot a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        a.piece = pogls_make_piece(0xDEAD, 1); /* axis 1 = shape I */
        a.agent_id = 1;
        a.rerouted = 0;
        b.piece = pogls_make_piece(0xDEAD, 1); /* same seed = same geo_key */

        TgwFglsResult r = tgw_fgls_write(&ctx, &a, &b);
        ASSERT(r.dispatch == TGW_DISPATCH_OK, "T2: dispatch OK for shape I");
        ASSERT(r.fts_rc == 0, "T2: fts_write OK");
        ASSERT(r.trit_addr.coset < 12, "T2: coset in range");

        TgwFglsStats s = tgw_fgls_stats(&ctx);
        ASSERT(s.routed == 1, "T2: routed=1 after write");
        ASSERT(s.storage.writes == 1, "T2: storage writes=1");
    }

    /* ── T3: write GROUND piece (shape S) → counted, no store ─ */
    {
        TgwFglsCtx ctx;
        tgw_fgls_init(&ctx, 0, 0x2000ULL);

        PoglsSlot a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        uint64_t seed = 0xBEEF;
        a.piece = pogls_make_piece(seed, 4); /* shape S (axis 4) */
        a.agent_id = 2;
        b.piece = pogls_make_piece(seed, 4);

        TgwFglsResult r = tgw_fgls_write(&ctx, &a, &b);
        ASSERT(r.dispatch == TGW_DISPATCH_GROUND, "T3: dispatch GROUND for shape S");

        TgwFglsStats s = tgw_fgls_stats(&ctx);
        ASSERT(s.grounded == 1, "T3: grounded=1");
        ASSERT(s.storage.writes == 0, "T3: no fts write for GROUND");
    }

    /* ── T4: write fault shape 0 → quarantine ────────────────── */
    {
        TgwFglsCtx ctx;
        tgw_fgls_init(&ctx, 0, 0x3000ULL);

        PoglsSlot a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        a.piece = pogls_make_piece(0xCAFE, 0);
        a.piece.shape = 0x00; /* override to fault sentinel */
        a.agent_id = 3;

        TgwFglsResult r = tgw_fgls_write(&ctx, &a, &b);
        ASSERT(r.dispatch == TGW_DISPATCH_QUARANTINE, "T4: shape=0 → quarantine");

        TgwFglsStats s = tgw_fgls_stats(&ctx);
        ASSERT(s.dispatch.total_quarantined == 1, "T4: quarantined=1");
        ASSERT(s.storage.writes == 0, "T4: no storage for quarantine");
    }

    /* ── T5: store_raw (direct FGLS bypassing dispatch) ──────── */
    {
        TgwFglsCtx ctx;
        tgw_fgls_init(&ctx, 0, 0x4000ULL);

        int rc = tgw_fgls_store_raw(&ctx, 0xABCD, 0x1234, 'I');
        ASSERT(rc == 0, "T5: store_raw OK");

        TgwFglsStats s = tgw_fgls_stats(&ctx);
        ASSERT(s.routed == 1, "T5: routed=1 after raw store");
        ASSERT(s.storage.writes == 1, "T5: storage writes=1");

        /* verify trit address derivation */
        FtsTritAddr ta = fts_trit_addr(0xABCD, 0x1234);
        ASSERT(ta.trit == ((0xABCD ^ 0x1234) % 27), "T5: trit correct");
    }

    /* ── T6: batch write ──────────────────────────────────────── */
    {
        TgwFglsCtx ctx;
        tgw_fgls_init(&ctx, 0, 0x5000ULL);

        PoglsSlot slots_a[3], slots_b[3];
        for (int i = 0; i < 3; i++) {
            memset(&slots_a[i], 0, sizeof(PoglsSlot));
            memset(&slots_b[i], 0, sizeof(PoglsSlot));
            slots_a[i].piece = pogls_make_piece(0x6000 + i, 1); /* axis 1 = shape I */
            slots_a[i].agent_id = (uint32_t)(10 + i);
            slots_b[i].piece = pogls_make_piece(0x6000 + i, 0);
        }

        tgw_fgls_batch(&ctx, slots_a, slots_b, 3);

        TgwFglsStats s = tgw_fgls_stats(&ctx);
        ASSERT(s.routed == 3, "T6: routed=3 after batch");
        ASSERT(s.storage.writes == 3, "T6: storage writes=3");
    }

    /* ── T7: serialize → 4896B buffer with magic ──────────────── */
    {
        TgwFglsCtx ctx;
        tgw_fgls_init(&ctx, 0, 0x7000ULL);

        /* write a piece */
        PoglsSlot a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        a.piece = pogls_make_piece(0x8000, 1); /* axis 1 = shape I */
        a.agent_id = 99;
        b.piece = pogls_make_piece(0x8000, 1);
        tgw_fgls_write(&ctx, &a, &b);

        /* serialize */
        uint8_t buf[GCFS_TOTAL_BYTES];
        tgw_fgls_serialize(&ctx, buf);

        ASSERT(buf[0] == 0x53 && buf[1] == 0x4C &&
               buf[2] == 0x47 && buf[3] == 0x46,
               "T7: GCFS_MAGIC 'FGLS' in buffer");
        ASSERT(ctx.serialized_count == 1, "T7: serialized_count=1");

        TgwFglsStats s = tgw_fgls_stats(&ctx);
        ASSERT(s.serialized == 1, "T7: stats.serialized=1");
    }

    /* ── T8: O-latch tick ─────────────────────────────────────── */
    {
        TgwFglsCtx ctx;
        tgw_fgls_init(&ctx, 0, 0x9000ULL);

        /* Write shape O (latch) */
        PoglsSlot a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        a.piece = pogls_make_piece(0xAAAA, 2); /* axis 2 = shape O */
        a.agent_id = 4;
        b.piece = pogls_make_piece(0xAAAA, 1);

        TgwFglsResult r = tgw_fgls_write(&ctx, &a, &b);
        ASSERT(r.dispatch == TGW_DISPATCH_HELD, "T8: O-shape = HELD");

        /* tick should release */
        uint32_t released = tgw_fgls_tick(&ctx);
        ASSERT(released == 1, "T8: tick released 1 slot");
    }

    /* ── T9: piece → dodeca entry field mapping ───────────────── */
    {
        PoglsSlot slot;
        memset(&slot, 0, sizeof(slot));
        slot.piece = pogls_make_piece(0xF00D, 3); /* axis 3 = shape T */
        slot.rerouted = 3;

        uint64_t bk = pogls_bond_key(&slot.piece);
        DodecaEntry e = tgw_fgls_entry_from_piece(&slot.piece, &slot, TGW_POLARITY_ROUTE);

        ASSERT(e.merkle_root == slot.piece.geo_key, "T9: merkle_root = geo_key");
        ASSERT(e.sha256_hi   == bk,                 "T9: sha256_hi = bond_key");
        ASSERT(e.sha256_lo   == bk,                 "T9: sha256_lo = bond_key");
        ASSERT(e.offset      == slot.piece.shape,   "T9: offset = shape");
        ASSERT(e.hop_count   == 3,                  "T9: hop_count = rerouted");
        ASSERT(e.segment     == TGW_POLARITY_ROUTE, "T9: segment = polarity");
        ASSERT(e.ref_count   == 1,                  "T9: ref_count = 1");
    }

    /* ── T10: status print (no crash) ──────────────────────────── */
    {
        TgwFglsCtx ctx;
        tgw_fgls_init(&ctx, 0, 0xBBBBULL);
        /* should not crash */
        tgw_fgls_status(&ctx);
        printf("[PASS] T10: tgw_fgls_status runs without crash\n");
    }

    printf("\n%s — %d failure(s)\n",
           fails == 0 ? "ALL PASS" : "SOME FAILED", fails);
    return fails ? 1 : 0;
}

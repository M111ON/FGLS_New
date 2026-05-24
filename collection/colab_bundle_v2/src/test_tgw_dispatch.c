#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <assert.h>
#include "tgw_bond_dispatch.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ✓ %s\n", msg); g_pass++; } \
    else { printf("  ✗ FAIL: %s\n", msg); g_fail++; assert(0); } \
} while(0)

static void section(int n, const char *title) {
    printf("\n[%d] %s\n", n, title);
}

int main(void) {
    printf("══════════════════════════════════════════════════════\n");
    printf("  TGW BOND DISPATCH — test suite\n");
    printf("  TRing slots: %u\n", TGW_TRING_SLOTS);
    printf("══════════════════════════════════════════════════════\n");

    /* [1] shape_polarity mapping */
    section(1, "shape → polarity");
    CHECK(tgw_shape_polarity(SHAPE_I) == TGW_POLARITY_ROUTE,  "I → ROUTE");
    CHECK(tgw_shape_polarity(SHAPE_O) == TGW_POLARITY_ROUTE,  "O → ROUTE");
    CHECK(tgw_shape_polarity(SHAPE_T) == TGW_POLARITY_ROUTE,  "T → ROUTE");
    CHECK(tgw_shape_polarity(SHAPE_J) == TGW_POLARITY_ROUTE,  "J → ROUTE");
    CHECK(tgw_shape_polarity(SHAPE_S) == TGW_POLARITY_GROUND, "S → GROUND");
    CHECK(tgw_shape_polarity(SHAPE_Z) == TGW_POLARITY_GROUND, "Z → GROUND");
    CHECK(tgw_shape_polarity(SHAPE_L) == TGW_POLARITY_GROUND, "L → GROUND");
    CHECK(tgw_shape_polarity(0x00) == 0xFF, "0x00 → FAULT");
    CHECK(tgw_shape_polarity(0x99) == 0xFF, "unknown → FAULT");

    /* [2] TRing slot allocation */
    section(2, "TRing slot allocation");
    for (int i = 0; i < 100; i++) {
        uint16_t r = tgw_tring_pos((uint64_t)i * 0x9e3779b97f4a7c15ULL, TGW_POLARITY_ROUTE);
        CHECK(r < TGW_TRING_SLOTS, "ROUTE slot in range");
    }
    for (int i = 0; i < 100; i++) {
        uint16_t g = tgw_tring_pos((uint64_t)i * 0x9e3779b97f4a7c15ULL, TGW_POLARITY_GROUND);
        CHECK(g < TGW_TRING_SLOTS, "GROUND slot in range");
        CHECK((g & 1) == 1, "GROUND slot is odd");
    }

    /* [3] full dispatch: I-shape ROUTE */
    section(3, "I-shape dispatch (ROUTE)");
    {
        TgwDispatchCtx ctx;
        tgw_dispatch_init(&ctx, 0);

        uint64_t seed = pogls_fibo_addr(0x9009000000000001ULL);
        PoglsSlot slot_a = {0}, slot_b = {0};
        slot_a.piece    = pogls_make_piece(seed, 1);
        slot_b.piece    = pogls_make_piece(seed ^ 1, 3);
        slot_a.agent_id = 1;
        slot_b.agent_id = 2;

        TgwDispatchResult r = tgw_dispatch(&ctx, &slot_a, &slot_b, NULL, 0);
        CHECK(r == TGW_DISPATCH_OK, "I-shape → DISPATCH_OK");
        CHECK(ctx.total_dispatched == 1, "counter incremented");
        CHECK(ctx.active_count == 1, "active slot registered");
    }

    /* [4] O-shape HELD */
    section(4, "O-shape dispatch (HELD)");
    {
        TgwDispatchCtx ctx;
        tgw_dispatch_init(&ctx, 0);

        PoglsSlot slot_a = {0}, slot_b = {0};
        slot_a.piece    = pogls_make_piece(pogls_fibo_addr(0xCAFE000000000001ULL), 2);
        slot_b.piece    = pogls_make_piece(pogls_fibo_addr(0xCAFE000000000002ULL), 3);
        slot_a.agent_id = 1;
        slot_b.agent_id = 2;

        TgwDispatchResult r = tgw_dispatch(&ctx, &slot_a, &slot_b, NULL, 0);
        CHECK(r == TGW_DISPATCH_HELD, "O-shape → HELD");
        /* find the O-shaped slot in the ring */
        uint16_t o_count = 0;
        for (uint16_t i = 0; i < TGW_TRING_SLOTS; i++) {
            if (ctx.ring[i].hold_count == TGW_HOLD_CYCLES) o_count++;
        }
        CHECK(o_count == 1, "one slot with hold_count = TGW_HOLD_CYCLES");

        uint32_t released = tgw_tick(&ctx);
        CHECK(released == 1, "tick → 1 released");
    }

    /* [5] T-shape FANOUT */
    section(5, "T-shape dispatch (FANOUT)");
    {
        TgwDispatchCtx ctx;
        tgw_dispatch_init(&ctx, 0);

        PoglsSlot slot_a = {0}, slot_b = {0};
        slot_a.piece    = pogls_make_piece(pogls_fibo_addr(0xBEEF000000000001ULL), 3);
        slot_b.piece    = pogls_make_piece(pogls_fibo_addr(0xBEEF000000000002ULL), 1);
        slot_a.agent_id = 1;
        slot_b.agent_id = 2;

        uint32_t targets[3] = {10, 20, 30};
        TgwDispatchResult r = tgw_dispatch(&ctx, &slot_a, &slot_b, targets, 3);
        CHECK(r == TGW_DISPATCH_FANOUT, "T-shape → FANOUT");
        /* find the T-shaped slot */
        const TgwTRingSlot *ts = NULL;
        for (uint16_t i = 0; i < TGW_TRING_SLOTS; i++) {
            if (ctx.ring[i].shape == SHAPE_T) { ts = &ctx.ring[i]; break; }
        }
        CHECK(ts != NULL, "T-shape slot found in ring");
        CHECK(ts->fanout_n == 3, "fanout_n = 3");
        CHECK(ts->fanout_ids[0] == 10, "fanout_id[0] = 10");
        CHECK(ts->fanout_ids[2] == 30, "fanout_id[2] = 30");
    }

    /* [6] S-shape GROUND */
    section(6, "S-shape dispatch (GROUND)");
    {
        TgwDispatchCtx ctx;
        tgw_dispatch_init(&ctx, 0);

        PoglsSlot slot_a = {0}, slot_b = {0};
        slot_a.piece    = pogls_make_piece(pogls_fibo_addr(0xDEAD000000000001ULL), 4);
        slot_b.piece    = pogls_make_piece(pogls_fibo_addr(0xDEAD000000000002ULL), 1);
        slot_a.agent_id = 1;
        slot_b.agent_id = 2;

        TgwDispatchResult r = tgw_dispatch(&ctx, &slot_a, &slot_b, NULL, 0);
        CHECK(r == TGW_DISPATCH_GROUND, "S-shape → GROUND");
        CHECK(ctx.total_grounded == 1, "grounded counter = 1");
        /* find the S-shaped slot and verify it's odd */
        const TgwTRingSlot *ts = NULL;
        for (uint16_t i = 0; i < TGW_TRING_SLOTS; i++) {
            if (ctx.ring[i].shape == SHAPE_S) { ts = &ctx.ring[i]; break; }
        }
        CHECK(ts != NULL, "S-shape slot found in ring");
        CHECK((ts->tring_pos & 1) == 1, "GROUND slot is odd");
    }

    /* [7] shape=0 → QUARANTINE */
    section(7, "shape=0 → QUARANTINE");
    {
        TgwDispatchCtx ctx;
        tgw_dispatch_init(&ctx, 0);

        PoglsSlot slot_a = {0}, slot_b = {0};
        slot_a.piece    = pogls_make_piece(pogls_fibo_addr(0xAAAA000000000001ULL), 0);
        slot_b.piece    = pogls_make_piece(pogls_fibo_addr(0xAAAA000000000002ULL), 1);
        slot_a.agent_id = 1;
        slot_b.agent_id = 2;
        /* axis=0 → shape=0x00 */
        slot_a.piece.shape = 0x00;

        TgwDispatchResult r = tgw_dispatch(&ctx, &slot_a, &slot_b, NULL, 0);
        CHECK(r == TGW_DISPATCH_QUARANTINE, "shape=0 → QUARANTINE");
        CHECK(ctx.total_quarantined == 1, "quarantined counter = 1");
    }

    /* [8] stats sanity */
    section(8, "dispatch stats");
    {
        TgwDispatchCtx ctx;
        tgw_dispatch_init(&ctx, 0);

        /* dispatch 3 mixed shapes */
        uint64_t base = 0x1234000000000000ULL;
        for (int i = 0; i < 3; i++) {
            PoglsSlot a = {0}, b = {0};
            a.piece = pogls_make_piece(pogls_fibo_addr(base ^ (uint64_t)(i*2)), 1);
            b.piece = pogls_make_piece(pogls_fibo_addr(base ^ (uint64_t)(i*2+1)), 3);
            a.agent_id = (uint32_t)i;
            b.agent_id = (uint32_t)i+10;
            tgw_dispatch(&ctx, &a, &b, NULL, 0);
        }
        TgwStats s = tgw_get_stats(&ctx);
        CHECK(s.active_count == 3,      "active = 3");
        CHECK(s.total_dispatched == 3,  "dispatched = 3");
        CHECK(s.total_grounded == 0,    "no ground shapes");
        CHECK(s.total_quarantined == 0, "no quarantine");
        CHECK(s.route_slots > 0,        "route slots occupied");
    }

    /* [9] slot query */
    section(9, "slot query");
    {
        TgwDispatchCtx ctx;
        tgw_dispatch_init(&ctx, 0);

        PoglsSlot a = {0}, b = {0};
        a.piece = pogls_make_piece(pogls_fibo_addr(0x9009900900000001ULL), 1);
        b.piece = pogls_make_piece(pogls_fibo_addr(0x9009900900000002ULL), 3);
        a.agent_id = 42;
        b.agent_id = 99;
        tgw_dispatch(&ctx, &a, &b, NULL, 0);

        /* dispatch uses bond_valid ? pk : bond_key(slot_a) since pieces not origin-paired */
        uint64_t routing_key = pogls_bond_key(&a.piece);
        uint16_t pos = tgw_tring_pos(routing_key, TGW_POLARITY_ROUTE);
        const TgwTRingSlot *ts = tgw_slot_at(&ctx, pos);
        CHECK(ts != NULL, "slot found by position");
        CHECK(ts->bond_key == routing_key, "bond_key matches routing_key");
        CHECK(ts->agent_id == 42, "agent_id matches");

        const TgwTRingSlot *ts2 = tgw_find_slot(&ctx, routing_key, TGW_POLARITY_ROUTE);
        CHECK(ts2 == ts, "find_slot matches slot_at");
        CHECK(tgw_find_slot(&ctx, routing_key ^ 1, TGW_POLARITY_ROUTE) == NULL,
              "find_slot returns NULL for wrong key");
    }

    printf("\n══════════════════════════════════════════════════════\n");
    if (g_fail == 0) {
        printf("  ALL TESTS PASSED  (%d/%d)\n", g_pass, g_pass + g_fail);
    } else {
        printf("  FAILED: %d  PASSED: %d\n", g_fail, g_pass);
    }
    printf("══════════════════════════════════════════════════════\n");
    return g_fail ? 1 : 0;
}

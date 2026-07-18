/*
 * test_context_store.c — Test Residual Space as Temporary Context Store
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "context_store.h"
#include "sid.h"

/* ── Test 1: Basic freeze/thaw roundtrip ──────────────────── */
static int test_freeze_thaw(ContextStore *cs) {
    printf("Test 1: Basic freeze/thaw roundtrip...\n");

    PoglsPiece piece = pogls_make_piece(0xDEADBEEF, 0);

    const char *data = "ghost data — frozen in time";
    uint32_t size = strlen(data) + 1;

    uint64_t bk = ctx_store_freeze(cs, &piece, data, size, 1);
    assert(bk != RS_BOND_KEY_RESERVED);

    uint32_t thaw_size = 0;
    const void *thawed = ctx_store_thaw(cs, bk, &thaw_size);
    assert(thawed != NULL);
    assert(thaw_size == size);
    assert(memcmp(thawed, data, size) == 0);

    printf("  PASS: freeze/thaw roundtrip (bond_key=0x%016llx)\n",
           (unsigned long long)bk);
    return 1;
}

/* ── Test 2: Multiple entries ────────────────────────────── */
static int test_multiple_entries(ContextStore *cs) {
    printf("Test 2: Multiple entries...\n");

    PoglsPiece p1 = pogls_make_piece(0x1111, 0);
    PoglsPiece p2 = pogls_make_piece(0x2222, 0);
    PoglsPiece p3 = pogls_make_piece(0x3333, 0);

    const char *d1 = "entry 1";
    const char *d2 = "entry 2";
    const char *d3 = "entry 3";

    uint64_t bk1 = ctx_store_freeze(cs, &p1, d1, strlen(d1)+1, 0);
    uint64_t bk2 = ctx_store_freeze(cs, &p2, d2, strlen(d2)+1, 0);
    uint64_t bk3 = ctx_store_freeze(cs, &p3, d3, strlen(d3)+1, 0);

    assert(bk1 != RS_BOND_KEY_RESERVED);
    assert(bk2 != RS_BOND_KEY_RESERVED);
    assert(bk3 != RS_BOND_KEY_RESERVED);
    assert(bk1 != bk2);
    assert(bk2 != bk3);

    uint32_t sz;
    assert(memcmp(ctx_store_thaw(cs, bk1, &sz), d1, sz) == 0);
    assert(memcmp(ctx_store_thaw(cs, bk2, &sz), d2, sz) == 0);
    assert(memcmp(ctx_store_thaw(cs, bk3, &sz), d3, sz) == 0);

    assert(ctx_store_count(cs) == 4); /* 1 from test1 + 3 new */

    printf("  PASS: 3 entries, all thaw correctly\n");
    return 1;
}

/* ── Test 3: Verify integrity ────────────────────────────── */
static int test_verify(ContextStore *cs) {
    printf("Test 3: Verify bond_key integrity...\n");

    PoglsPiece piece = pogls_make_piece(0xCAFEBABE, 0);

    const char *data = "verify me";
    uint64_t bk = ctx_store_freeze(cs, &piece, data, strlen(data)+1, 0);
    assert(bk != RS_BOND_KEY_RESERVED);

    assert(ctx_store_verify(cs, &piece) == 1);

    PoglsPiece wrong = pogls_make_piece(0x00000000, 0);
    assert(ctx_store_verify(cs, &wrong) == 0);

    printf("  PASS: verify integrity works\n");
    return 1;
}

/* ── Test 4: Eviction ────────────────────────────────────── */
static int test_eviction(ContextStore *cs) {
    printf("Test 4: Eviction...\n");

    uint32_t before = ctx_store_count(cs);
    int evicted = ctx_store_evict(cs);
    assert(evicted == 1);
    assert(ctx_store_count(cs) == before - 1);

    printf("  PASS: evicted 1 entry, count=%u\n", ctx_store_count(cs));
    return 1;
}

/* ── Test 5: Context building pattern ────────────────────── */
static int test_context_pattern(ContextStore *cs) {
    printf("Test 5: Context building pattern...\n");

    PoglsPiece piece = pogls_make_piece(0x7C07EC71, 0);

    const char *context_data = "This is temporary context before ingest";
    uint64_t bk = ctx_store_freeze(cs, &piece, context_data,
                                     strlen(context_data)+1, 1);

    uint32_t ctx_size = 0;
    const void *ctx = ctx_store_thaw(cs, bk, &ctx_size);
    assert(ctx != NULL);
    assert(ctx_size > 0);

    printf("  Context: \"%s\" (%u bytes)\n", (const char *)ctx, ctx_size);

    /* Check ingest-ready flag */
    uint32_t mask = cs->rs.capacity - 1;
    uint32_t slot = _rs_hash(bk, mask);
    while (cs->rs.entries[slot]) {
        ResidualEntry *e = cs->rs.entries[slot];
        if ((e->flags & RS_ENTRY_VALID) && e->bond_key == bk) {
            assert(e->flags & CS_FLAG_INGEST_READY);
            printf("  PASS: ingest-ready flag set\n");
            return 1;
        }
        slot = (slot + 1) & mask;
    }

    printf("  FAIL: entry not found\n");
    return 0;
}

/* ── Main ────────────────────────────────────────────────── */
int main(void) {
    printf("═══════════════════════════════════════════════\n");
    printf("  Context Store Test Suite\n");
    printf("  Residual Space as Temporary Context Store\n");
    printf("═══════════════════════════════════════════════\n\n");

    ContextStore cs;
    ctx_store_init(&cs, 256);

    int pass = 0, fail = 0;

    pass += test_freeze_thaw(&cs);
    pass += test_multiple_entries(&cs);
    pass += test_verify(&cs);
    pass += test_eviction(&cs);
    pass += test_context_pattern(&cs);

    fail = 5 - pass;

    printf("\n── Stats ──\n");
    ctx_store_stats(&cs);

    ctx_store_free(&cs);

    printf("\n═══════════════════════════════════════════════\n");
    printf("  Results: %d/%d pass, %d fail\n", pass, pass+fail, fail);
    printf("═══════════════════════════════════════════════\n");

    return fail ? 1 : 0;
}

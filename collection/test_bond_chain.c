/*
 * test_bond_chain.c — verify pogls_bond_chain.h
 *
 * Compile:
 *   gcc -O2 -I. -o test_bond_chain test_bond_chain.c
 *
 * Tests:
 *   [1] Build chain, walk returns original order
 *   [2] Reroute: origin_key stable, prev/next stable
 *   [3] Scatter: find by bond_key from any position
 *   [4] Mixed route hints → stats count correct
 *   [5] Hash table build + O(1) find
 *   [6] Hash table miss (non-existent key)
 *   [7] Hash table find after reroute (origin_key still works)
 *   [8] Hash table large N smoke
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "pogls_bond_chain.h"

#define N 16
#define FACE_MAX 12   /* gp_level=1 */

static int pass=0, fail=0;
#define CHECK(cond, msg) do { \
    if(cond){printf("  ok  %s\n",msg);pass++;} \
    else    {printf("  FAIL %s\n",msg);fail++;} \
} while(0)

int main(void) {
    printf("==================================\n");
    printf("  pogls_bond_chain test  N=%d\n", N);
    printf("==================================\n\n");

    BondChain bc;
    assert(bond_chain_init(&bc, N) == 0);
    bond_chain_build(&bc, FACE_MAX);

    /* [1] Walk order */
    printf("[1] chain walk\n");
    uint32_t order[N];
    uint32_t walked = bond_chain_walk(&bc, order, N);
    CHECK(walked == N, "walked all chunks");
    int seq_ok = 1;
    for (uint32_t i = 0; i < walked; i++)
        if (order[i] != i) { seq_ok = 0; break; }
    CHECK(seq_ok, "walk returns 0..N-1 in order");

    /* [2] Reroute — origin_key must survive */
    printf("\n[2] reroute stability\n");
    uint64_t ok_before[N];
    for (int i = 0; i < N; i++)
        ok_before[i] = bc.nodes[i].origin_key;

    bond_chain_reroute(&bc, 3, POGLS_OVERFLOW);
    bond_chain_reroute(&bc, 7, POGLS_FAULT);

    CHECK(bc.nodes[3].origin_key == ok_before[3], "node[3] origin_key stable after reroute");
    CHECK(bc.nodes[7].origin_key == ok_before[7], "node[7] origin_key stable after reroute");
    CHECK(bc.nodes[3].flags & BNODE_FLAG_REROUTED, "node[3] rerouted flag set");

    /* prev/next chain links unaffected */
    uint64_t prev3 = bc.nodes[3].prev_key;
    uint64_t next3 = bc.nodes[3].next_key;
    CHECK(prev3 == bc.nodes[2].origin_key, "node[3].prev_key -> node[2]");
    CHECK(next3 == bc.nodes[4].origin_key, "node[3].next_key -> node[4]");

    /* [3] Scatter: find any node by bond_key */
    printf("\n[3] scatter find\n");
    uint64_t target = bc.nodes[9].origin_key;
    uint32_t found  = bond_chain_find(&bc, target);
    CHECK(found == 9, "find node[9] by bond_key from any position");

    target = bc.nodes[0].origin_key;
    found  = bond_chain_find(&bc, target);
    CHECK(found == 0, "find head node by bond_key");

    /* [4] Route hints + stats */
    printf("\n[4] route hints / stats\n");
    bond_chain_set_route(&bc, 0,  BOND_ROUTE_RAM);
    bond_chain_set_route(&bc, 4,  BOND_ROUTE_VRAM);
    bond_chain_set_route(&bc, 8,  BOND_ROUTE_DISK);
    bond_chain_set_route(&bc, 12, BOND_ROUTE_NET);

    BondChainStats s = bond_chain_stats(&bc);
    CHECK(s.n_total == N,         "stats: n_total correct");
    CHECK(s.n_rerouted == 2,      "stats: 2 rerouted");
    CHECK(s.n_head == 1,          "stats: 1 head");
    CHECK(s.n_tail == 1,          "stats: 1 tail");
    CHECK(s.chain_broken == 0,    "stats: chain intact");
    CHECK(s.n_route[BOND_ROUTE_RAM]  == 1, "route RAM count=1");
    CHECK(s.n_route[BOND_ROUTE_VRAM] == 1, "route VRAM count=1");

    /* [5] Hash table build + O(1) find */
    printf("\n[5] hash table build + O(1) find\n");
    int ht_ret = bond_chain_build_ht(&bc);
    CHECK(ht_ret == 0, "bond_chain_build_ht returned 0");

    CHECK(bc.ht.capacity >= N, "hash table capacity >= N");
    CHECK((bc.ht.capacity & (bc.ht.capacity - 1)) == 0, "hash table capacity is power of 2");

    for (uint32_t i = 0; i < N; i++) {
        uint64_t k = bc.nodes[i].origin_key;
        uint32_t idx = bond_chain_find_ht(&bc, k);
        if (idx != i) {
            printf("  FAIL ht_find[%u] expected %u got %u\n", i, i, idx);
            fail++;
            goto after_ht_find;
        }
    }
    CHECK(1, "bond_chain_find_ht O(1) finds all N nodes");

after_ht_find:

    /* [6] Hash table miss */
    printf("\n[6] hash table miss\n");
    uint64_t fake = 0xDEADBEEFCAFEBABEULL;
    /* Verify fake_key is not in the chain */
    uint32_t not_found = bond_chain_find_ht(&bc, fake);
    CHECK(not_found == UINT32_MAX, "non-existent key returns UINT32_MAX");

    /* [7] Hash table find after reroute (origin_key still works) */
    printf("\n[7] ht find after reroute\n");
    uint32_t idx_r3 = bond_chain_find_ht(&bc, ok_before[3]);  /* origin_key of node 3 */
    CHECK(idx_r3 == 3, "ht find node[3] by origin_key after reroute");

    /* [8] Hash table large N smoke */
    printf("\n[8] large N smoke test\n");

    bond_chain_free(&bc);

    uint32_t BIG = 10000;
    BondChain big;
    assert(bond_chain_init(&big, BIG) == 0);
    bond_chain_build(&big, 64);

    ht_ret = bond_chain_build_ht(&big);
    CHECK(ht_ret == 0, "large N: build_ht ok");

    uint32_t ht_hits = 0;
    for (uint32_t i = 0; i < BIG; i++) {
        uint64_t k = big.nodes[i].origin_key;
        if (bond_chain_find_ht(&big, k) == i) ht_hits++;
    }
    CHECK(ht_hits == BIG, "large N: all finds hit");

    uint32_t miss = 0;
    for (uint32_t i = 0; i < 100; i++) {
        uint64_t fk = (uint64_t)i * 0xFACEB00C + 0xDEAD;
        if (bond_chain_find_ht(&big, fk) == UINT32_MAX) miss++;
    }
    CHECK(miss == 100, "large N: all 100 fake keys miss");

    bond_chain_free(&big);

    printf("\n==================================\n");
    if (fail == 0)
        printf("  ALL PASS  (%d/%d)\n", pass, pass+fail);
    else
        printf("  FAIL=%d  PASS=%d/%d\n", fail, pass, pass+fail);
    printf("==================================\n");
    return fail ? 1 : 0;
}

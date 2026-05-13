/*
 * test_frustum_wire.c — verify tgw_frustum_wire.h
 * gcc -O2 -o test_frustum_wire test_frustum_wire.c && ./test_frustum_wire
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* stubs */

#include "geo_temporal_lut.h"
#include "geo_goldberg_lut.h"
#include "geo_metatron_route.h"
#include "tgw_frustum_wire.h"

static int pass=0, fail=0;
#define CHK(cond,msg) do{ if(cond){printf("  ✓ %s\n",msg);pass++;}\
                          else{printf("  ✗ FAIL: %s\n",msg);fail++;} }while(0)

int main(void)
{
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  tgw_frustum_wire — frustum parallel + Option B ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    /* ── 1. frustum parallel write — all Lv1 addresses ── */
    printf("▶ FRUSTUM_PARALLEL_HOOK — full Lv1 sweep\n");
    FrustumStore fs;
    frustum_store_init(&fs);

    for (uint64_t a = 0; a < FRUSTUM_TETRA_CEILING; a++)
        FRUSTUM_PARALLEL_HOOK(a, (uint64_t)(a * 7u), &fs, FIBO_SEED_DEFAULT);

    CHK(fs.total_writes == FRUSTUM_TETRA_CEILING, "3456 writes recorded");
    CHK(fs.total_silenced == 0u, "no silenced (no mask set)");

    FrustumStats st = frustum_stats(&fs);
    CHK(st.occupied_slots == TRIT_MOD, "27 of 54 slots occupied (trit space = 27 reachable)");

    /* ── 2. merkle_of: deterministic ── */
    printf("\n▶ _frustum_merkle_of — deterministic\n");
    CHK(_frustum_merkle_of(100, 42) == _frustum_merkle_of(100, 42),
        "same inputs → same merkle");
    CHK(_frustum_merkle_of(100, 42) != _frustum_merkle_of(100, 43),
        "diff val → diff merkle");

    /* ── 3. metatron_target_face_b — tetra zone ── */
    printf("\n▶ metatron_target_face_b — tetra zone [0..3455]\n");
    int tetra_ok = 1;
    for (uint64_t a = 0; a < FRUSTUM_TETRA_CEILING; a++) {
        uint8_t f = metatron_target_face_b(a);
        if (f > 11u) { tetra_ok = 0; break; }
    }
    CHK(tetra_ok, "all tetra addrs → face 0..11");

    /* spot checks */
    uint8_t f0  = metatron_target_face_b(0);
    uint8_t f60 = metatron_target_face_b(60);
    CHK(f0  == (uint8_t)TRING_COMP(GEO_WALK[0]),  "addr=0  → TRING_COMP(GEO_WALK[0])");
    CHK(f60 == (uint8_t)TRING_COMP(GEO_WALK[60]), "addr=60 → TRING_COMP(GEO_WALK[60])");

    /* ── 4. metatron_target_face_b — octa zone ── */
    printf("\n▶ metatron_target_face_b — octa zone [3456..6911]\n");
    int octa_ok = 1;
    for (uint64_t a = FRUSTUM_TETRA_CEILING; a < FRUSTUM_JUNCTION; a++) {
        uint8_t f = metatron_target_face_b(a);
        if (f > 11u) { octa_ok = 0; break; }
    }
    CHK(octa_ok, "all octa addrs → face 0..11");

    /* octa face = CROSS of tetra face at mirrored pos */
    uint64_t octa_addr = FRUSTUM_TETRA_CEILING;   /* addr=3456 */
    uint32_t octa_pos  = 0u;                        /* (3456-3456)%720=0 */
    uint8_t  src_f     = (uint8_t)TRING_COMP(GEO_WALK[octa_pos]);
    uint8_t  expected  = METATRON_CROSS[src_f];
    CHK(metatron_target_face_b(octa_addr) == expected,
        "octa addr=3456 → CROSS[src_face] ✓");

    /* octa face ≠ tetra face at same walk pos (CROSS flips ring) */
    uint8_t tetra_f = metatron_target_face_b(0);
    uint8_t octa_f  = metatron_target_face_b(FRUSTUM_TETRA_CEILING);
    CHK(tetra_f != octa_f || METATRON_CROSS[tetra_f] == octa_f,
        "octa face is ring-flipped from tetra");

    /* ── 5. above junction wraps cleanly ── */
    printf("\n▶ metatron_target_face_b — above junction wraps\n");
    uint8_t fa = metatron_target_face_b(FRUSTUM_JUNCTION);
    uint8_t fb = metatron_target_face_b(0);   /* 6912 % 6912 = 0 */
    CHK(fa == fb, "addr=6912 wraps to 0 → same face");
    uint8_t fc = metatron_target_face_b(FRUSTUM_JUNCTION + 60);
    uint8_t fd = metatron_target_face_b(60);
    CHK(fc == fd, "addr=6912+60 wraps to 60 → same face");

    /* ── 6. metatron_b_decide — route validity ── */
    printf("\n▶ metatron_b_decide — all zones\n");
    int route_ok = 1;
    uint64_t test_addrs[] = {0,42,719,720,3455,3456,3500,6911,6912,7000};
    for (int i = 0; i < 10; i++) {
        MetatronB mb = metatron_b_decide(test_addrs[i]);
        if (mb.target_face > 11u) { route_ok=0; break; }
        if (mb.route > META_ROUTE_HUB) { route_ok=0; break; }
    }
    CHK(route_ok, "all test addrs: target_face 0..11, route valid");

    /* ── 7. TGW_FULL_HOOK — combined ── */
    printf("\n▶ TGW_FULL_HOOK — combined frustum + metatron\n");
    frustum_store_init(&fs);
    MetatronB mb;
    TGW_FULL_HOOK(1234u, 0xABCDu, &fs, FIBO_SEED_DEFAULT, mb);
    CHK(fs.total_writes == 1u, "frustum write fired");
    CHK(mb.target_face <= 11u, "metatron target_face valid");
    CHK(mb.route <= META_ROUTE_HUB, "metatron route valid");

    /* ── 8. sacred numbers ── */
    printf("\n▶ Sacred numbers\n");
    CHK(FRUSTUM_TETRA_CEILING == 3456u, "TETRA_CEILING=3456=GEO_FULL_N");
    CHK(FRUSTUM_JUNCTION      == 6912u, "JUNCTION=6912=2⁸×3³");
    CHK(FRUSTUM_JUNCTION == FRUSTUM_TETRA_CEILING * 2u, "6912=3456×2 ✓");

    printf("\n══════════════════════════════════════════════════\n");
    printf("  PASS: %d   FAIL: %d\n", pass, fail);
    if (!fail) printf("✅ All PASS\n");
    else       printf("❌ %d FAIL\n", fail);
    return fail ? 1 : 0;
}

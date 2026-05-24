#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geo_metatron_reshape.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok  %s\n", msg); pass++; } \
    else      { printf("  FAIL %s\n", msg); fail++; } \
} while(0)

int main(void) {
    printf("========================================\n");
    printf("  geo_metatron_reshape test\n");
    printf("========================================\n\n");

    /* [1] Internal verify suite — covers all invariants */
    printf("[1] geo_metatron_reshape_verify()\n");
    CHECK(geo_metatron_reshape_verify() == 0, "verify() == 0");

    /* [2] peano_l2: 81 cells fill 9x9 exactly once */
    printf("\n[2] peano_l2\n");
    uint8_t grid[9][9]; memset(grid, 0, sizeof(grid));
    for (uint16_t i = 0; i < 81; i++) {
        PeanoCoord c = peano_l2((uint8_t)i);
        CHECK(c.row <= 8 && c.col <= 8, "coord in [0,8]");
        grid[c.row][c.col]++;
    }
    uint32_t filled = 0;
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 9; c++)
            if (grid[r][c] == 1) filled++;
    CHECK(filled == 81, "81 cells filled once");

    /* [3] ico_cpair spot check */
    printf("\n[3] ico_cpair\n");
    CHECK(ico_cpair(0)   == 81, "ico_cpair(0)==81");
    CHECK(ico_cpair(81)  == 0,  "ico_cpair(81)==0");
    CHECK(ico_cpair(40)  == 121, "ico_cpair(40)==121");
    CHECK(ico_cpair(161) == 80,  "ico_cpair(161)==80");
    CHECK(ico_cpair(ico_cpair(123)) == 123, "self-inverse");

    /* [4] meta_cpair spot check */
    printf("\n[4] meta_cpair\n");
    CHECK(meta_cpair(0)    == 720,  "cpair(0)==720");
    CHECK(meta_cpair(720)  == 0,    "cpair(720)==0");
    CHECK(meta_cpair(1439) == 719,  "cpair(1439)==719");
    CHECK(meta_cpair(meta_cpair(555)) == 555, "self-inverse");

    /* [5] ico_enc roundtrip — first, last, mid */
    printf("\n[5] ico_enc decompose\n");
    uint8_t p2, pi2;
    ico_decompose(ico_enc(0, 0), &p2, &pi2);  CHECK(p2==0 && pi2==0, "pole=0 idx=0");
    ico_decompose(ico_enc(1, 80), &p2, &pi2); CHECK(p2==1 && pi2==80, "pole=1 idx=80");
    ico_decompose(ico_enc(0, 40), &p2, &pi2); CHECK(p2==0 && pi2==40, "pole=0 idx=40");

    /* [6] crop role count */
    printf("\n[6] crop roles\n");
    uint32_t n_v=0, n_e=0, n_c=0, n_s=0;
    for (uint8_t r = 0; r <= 8; r++)
        for (uint8_t c = 0; c <= 8; c++) {
            uint8_t cid;
            uint8_t role = peano_crop(r, c, &cid);
            if (role == VERTEX) n_v++;
            if (role == EDGE)   n_e++;
            if (role == CORE)   n_c++;
            if (role == SHADOW) n_s++;
        }
    CHECK(n_v == 4,   "4 vertex");
    CHECK(n_c == 16,  "16 core");
    CHECK(n_s == 45,  "45 outside");
    CHECK(n_v+n_e+n_c == 36, "36 active total");

    /* [7] geo_metatron_reshape pipeline — 3 positions per pole */
    printf("\n[7] pipeline\n");
    DualCell d;

    d = geo_metatron_reshape(WORLD_A_NORTH, 0, 0);
    CHECK(d.ico_idx == 0 && d.pole == 0 && d.peano_idx == 0, "World A idx=0");

    d = geo_metatron_reshape(WORLD_B_SOUTH, 0, 0);
    CHECK(d.ico_idx == 81 && d.pole == 1 && d.peano_idx == 0, "World B idx=0");

    d = geo_metatron_reshape(WORLD_A_NORTH, 40, 40);
    CHECK(d.cell_role != SHADOW && d.in_crop == 1, "World A idx=40 in crop");

    d = geo_metatron_reshape(WORLD_B_SOUTH, 80, 80);
    CHECK(d.ico_idx == 161, "World B idx=80 ico_idx=161");

    /* [8] 36+28=64 */
    printf("\n[8] invariant\n");
    CHECK(CROP_ACTIVE + SHADOW_COUNT == HILBERT_GRID, "36+28=64");

    printf("\n========================================\n");
    if (fail == 0)
        printf("  ALL PASS  (%d/%d)\n", pass, pass + fail);
    else
        printf("  FAIL=%d  PASS=%d/%d\n", fail, pass, pass + fail);
    printf("========================================\n");
    return fail ? 1 : 0;
}

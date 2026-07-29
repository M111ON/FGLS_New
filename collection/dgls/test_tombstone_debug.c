#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "dgls/geo/include/residual_space.h"
#include "bond/include/pogls_bond.h"

int main() {
    ResidualSpace rs;
    if (rs_init(&rs, 256) != 0) { printf("rs_init FAIL\n"); return 1; }

    uint8_t buf[8];
    PoglsPiece p1 = pogls_make_piece(10, 1);
    PoglsPiece p2 = pogls_make_piece(20, 2);
    PoglsPiece p3 = pogls_make_piece(30, 3);
    memset(buf, 0xAA, 8);

    uint64_t bk1 = rs_freeze(&rs, &p1, buf, 8, 0);
    uint64_t bk2 = rs_freeze(&rs, &p2, buf, 8, 0);
    uint64_t bk3 = rs_freeze(&rs, &p3, buf, 8, 0);
    printf("freeze: bk1=%llu bk2=%llu bk3=%llu\n", (unsigned long long)bk1, (unsigned long long)bk2, (unsigned long long)bk3);

    ResidualSpaceStats st = rs_stats(&rs);
    printf("after freeze: count=%u tombstone=%u\n", st.count, st.tombstone_count);

    int r1 = rs_tombstone(&rs, bk1);
    printf("rs_tombstone(bk1) = %d\n", r1);

    int r2 = rs_tombstone(&rs, bk1);
    printf("rs_tombstone(bk1) again = %d\n", r2);

    st = rs_stats(&rs);
    printf("after tombstone: count=%u tombstone=%u\n", st.count, st.tombstone_count);

    uint32_t swept = rs_tombstone_sweep(&rs);
    printf("rs_tombstone_sweep = %u\n", swept);

    st = rs_stats(&rs);
    printf("after sweep: count=%u tombstone=%u\n", st.count, st.tombstone_count);

    rs_free(&rs);
    printf("TEST PASS\n");
    return 0;
}
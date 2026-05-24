#include <stdio.h>
#include <stdint.h>
#include "frustum_trit.h"

#define PHI_SEED 1696631ULL

int main(void)
{
    int pass = 0, fail = 0;

    /* test cases: addr, value pairs */
    struct { uint64_t addr; uint32_t value; } tests[] = {
        {0,          0},
        {1,          1},
        {63,         255},
        {64,         128},
        {3456,       42},
        {6912,       0xDEAD},
        {27648,      0xFF},
        {0xDEADBEEF, 0x1234},
        {(uint64_t)-1, 0},
        {144,        720},
    };
    int n = sizeof(tests)/sizeof(tests[0]);

    for (int i = 0; i < n; i++) {
        uint64_t addr  = tests[i].addr;
        uint32_t value = tests[i].value;

        TritAddr t = trit_decompose(addr, value, PHI_SEED);

        /* recover addr from slope */
        uint64_t addr2 = trit_recover_addr(t.slope, PHI_SEED);

        /* verify */
        int ok = trit_verify(&t, addr, value, PHI_SEED)
              && (addr2 == addr);

        printf("[%s] addr=%llu val=%u → trit=%u coset=%u face=%u level=%u letter=%c slope_recover=%s\n",
            ok ? "PASS" : "FAIL",
            (unsigned long long)addr, value,
            t.trit, t.coset, t.face, t.level,
            'A' + t.letter,
            addr2 == addr ? "OK" : "MISMATCH");

        ok ? pass++ : fail++;
    }

    printf("\n%d/%d PASS\n", pass, n);
    return fail ? 1 : 0;
}

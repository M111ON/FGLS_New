#include <stdio.h>
#include "frustum_slot64.h"

#define PHI_SEED 1696631ULL

int main(void)
{
    printf("FrustumSlot64 size = %zu (expect 64)\n", sizeof(FrustumSlot64));
    printf("FrustumStore  size = %zu (expect 3456)\n", sizeof(FrustumStore));

    FrustumStore s;
    fstore_init(&s);

    struct { uint64_t addr; uint32_t value; } tests[] = {
        {0,     0xABCD},
        {1,     0x1234},
        {53,    0xDEAD},
        {54,    0xBEEF},   /* wraps slot index */
        {3456,  0xFF00},
        {6912,  0x0042},
        {27648, 0x1111},
    };
    int n = sizeof(tests)/sizeof(tests[0]);
    int pass = 0;

    for (int i = 0; i < n; i++) {
        uint64_t addr  = tests[i].addr;
        uint32_t value = tests[i].value;

        fstore_write(&s, addr, value, PHI_SEED);
        uint32_t got = fstore_read(&s, addr, value, PHI_SEED);
        int ok = (got == value);
        printf("[%s] addr=%-6llu val=0x%04X got=0x%04X\n",
               ok?"PASS":"FAIL",
               (unsigned long long)addr, value, got);
        if (ok) pass++;
    }

    printf("\n%d/%d PASS\n", pass, n);
    return (pass == n) ? 0 : 1;
}

#include <stdio.h>
#include <string.h>
#include "dramtile_store.h"
int main() {
    fprintf(stderr, "step1: macros OK\n");
    DRamTileStore s;
    memset(&s, 0, sizeof(s));
    fprintf(stderr, "step2: store=%zu\n", sizeof(s));
    uint32_t a = dt_name_to_addr("test");
    fprintf(stderr, "step3: addr=%u\n", a);
    return 0;
}

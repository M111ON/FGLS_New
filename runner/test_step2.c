#include <stdio.h>
#include <string.h>
#include "dramtile_store.h"
int main() {
    fprintf(stderr, "step1\n");
    DRamTileStore s;
    memset(&s, 0, sizeof(s));
    fprintf(stderr, "step2: calling init_twin\n");
    int r = dt_store_init_twin(&s, "test_step2.bin", 4UL*1024*1024);
    fprintf(stderr, "step3: init=%d n_stored=%u base=%p cap=%zu\n",
            r, s.n_stored, (void*)s.base, s.capacity);
    if (s.base) {
        fprintf(stderr, "step4: unmap\n");
        dt_store_destroy_twin(&s);
        fprintf(stderr, "step5: done\n");
    }
    remove("test_step2.bin");
    return 0;
}

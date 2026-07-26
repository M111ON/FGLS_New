#include <stdio.h>
#include "dramtile_store.h"

int main(void) {
    DRamTileStore s;
    if (dt_store_init_twin(&s, "test_model.dramtile", 1UL*1024*1024*1024) != 0) {
        fprintf(stderr, "FAIL\n"); return 1;
    }
    printf("n_stored=%u, used=%zu, cap=%zu\n", s.n_stored, s.used, s.capacity);
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (s.hash[i].dram_addr == 0) continue;
        printf("  [%d] addr=0x%08x off=%zu sz=%zu name=%s\n",
               i, s.hash[i].dram_addr, s.hash[i].offset, s.hash[i].size, s.hash[i].name);
    }
    dt_store_destroy_twin(&s);
    return 0;
}
#include <stdio.h>
#include <string.h>
#include "dramtile_store.h"
int main() {
    DRamTileStore store;
    memset(&store, 0, sizeof(store));
    if (dt_store_init_twin(&store, "I:\\FGLS_new\\test_model.dramtile", 1024*1024*1024) != 0) {
        fprintf(stderr, "FAIL: init\n"); return 1;
    }
    fprintf(stderr, "OK: %u tensors, %zu used\n", store.n_stored, store.used);
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store.hash[i].dram_addr == 0) continue;
        if (store.hash[i].dram_addr & 0x80000000u) continue;
        fprintf(stderr, "  [%d] %s addr=0x%x off=%zu sz=%zu\n",
                i, store.hash[i].name, store.hash[i].dram_addr,
                store.hash[i].offset, store.hash[i].size);
    }
    dt_store_destroy_twin(&store);
    return 0;
}

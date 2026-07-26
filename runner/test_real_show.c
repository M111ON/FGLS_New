#include <stdio.h>
#include <string.h>
#include "dramtile_store.h"
int main() {
    DRamTileStore store;
    memset(&store, 0, sizeof(store));
    dt_store_init_twin(&store, "real_weights.dramtile", 1024*1024*1024);
    fprintf(stderr, "%u tensors\n", store.n_stored);
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store.hash[i].dram_addr == 0) continue;
        if (store.hash[i].dram_addr & 0x80000000u) continue;
        fprintf(stderr, "  slot=%3d  name=%-30s  off=%-4zu  sz=%-4zu\n",
                i, store.hash[i].name, store.hash[i].offset, store.hash[i].size);
    }
    dt_store_destroy_twin(&store);
    return 0;
}

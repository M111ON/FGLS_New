#include <stdio.h>
#include <string.h>
#include "dramtile_store.h"
int main() {
    DRamTileStore store;
    memset(&store, 0, sizeof(store));
    int rc = dt_store_init_twin(&store, "real_weights.dramtile", 1024*1024*1024);
    fprintf(stderr, "init rc=%d cap=%zu n=%u used=%zu\n",
            rc, store.capacity, store.n_stored, store.used);
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store.hash[i].dram_addr == 0) continue;
        fprintf(stderr, "  slot=%3d  addr=0x%08x  name=%-30s  off=%-4zu  sz=%-4zu\n",
                i, store.hash[i].dram_addr, store.hash[i].name,
                store.hash[i].offset, store.hash[i].size);
    }
    dt_store_destroy_twin(&store);
    return 0;
}

#include <stdio.h>
#include <string.h>
#include "dramtile_store.h"
int main() {
    fprintf(stderr, "=== Phase 1: init + write\n");
    DRamTileStore s;
    memset(&s, 0, sizeof(s));
    int r = dt_store_init_twin(&s, "test_step3.bin", 4UL*1024*1024);
    fprintf(stderr, "init=%d\n", r);
    
    uint8_t buf[64];
    memset(buf, 0x41, 64);
    uint8_t *p = dt_put(&s, "t1", buf, 64);
    fprintf(stderr, "put=%p\n", (void*)p);
    
    dt_store_destroy_twin(&s);
    fprintf(stderr, "destroyed\n");
    
    fprintf(stderr, "=== Phase 2: reopen\n");
    DRamTileStore s2;
    memset(&s2, 0, sizeof(s2));
    r = dt_store_init_twin(&s2, "test_step3.bin", 4UL*1024*1024);
    fprintf(stderr, "reopen=%d n_stored=%u\n", r, s2.n_stored);
    
    uint8_t *g = dt_get(&s2, "t1");
    fprintf(stderr, "get=%p\n", (void*)g);
    if (g) fprintf(stderr, "val=0x%02x\n", g[0]);
    
    dt_store_destroy_twin(&s2);
    remove("test_step3.bin");
    fprintf(stderr, "done\n");
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dramtile_store.h"

int main() {
    fprintf(stderr, "start\n");
    DRamTileStore s;
    fprintf(stderr, "init...\n");
    int r = dt_store_init_twin(&s, "test_tiny.bin", 4UL*1024*1024);
    fprintf(stderr, "init=%d n_stored=%u\n", r, s.n_stored);
    
    uint8_t buf[64];
    memset(buf, 0x41, 64);
    uint8_t *p = dt_put(&s, "tiny.tensor", buf, 64);
    fprintf(stderr, "put=%p sz=%zu\n", (void*)p, dt_get_size(&s, "tiny.tensor"));
    
    dt_store_destroy_twin(&s);
    fprintf(stderr, "destroyed\n");
    
    r = dt_store_init_twin(&s, "test_tiny.bin", 4UL*1024*1024);
    fprintf(stderr, "reopen=%d n_stored=%u\n", r, s.n_stored);
    
    uint8_t *g = dt_get(&s, "tiny.tensor");
    fprintf(stderr, "get=%p\n", (void*)g);
    if (g && g[0] == 0x41) fprintf(stderr, "PASS\n");
    else fprintf(stderr, "FAIL\n");
    
    dt_store_destroy_twin(&s);
    remove("test_tiny.bin");
    return 0;
}

#include <stdio.h>
#include <stdint.h>
#include "sid.h"

int main(void) {
    printf("sid.h compiled OK\n");
    printf("SID_MAX_ENTRIES = %d\n", SID_MAX_ENTRIES);
    printf("SID_TWIDX_MAGIC = 0x%08X\n", SID_TWIDX_MAGIC);
    
    // Test SID capture with dummy Q8_0 data
    uint8_t q8_data[68]; // 2 blocks × 34 bytes
    for (int i = 0; i < 68; i++) q8_data[i] = (uint8_t)(i * 7);
    
    SIDCoord coord;
    int rc = sid_capture(q8_data, 68, 1, &coord); // dtype=1 (Q8_0)
    printf("sid_capture rc=%d, node_id=%u, drain=%d\n", rc, coord.node_id, coord.drain);
    
    // Test roundtrip
    rc = sid_verify_roundtrip(q8_data, 68, 1, "test_tensor");
    printf("sid_verify_roundtrip rc=%d\n", rc);
    
    return 0;
}

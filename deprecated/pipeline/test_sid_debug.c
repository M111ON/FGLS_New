#include <stdio.h>
#include <stdint.h>
#include "sid.h"

int main(void) {
    // Test with Q8_0 data
    uint8_t q8_data[68];
    for (int i = 0; i < 68; i++) q8_data[i] = (uint8_t)(i * 7);
    
    // Get original signature
    int64_t orig_vx, orig_vy;
    sid_signature_q80(q8_data, 68, &orig_vx, &orig_vy);
    printf("Original: vx=%lld, vy=%lld\n", orig_vx, orig_vy);
    
    // Capture
    SIDCoord coord;
    sid_capture(q8_data, 68, 1, &coord);
    printf("Capture: node_id=%u, resid_x=%lld, resid_y=%lld, drain=%d\n",
           coord.node_id, coord.resid_x, coord.resid_y, coord.drain);
    
    // Summon
    int64_t sum_vx, sum_vy;
    sid_summon(&coord, &sum_vx, &sum_vy);
    printf("Summon: vx=%lld, vy=%lld\n", sum_vx, sum_vy);
    
    // Compare
    printf("Match: %s\n", (orig_vx == sum_vx && orig_vy == sum_vy) ? "YES" : "NO");
    printf("Diff: vx=%lld, vy=%lld\n", orig_vx - sum_vx, orig_vy - sum_vy);
    
    return 0;
}

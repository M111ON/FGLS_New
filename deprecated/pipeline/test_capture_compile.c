#include <stdio.h>
#include <stdint.h>
#include "capture_pipeline.h"

int main(void) {
    printf("capture_pipeline.h compiled OK\n");
    printf("CAPTURE_MAX_TENSORS = %d\n", CAPTURE_MAX_TENSORS);
    printf("TW_REWIND_SLOTS = %d\n", TW_REWIND_SLOTS);
    
    // Test capture init
    CaptureResult cr;
    capture_init(&cr);
    printf("capture_init OK, timestamp: %s\n", cr.timestamp);
    
    // Test tensor capture with dummy data
    uint8_t dummy_tensor[1024];
    for (int i = 0; i < 1024; i++) dummy_tensor[i] = (uint8_t)(i * 37);
    
    int rc = capture_tensor(&cr, dummy_tensor, 1024, 0, 100, 0);
    printf("capture_tensor rc=%d, n_tensors=%u\n", rc, cr.n_tensors_captured);
    
    capture_summary(&cr, stdout);
    
    return 0;
}

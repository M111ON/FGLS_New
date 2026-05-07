/* Minimal sanity test */
#include <stdio.h>
#include <stdint.h>

#define LetterPair_DEFINED
#define CubeNode_DEFINED

#include "geo_letter_cube_patched.h"
#include "lc_twin_gate_patched.h"
#include "tgw_stream_dispatch.h"

int main(void) {
    printf("Headers included successfully\n");
    printf("Testing TGWStreamDispatch struct...\n");
    
    TGWStreamDispatch sd;
    tgw_stream_dispatch_init(&sd);
    printf("  initialized: pkts_rx=%u\n", sd.ss.pkts_rx);
    
    printf("✅ Basic sanity check passed\n");
    return 0;
}

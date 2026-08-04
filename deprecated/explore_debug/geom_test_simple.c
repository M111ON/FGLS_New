#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/kis_geom_codec.h"

int main(void) {
    printf("Testing geom codec...\n");
    
    int8_t w[4] = {1, -1, 2, -2};
    int8_t decoded[4] = {0};
    uint8_t buf[1024];
    
    printf("Encode...\n");
    uint32_t enc = kis_geom_encode(w, 4, buf, sizeof(buf));
    printf("Encoded: %u bytes\n", enc);
    
    printf("Decode...\n");
    int rc = kis_geom_decode(buf, enc, decoded, 4);
    printf("Decode rc: %d\n", rc);
    
    for (int i = 0; i < 4; i++) {
        printf("  %d -> %d\n", w[i], decoded[i]);
    }
    
    return 0;
}

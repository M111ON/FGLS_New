#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/kis_codec_v5.h"

int main(void) {
    /* Small test: 10 weights, trace encode/decode */
    int8_t w[10] = {5, 3, 8, 1, 9, 2, 7, 4, 6, 0};
    uint32_t n = 10;
    
    printf("Original: ");
    for (uint32_t i = 0; i < n; i++) printf("%d ", w[i]);
    printf("\n");
    
    /* Build codebook */
    KIS_V5_Codebook cb;
    kis_v5_codebook_build(&cb, w, n);
    
    /* Sorted */
    int8_t sorted[10];
    kis_v5_codebook_reconstruct(&cb, sorted, n);
    printf("Sorted:   ");
    for (uint32_t i = 0; i < n; i++) printf("%d ", sorted[i]);
    printf("\n");
    
    /* Prefix */
    uint32_t prefix[256] = {0};
    for (int v = 1; v < 256; v++) prefix[v] = prefix[v-1] + cb.histo[v-1];
    
    /* Trace encode */
    uint32_t counter[256] = {0};
    printf("\nEncode trace:\n");
    for (uint32_t i = 0; i < n; i++) {
        uint8_t code = (uint8_t)w[i];
        uint32_t sorted_pos = prefix[code] + counter[code];
        
        uint8_t rot_best = 0;
        int32_t res_best = 0;
        uint32_t abs_best = 0xFFFFFFFF;
        
        for (uint8_t r = 0; r < 36; r++) {
            uint32_t beam = kis_v5_beam_slot(code, r);
            int32_t res = (int32_t)i - (int32_t)beam;
            uint32_t abs_res = (res < 0) ? (uint32_t)(-res) : (uint32_t)res;
            if (abs_res < abs_best) {
                abs_best = abs_res;
                rot_best = r;
                res_best = res;
            }
        }
        printf("  i=%u code=%u sorted_pos=%u beam(rot=%u)=%u residual=%d\n",
               i, code, sorted_pos, rot_best, kis_v5_beam_slot(code, rot_best), res_best);
        counter[code]++;
    }
    
    /* Full roundtrip */
    uint8_t buf[4096];
    uint32_t enc = kis_v5_encode(w, n, buf, sizeof(buf));
    printf("\nEncoded: %u bytes\n", enc);
    
    int8_t out[10] = {0};
    int dec = kis_v5_decode(buf, enc, out, n);
    printf("Decode status: %d\n", dec);
    printf("Output:  ");
    for (uint32_t i = 0; i < n; i++) printf("%d ", out[i]);
    printf("\n");
    
    uint64_t mm = 0;
    for (uint32_t i = 0; i < n; i++) if (w[i] != out[i]) mm++;
    printf("Mismatches: %lu / %u\n", (unsigned long)mm, n);
    
    return 0;
}

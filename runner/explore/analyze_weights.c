#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "beam_addressing/gguf_reader.h"

int main(void) {
    GGUF_File *gf = gguf_open("I:/model/Qwen3-0.6B-Q8_0.gguf");
    if (!gf) return 1;
    
    FILE *fp = fopen("I:/model/Qwen3-0.6B-Q8_0.gguf", "rb");
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    
    printf("=== Qwen3-0.6B Q8_0 Weight Analysis ===\n");
    printf("File: %.1f MB\n\n", fsz/1048576.0);
    
    int q8_count = 0;
    for (uint64_t t = 0; t < gf->tensor_count; t++)
        if (gf->tensors[t].type == 8) q8_count++;
    printf("Tensors: %lu total, %d Q8_0\n\n", gf->tensor_count, q8_count);
    
    uint64_t phase[4] = {0, 0, 0, 0};
    uint64_t total = 0;
    uint64_t probe_dist[16] = {0};
    
    for (uint64_t t = 0; t < gf->tensor_count; t++) {
        if (gf->tensors[t].type != 8) continue;
        uint64_t sz = gf->tensors[t].size_bytes;
        if (sz < 34) continue;
        
        uint64_t off = gf->tensor_data_start + gf->tensors[t].offset;
        uint64_t n_q8 = sz / 34;
        
        for (uint64_t qb = 0; qb < n_q8; qb++) {
            uint8_t blk[34];
            fseek(fp, off + qb * 34, SEEK_SET);
            fread(blk, 1, 34, fp);
            
            for (int i = 2; i < 34; i++) {
                int8_t w = (int8_t)blk[i];
                if (w > -8 && w < 8) { phase[0]++; probe_dist[w+8]++; }
                else if (w > 0) phase[1]++;
                else if (w >= -32) phase[2]++;
                else phase[3]++;
                total++;
            }
        }
    }
    
    printf("Phase distribution:\n");
    printf("  PROBE  (|w|<8):  %8lu  (%5.1f%%)\n", phase[0], 100.0*phase[0]/total);
    printf("  MAIN   (w>0):   %8lu  (%5.1f%%)\n", phase[1], 100.0*phase[1]/total);
    printf("  MIRROR (w>=-32):%8lu  (%5.1f%%)\n", phase[2], 100.0*phase[2]/total);
    printf("  CANCEL (w<-32): %8lu  (%5.1f%%)\n", phase[3], 100.0*phase[3]/total);
    printf("  TOTAL:          %8lu\n\n", total);
    
    printf("PROBE values (|w|<8) — background noise:\n");
    for (int i = 0; i < 16; i++)
        printf("  w=%+d: %8lu  (%5.1f%% of PROBE)\n", i-8, probe_dist[i], 
               100.0*probe_dist[i]/phase[0]);
    
    printf("\n=== KEY QUESTION: PROBE is %.1f%% of all weights ===\n", 100.0*phase[0]/total);
    printf("If PROBE = background noise (random, no pattern), it's NOT compressible.\n");
    printf("If PROBE = structured (many zeros, repeated values), it IS compressible.\n");
    
    fclose(fp);
    gguf_close(gf);
    return 0;
}

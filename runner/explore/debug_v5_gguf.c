#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gguf_reader.h"
#include "core/kis_codec_v5.h"

int main(void) {
    const char *path = "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf";
    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("Cannot open\n"); return 1; }
    
    int tidx = -1;
    for (uint64_t i = 0; i < gf->tensor_count; i++)
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) { tidx = (int)i; break; }
    
    GGUF_Tensor *t = &gf->tensors[tidx];
    uint32_t n = 100000;
    printf("Tensor: %s (%u weights)\n", t->name, n);
    
    int8_t *raw = (int8_t *)malloc(n);
    uint64_t foff = gf->tensor_data_start + t->offset;
    foff = (foff + 31) & ~(uint64_t)31;
    fseek(gf->fp, (long)foff, SEEK_SET);
    uint32_t rd = 0;
    uint64_t nblk = (t->n_weights + 31) / 32;
    for (uint64_t b = 0; b < nblk && rd < n; b++) {
        uint16_t scale; int8_t w[32];
        if (fread(&scale,2,1,gf->fp) != 1) break;
        if (fread(w,1,32,gf->fp) != 32) break;
        for (int i = 0; i < 32 && rd < n; i++) raw[rd++] = w[i];
    }
    gguf_close(gf);
    
    printf("Read %u weights\n", rd);
    
    uint32_t buf_size = n * 5;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    uint32_t enc = kis_v5_encode(raw, rd, buf, buf_size);
    printf("Encoded: %u bytes ratio=%.2fx\n", enc, (double)enc/rd);
    
    int8_t *out = (int8_t *)calloc(rd, 1);
    int dec = kis_v5_decode(buf, enc, out, rd);
    printf("Decode status: %d\n", dec);
    
    uint64_t mm = 0;
    uint32_t first_mm = 0;
    for (uint32_t i = 0; i < rd; i++) {
        if (raw[i] != out[i]) {
            if (mm == 0) first_mm = i;
            mm++;
        }
    }
    printf("Mismatches: %lu / %u\n", (unsigned long)mm, rd);
    if (mm > 0) {
        printf("First at %u: expected %d got %d\n", first_mm, raw[first_mm], out[first_mm]);
        for (uint32_t i = first_mm; i < first_mm + 10 && i < rd; i++) {
            printf("  [%u] exp=%d got=%d\n", i, raw[i], out[i]);
        }
    }
    
    free(raw); free(buf); free(out);
    return 0;
}

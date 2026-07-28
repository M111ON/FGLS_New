/*
 * gguf_tensor_info.c — Dump tensor list using proven gguf_reader.h
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* Include the proven GGUF reader */
#include "../beam_addressing/gguf_reader.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("Usage: gguf_info <file.gguf>\n"); return 1; }
    
    GGUF_File *gf = gguf_open(argv[1]);
    if (!gf) { fprintf(stderr, "Failed to open %s\n", argv[1]); return 1; }
    
    printf("Version: %u  Tensors: %lu  KV pairs: %lu\n", 
           gf->version, (unsigned long)gf->tensor_count, (unsigned long)gf->kv_count);
    
    printf("\n%-50s %12s %12s %8s\n", "Tensor", "Elements", "Bytes", "MB");
    printf("──────────────────────────────────────────────────────────────────────────────\n");
    
    uint64_t total_bytes = 0;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        GGUF_Tensor *t = &gf->tensors[i];
        total_bytes += t->size_bytes;
        
        char dim_str[64] = "";
        if (t->n_dims == 1) sprintf(dim_str, "%lu", (unsigned long)t->dims[0]);
        else if (t->n_dims == 2) sprintf(dim_str, "%lux%lu", (unsigned long)t->dims[0], (unsigned long)t->dims[1]);
        else if (t->n_dims == 3) sprintf(dim_str, "%lux%lux%lu", (unsigned long)t->dims[0], (unsigned long)t->dims[1], (unsigned long)t->dims[2]);
        else if (t->n_dims == 4) sprintf(dim_str, "%lux%lux%lux%lu", (unsigned long)t->dims[0], (unsigned long)t->dims[1], (unsigned long)t->dims[2], (unsigned long)t->dims[3]);
        
        const char *tn = "?";
        switch(t->type) { 
            case GGML_TYPE_F32: tn="F32"; break; 
            case GGML_TYPE_F16: tn="F16"; break;
            case GGML_TYPE_Q4_0: tn="Q4_0"; break; 
            case GGML_TYPE_Q8_0: tn="Q8_0"; break;
            case GGML_TYPE_Q4_1: tn="Q4_1"; break;
            case GGML_TYPE_Q8_1: tn="Q8_1"; break;
        }
        
        printf("%-50s %12lu %12lu %7.1f  [%s %s]\n", 
               t->name, (unsigned long)t->n_weights, (unsigned long)t->size_bytes, 
               t->size_bytes/1e6, tn, dim_str);
    }
    
    printf("──────────────────────────────────────────────────────────────────────────────\n");
    printf("%-50s %12s %12lu %7.1f\n", "TOTAL", "", (unsigned long)total_bytes, total_bytes/1e6);
    
    gguf_close(gf);
    return 0;
}

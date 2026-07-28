#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../beam_addressing/gguf_reader.h"
int main() {
    GGUF_File *gf = gguf_open("I:/model/Qwen3-0.6B-Q4_0.gguf");
    if (!gf) return 1;
    
    /* Print all tensor types */
    for (uint64_t i = 0; i < gf->tensor_count && i < 15; i++) {
        GGUF_Tensor *t = &gf->tensors[i];
        printf("  [%lu] %-30s type=%u  dims=", i, t->name, t->type);
        for (uint32_t d = 0; d < t->n_dims; d++) printf("%s%lu", d?",":"", (unsigned long)t->dims[d]);
        printf("  size_bytes=%lu  n_weights=%lu\n", (unsigned long)t->size_bytes, (unsigned long)t->n_weights);
    }
    
    /* Find token_embd and check its offset vs next tensor */
    int emb = gguf_find_tensor(gf, "token_embd");
    int blk0k = gguf_find_tensor(gf, "blk.0.attn_k");
    if (emb >= 0 && blk0k >= 0) {
        GGUF_Tensor *te = &gf->tensors[emb];
        GGUF_Tensor *tk = &gf->tensors[blk0k];
        printf("\ntoken_embd offset=%lu, blk.0.attn_k offset=%lu\n",
               (unsigned long)te->offset, (unsigned long)tk->offset);
        printf("Actual gap = %lu bytes = %.1f MB\n",
               (unsigned long)(tk->offset - te->offset), (tk->offset - te->offset)/1e6);
    }
    
    gguf_close(gf);
    return 0;
}

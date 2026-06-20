#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "llama.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s model.gguf\n", argv[0]); return 1; }
    
    llama_backend_init();

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) { fprintf(stderr, "model load fail\n"); return 1; }
    fprintf(stderr, "model loaded\n");
    
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    fprintf(stderr, "vocab = %p\n", (void*)vocab);
    
    /* Try all param combinations for tokenize */
    const char *prompt = "Hello";
    int len = (int)strlen(prompt);
    
    for (int add = 0; add <= 1; add++) {
        for (int parse = 0; parse <= 1; parse++) {
            int nt = llama_tokenize(vocab, prompt, len, NULL, 0, add, parse);
            fprintf(stderr, "  add=%d parse=%d → %d\n", add, parse, nt);
            
            if (nt > 0) {
                llama_token *toks = (llama_token*)malloc(nt * sizeof(llama_token));
                nt = llama_tokenize(vocab, prompt, len, toks, nt, add, parse);
                fprintf(stderr, "    fill=%d: ", nt);
                for (int i = 0; i < nt && i < 10; i++) fprintf(stderr, "%d ", toks[i]);
                if (nt > 10) fprintf(stderr, "...");
                fprintf(stderr, "\n");
                free(toks);
            }
        }
    }
    
    llama_model_free(model);
    llama_backend_free();
    return 0;
}

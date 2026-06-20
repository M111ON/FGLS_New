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
    
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    
    /* Test: return value convention */
    const char *text = "Hello";
    int text_len = (int)strlen(text);
    
    int nt = llama_tokenize(vocab, text, text_len, NULL, 0, true, false);
    fprintf(stderr, "raw=%d\n", nt);
    
    /* In b9733, negative return = -needed_count */
    if (nt < 0) {
        int need = -nt;  /* number of tokens needed */
        fprintf(stderr, "need=%d tokens\n", need);
        
        /* Allocate +1 for safety */
        int *toks = (int*)malloc((need + 4) * sizeof(int));
        int got = llama_tokenize(vocab, text, text_len, toks, need + 4, true, false);
        fprintf(stderr, "got=%d tokens: ", got);
        for (int i = 0; i < got; i++) fprintf(stderr, "%d ", toks[i]);
        fprintf(stderr, "\n");
        free(toks);
    }
    
    /* Test "a" */
    nt = llama_tokenize(vocab, "a", 1, NULL, 0, true, false);
    fprintf(stderr, "'a' raw=%d need=%d\n", nt, nt < 0 ? -nt : nt);
    
    /* Test empty-ish */
    nt = llama_tokenize(vocab, "!", 1, NULL, 0, true, false);
    fprintf(stderr, "'!' raw=%d need=%d\n", nt, nt < 0 ? -nt : nt);
    
    /* Test single space */
    nt = llama_tokenize(vocab, " ", 1, NULL, 0, true, false);
    fprintf(stderr, "' ' raw=%d need=%d\n", nt, nt < 0 ? -nt : nt);
    
    /* Test long text */
    nt = llama_tokenize(vocab, "Hello world this is a test of the tokenizer", 46, NULL, 0, true, false);
    fprintf(stderr, "long raw=%d need=%d\n", nt, nt < 0 ? -nt : nt);
    
    llama_model_free(model);
    llama_backend_free();
    return 0;
}

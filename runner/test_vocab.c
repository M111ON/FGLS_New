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
    fprintf(stderr, "vocab = %p\n", (void*)vocab);
    
    int n_vocab = llama_vocab_n_tokens(vocab);
    fprintf(stderr, "n_vocab = %d\n", n_vocab);
    
    fprintf(stderr, "add_bos = %d\n", llama_vocab_get_add_bos(vocab));
    fprintf(stderr, "bos_id = %d\n", llama_vocab_bos(vocab));
    fprintf(stderr, "eos_id = %d\n", llama_vocab_eos(vocab));
    
    /* Check a few token texts to verify vocab OK */
    for (int id = 0; id < 5 && id < n_vocab; id++) {
        const char *t = llama_vocab_get_text(vocab, id);
        fprintf(stderr, "  token %d: '%s'\n", id, t ? t : "(null)");
    }
    
    /* Try tokenizing with the same signature as the runner uses */
    const char *text = "Hello";
    int text_len = (int)strlen(text);
    fprintf(stderr, "\ntokenizing '%s' with text_len=%d\n", text, text_len);
    
    /* Try as int, not bool */
    int nt = llama_tokenize(vocab, text, text_len, NULL, 0, 1, 0);
    fprintf(stderr, "int(b1,p0): %d\n", nt);
    
    nt = llama_tokenize(vocab, text, text_len, NULL, 0, 0, 0);
    fprintf(stderr, "int(b0,p0): %d\n", nt);
    
    /* If got count, fill */
    for (int trial = 0; trial < 2; trial++) {
        int add = trial == 0 ? 1 : 0;
        nt = llama_tokenize(vocab, text, text_len, NULL, 0, add, 0);
        if (nt > 0) {
            int *toks = (int*)malloc(nt * 4);
            nt = llama_tokenize(vocab, text, text_len, toks, nt, add, 0);
            fprintf(stderr, "add=%d fill: ", add);
            for (int i = 0; i < nt; i++) fprintf(stderr, "%d ", toks[i]);
            fprintf(stderr, "\n");
            free(toks);
            break;
        }
    }
    
    llama_model_free(model);
    llama_backend_free();
    return 0;
}

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
    
    /* Tokenize with explicit int32_t casting */
    const char *text = "Hello";
    int text_len = (int)strlen(text);
    
    /* Try using the DEPRECATED function instead */
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    
    int nt;
    
    /* Maybe the issue is that the tokenizer needs to be initialized with a context */
    /* Let's create a context and try */
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 64;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "context fail\n"); return 1; }
    fprintf(stderr, "context OK\n");
    
    /* Try tokenize again after context creation */
    nt = llama_tokenize(vocab, text, text_len, NULL, 0, true, false);
    fprintf(stderr, "tokenize after ctx: %d\n", nt);
    
    /* Try with context - maybe there's a newer API */
    /* Some versions have llama_tokenize that works on context */
    
    /* OK let's try detokenize a known token to verify roundtrip */
    llama_token test_tok = 151643;  /* BOS */
    char buf[256];
    int n = llama_token_to_piece(vocab, test_tok, buf, 256, 0, true);
    fprintf(stderr, "detokenize %d: n=%d '%.*s'\n", test_tok, n, n > 0 ? n : 0, buf);
    
    test_tok = 198;  /* LF */
    n = llama_token_to_piece(vocab, test_tok, buf, 256, 0, true);
    fprintf(stderr, "detokenize %d: n=%d '%.*s'\n", test_tok, n, n > 0 ? n : 0, buf);
    
    /* Try the deprecated llama_tokenize with context */
    /* Actually the deprecation was for other functions */
    /* Let me check if llama_tokenize needs parse_special for BPE */
    
    /* Try with explicit flags */
    for (int parse = 0; parse <= 1; parse++) {
        nt = llama_tokenize(vocab, "Hello", 5, NULL, 0, false, parse);
        fprintf(stderr, "parse=%d: %d\n", parse, nt);
    }
    
    /* Try tokenizing the exact bytes of a known token */
    /* Token 0 is '!' */
    n = llama_token_to_piece(vocab, 0, buf, 256, 0, false);
    fprintf(stderr, "token 0 -> '%s' (n=%d)\n", buf, n);
    nt = llama_tokenize(vocab, "!", 1, NULL, 0, false, false);
    fprintf(stderr, "tokenize '!': %d\n", nt);
    
    #pragma GCC diagnostic pop
    
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}

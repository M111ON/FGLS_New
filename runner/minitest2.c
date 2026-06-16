#include <stdio.h>
#include <windows.h>
#include "llama.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model>\n", argv[0]);
        return 1;
    }
    const char *model_path = argv[1];
    fprintf(stderr, "[minitest] llama_backend_init...\n");
    llama_backend_init();
    fprintf(stderr, "[minitest] loading model...\n");
    fflush(stderr);
    
    struct llama_model_params mp = llama_model_default_params();
    mp.vocab_only = false;
    mp.use_mmap = true;
    
    fprintf(stderr, "[minitest] calling llama_model_load_from_file...\n");
    fflush(stderr);
    
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    
    fprintf(stderr, "[minitest] model=%p\n", (void*)model);
    fflush(stderr);
    
    if (model) {
        fprintf(stderr, "[minitest] loading context...\n");
        struct llama_context_params cp = llama_context_default_params();
        cp.n_ctx = 512;
        struct llama_context *ctx = llama_new_context_with_model(model, cp);
        fprintf(stderr, "[minitest] ctx=%p\n", (void*)ctx);
        if (ctx) llama_free(ctx);
        llama_model_free(model);
    } else {
        fprintf(stderr, "[minitest] model was NULL\n");
    }
    
    fprintf(stderr, "[minitest] done\n");
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "llama.h"
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"Usage...\n");return 1;}
    fprintf(stderr,"--- Starting ---\n");
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    fprintf(stderr,"--- Backend init done ---\n");
    struct llama_model_params mp=llama_model_default_params();
    fprintf(stderr,"--- Loading model... ---\n");
    struct llama_model*model=llama_model_load_from_file(argv[1],mp);
    if(!model){fprintf(stderr,"ERROR: model load\n");llama_backend_free();return 1;}
    fprintf(stderr,"--- Model loaded OK ---\n");
    llama_model_free(model);llama_backend_free();
    fprintf(stderr,"--- Done ---\n");
    return 0;
}

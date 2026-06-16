#include <stdio.h>
#include <windows.h>
#include "llama.h"
int main(int argc,char**argv){
    if(argc<2)return 1;
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model*model=llama_model_load_from_file(argv[1],llama_model_default_params());
    fprintf(stderr,"[A] model loaded\n");fflush(stderr);
    llama_model_free(model);
    llama_backend_free();
    fprintf(stderr,"[A] done\n");fflush(stderr);
    return 0;
}

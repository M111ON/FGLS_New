// test_math_h.c — does #include <math.h> break model load?
#include <stdio.h>
#include <math.h>
#include <windows.h>
#include "llama.h"
int main(int argc,char**argv){
    if(argc<2)return 1;
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model*model=llama_model_load_from_file(argv[1],llama_model_default_params());
    fprintf(stderr,"[M] model=%p\n",(void*)model);fflush(stderr);
    if(model)llama_model_free(model);
    llama_backend_free();
    return 0;
}

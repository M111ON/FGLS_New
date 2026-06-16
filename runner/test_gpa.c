#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "llama.h"

typedef int (*sid_fn)(void*);

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"Usage...\n");return 1;}
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model_params mp=llama_model_default_params();
    struct llama_model*model=llama_model_load_from_file(argv[1],mp);
    if(!model){fprintf(stderr,"ERROR: model load\n");llama_backend_free();return 1;}
    fprintf(stderr,"Model loaded OK\n");

    HMODULE h=LoadLibraryA("kernel32.dll");
    if(h){
        sid_fn c = (sid_fn)GetProcAddress(h,"GetModuleHandleA");
        fprintf(stderr,"GPA: %p\n", c);
        FreeLibrary(h);
    }

    llama_model_free(model);llama_backend_free();
    return 0;
}

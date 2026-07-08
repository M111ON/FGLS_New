#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "llama.h"

typedef int (*sid_fn_search)(void*, const char*);

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"Usage...\n");return 1;}
    llama_backend_init();
    ggml_backend_load("I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-sse42.dll");
    struct llama_model_params mp=llama_model_default_params();
    struct llama_model*model=llama_model_load_from_file(argv[1],mp);
    if(!model){fprintf(stderr,"ERROR: model load\n");llama_backend_free();return 1;}

    // Search for "blk." in model struct and nearby heap
    const uint8_t *base = (const uint8_t*)model;
    MEMORY_BASIC_INFORMATION mbi;

    // Search model struct itself (2048 bytes)
    for (size_t off = 0; off < 2048 - 5; off++) {
        if (base[off]=='b' && base[off+1]=='l' && base[off+2]=='k' && base[off+3]=='.') {
            fprintf(stderr, "[search] 'blk.' at model+%zu: offset %zu\n", off, off);
        }
    }

    // For each pointer in model struct, scan in detail
    for (int off = 0; off < 2048; off += 8) {
        void *val; memcpy(&val, base + off, sizeof(val));
        if (!val || (uintptr_t)val < 0x10000 || (uintptr_t)val > 0x7FFFFFFF0000ULL) continue;
        if (!VirtualQuery(val, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) continue;
        if (!(mbi.Protect & (PAGE_READONLY|PAGE_READWRITE))) continue;
        
        const uint8_t *scan_base = (const uint8_t*)val;
        size_t max_safe = 4096 < mbi.RegionSize ? 4096 : mbi.RegionSize;
        
        for (size_t i = 0; i < max_safe - 40; i += 1) {
            const char *s = (const char*)(scan_base + i);
            // Look for tensor-specific patterns (blk.X.xxx)
            if (!(s[0]=='b' && s[1]=='l' && s[2]=='k' && s[3]=='.' &&
                  s[4] >= '0' && s[4] <= '9')) continue;
            fprintf(stderr, "[search] at model+%2d heap %p +%zu: '", off, val, i);
            for (int j = 0; j < 40 && s[j] >= 32 && s[j] < 127; j++) fprintf(stderr, "%c", s[j]);
            fprintf(stderr, "'\n");
            // Now check backwards for tensor struct base
            // The full tensor name string starts at i in the string pool
            // But where is the tensor struct?
            // It should have a 'data' pointer that points to mmap'd data.
            // Let me look for the mgic offset where the first tensor's full name matches
        }
    }

    fprintf(stderr, "[search] done\n");
    llama_model_free(model);llama_backend_free();
    return 0;
}

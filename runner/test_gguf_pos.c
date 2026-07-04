/* Test: read GGUF tensor info and report pos_after_tensors */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GGUF_ALIGNMENT 32
#define GGUF_MAGIC_LOCAL 0x46554747u

#include "gguf_index.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    GGUFTensorIndex idx;
    memset(&idx, 0, sizeof(idx));
    
    const char *path = "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open GGUF\n"); return 1; }
    
    uint32_t magic, version;
    uint64_t n_kv;
    fread(&magic,4,1,f);
    fread(&version,4,1,f);
    fread(&idx.n_tensors,8,1,f);
    fread(&n_kv,8,1,f);
    printf("GGUF: magic=0x%08x ver=%u n_tensors=%llu n_kv=%llu\n",
           magic, version,
           (unsigned long long)idx.n_tensors,
           (unsigned long long)n_kv);
    
    /* Skip KV pairs */
    for (uint64_t i = 0; i < n_kv; i++) {
        uint64_t klen; fread(&klen,8,1,f); fseek(f,klen,SEEK_CUR);
        uint32_t vtype; fread(&vtype,4,1,f);
        switch (vtype) {
            case 0:case 1: fseek(f,1,SEEK_CUR);break;
            case 2:case 3: fseek(f,2,SEEK_CUR);break;
            case 4:case 5:case 6: fseek(f,4,SEEK_CUR);break;
            case 7: fseek(f,1,SEEK_CUR);break;
            case 10:case 11:case 12: fseek(f,8,SEEK_CUR);break;
            case 8:{uint64_t sl;fread(&sl,8,1,f);fseek(f,sl,SEEK_CUR);break;}
            case 9:{uint32_t at;uint64_t al;fread(&at,4,1,f);fread(&al,8,1,f);
                size_t es=0; switch(at){
                    case 0:case 1:es=1;break;case 2:case 3:es=2;break;
                    case 4:case 5:case 6:es=4;break;case 7:es=1;break;
                    case 10:case 11:es=8;break;
                    case 12:es=8;break;
                    case 8:for(uint64_t j=0;j<al;j++){uint64_t sl;fread(&sl,8,1,f);fseek(f,sl,SEEK_CUR);}continue;
                    default:es=4;}fseek(f,es*al,SEEK_CUR);break;}
            default: fseek(f,8,SEEK_CUR);break;
        }
    }
    
    long pos_kv = ftell(f);
    printf("pos_after_kv = %ld\n", pos_kv);
    
    /* Read tensor info */
    long prev = pos_kv;
    
    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        uint64_t klen; fread(&klen,8,1,f);
        char *name = malloc(klen+1); fread(name,1,klen,f); name[klen]=0; free(name);
        uint32_t nd; fread(&nd,4,1,f);
        int64_t ne = 1;
        for (uint32_t j = 0; j < nd; j++) {
            int64_t dim; fread(&dim,8,1,f);
            ne *= dim;
        }
        uint32_t dt; fread(&dt,4,1,f);
        int64_t off; fread(&off,8,1,f);
        
        long pos = ftell(f);
        long entry_bytes = pos - prev;
        
        /* Print first 3 and last 1 */
        if (i < 3 || i == idx.n_tensors - 1 || pos > prev + 100) {
            printf("  [%llu] nd=%u entry=%ld bytes (cum=%ld)\n",
                   (unsigned long long)i, nd, entry_bytes, pos - pos_kv);
        }
        
        prev = pos;
    }
    
    long pos_after = ftell(f);
    printf("\npos_after_tensors = %ld\n", pos_after);
    
    /* Compute data_sec_off */
    uint64_t align_mod = (uint64_t)pos_after % GGUF_ALIGNMENT;
    uint64_t data_sec_off = (align_mod == 0) ? (uint64_t)pos_after
                                             : (uint64_t)pos_after + (GGUF_ALIGNMENT - align_mod);
    printf("align_mod = %llu\n", (unsigned long long)align_mod);
    printf("data_sec_off (C formula) = %llu\n", (unsigned long long)data_sec_off);
    printf("aligned DOWN = %llu\n", (unsigned long long)((uint64_t)pos_after / 32 * 32));
    
    /* Compare with Python result */
    printf("\nExpected (Python pos_after): 5947741\n");
    printf("C pos_after: %ld (diff=%ld)\n", pos_after, pos_after - 5947741L);
    
    fclose(f);
    return 0;
}

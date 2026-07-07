#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
int main() {
    FILE *f = fopen("I:/model/LFM2.5-8B-A1B-Q4_K_M.gguf", "rb");
    if (!f) return 1;
    uint32_t magic, ver; fread(&magic,4,1,f); fread(&ver,4,1,f);
    uint64_t n_t, n_kv; fread(&n_t,8,1,f); fread(&n_kv,8,1,f);
    printf("tensors=%llu kv=%llu\n", (unsigned long long)n_t, (unsigned long long)n_kv);
    /* skip KV */
    for (uint64_t k = 0; k < n_kv; k++) {
        uint64_t klen; fread(&klen,8,1,f); fseek(f, (long)klen, SEEK_CUR);
        uint32_t vtype; fread(&vtype,4,1,f);
        if (vtype == 9) {
            uint32_t at; uint64_t an; fread(&at,4,1,f); fread(&an,8,1,f);
            static const uint8_t esz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
            if (at == 8) { for (uint64_t a = 0; a < an; a++) { uint64_t sl; fread(&sl,8,1,f); fseek(f,(long)sl,SEEK_CUR); } }
            else fseek(f, (long)(esz[at]*an), SEEK_CUR);
        } else {
            switch(vtype) {
                case 0:case 1:case 7: fseek(f,1,SEEK_CUR); break;
                case 2:case 3: fseek(f,2,SEEK_CUR); break;
                case 4:case 5:case 6: fseek(f,4,SEEK_CUR); break;
                case 10:case 11:case 12: fseek(f,8,SEEK_CUR); break;
                case 8: { uint64_t sl; fread(&sl,8,1,f); fseek(f,(long)sl,SEEK_CUR); break; }
            }
        }
    }
    printf("after KV: pos=%ld\n", ftell(f));
    /* read tensor info */
    for (uint64_t i = 0; i < n_t && i < 20; i++) {
        uint64_t nlen; fread(&nlen,8,1,f);
        char name[256]; fread(name,(size_t)nlen,1,f); name[nlen]=0;
        uint32_t nd; fread(&nd,4,1,f);
        int64_t dims[4]={1,1,1,1};
        for (uint32_t d=0;d<nd;d++) fread(&dims[d],8,1,f);
        uint32_t dtype; fread(&dtype,4,1,f);
        uint64_t off; fread(&off,8,1,f);
        static const char *dtn[] = {"F32","F16","Q4_0","Q4_1","?","?","Q5_0","Q5_1","Q8_0","Q8_1","Q2_K","Q3_K","Q4_K","Q5_K","Q6_K","Q8_K","IQ2_XXS","IQ2_XS","IQ3_XXS","IQ1_S","IQ4_NL","IQ3_S","IQ2_S","IQ4_XS","I8","I16","I32","I64","F64","IQ1_M","BF16"};
        printf("%s: nd=%u dims=(%lld,%lld,%lld,%lld) dtype=%u(%s) off=%llu\n",
               name, nd, (long long)dims[0], (long long)dims[1], (long long)dims[2], (long long)dims[3],
               dtype, dtype<31?dtn[dtype]:"?", (unsigned long long)off);
    }
    fclose(f);
    return 0;
}

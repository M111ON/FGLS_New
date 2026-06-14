#ifndef GGUF_INDEX_H
#define GGUF_INDEX_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GGUF_MAGIC_LOCAL 0x46554747u
#define GGUF_F32         0u
#define GGUF_F16         1u
#define GGUF_Q8_0        8u

typedef struct {
    uint64_t n_tensors;
    char   **names;
    uint32_t *dtypes;
    uint64_t *offsets;
    uint64_t *sizes;
} GGUFTensorIndex;

static int cmp_u64_desc(const void *a, const void *b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return (x > y) ? -1 : (x < y) ? 1 : 0;
}

static int gguf_idx_open(const char *path, GGUFTensorIndex *idx) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    memset(idx, 0, sizeof(*idx));

    uint32_t magic, version;
    uint64_t n_kv;
    if (fread(&magic,4,1,f)!=1||fread(&version,4,1,f)!=1||
        fread(&idx->n_tensors,8,1,f)!=1||fread(&n_kv,8,1,f)!=1)
        { fclose(f); return -1; }
    if (magic != GGUF_MAGIC_LOCAL) { fclose(f); return -1; }

    for (uint64_t i = 0; i < n_kv; i++) {
        uint64_t klen; fread(&klen,8,1,f); fseek(f,klen,SEEK_CUR);
        uint32_t vtype; fread(&vtype,4,1,f);
        switch (vtype) {
            case 0:case 1: fseek(f,1,SEEK_CUR);break;
            case 2:case 3: fseek(f,2,SEEK_CUR);break;
            case 4:case 5:case 6:case 10:case 11: fseek(f,4,SEEK_CUR);break;
            case 7: fseek(f,1,SEEK_CUR);break;
            case 12: fseek(f,8,SEEK_CUR);break;
            case 8:{uint64_t sl;fread(&sl,8,1,f);fseek(f,sl,SEEK_CUR);break;}
            case 9:{uint32_t at;uint64_t al;fread(&at,4,1,f);fread(&al,8,1,f);
                size_t es=0; switch(at){
                    case 0:case 1:es=1;break;case 2:case 3:es=2;break;
                    case 4:case 5:case 6:es=4;break;case 7:es=1;break;
                    case 8:for(uint64_t j=0;j<al;j++){uint64_t sl;fread(&sl,8,1,f);fseek(f,sl,SEEK_CUR);}continue;
                    default:es=4;}fseek(f,es*al,SEEK_CUR);break;}
            default: fseek(f,4,SEEK_CUR);
        }
    }

    uint64_t n = idx->n_tensors;
    idx->names=(char**)calloc(n,sizeof(char*));
    idx->dtypes=(uint32_t*)calloc(n,sizeof(uint32_t));
    idx->offsets=(uint64_t*)calloc(n,sizeof(uint64_t));
    idx->sizes=(uint64_t*)calloc(n,sizeof(uint64_t));

    for (uint64_t i = 0; i < n; i++) {
        uint64_t klen; fread(&klen,8,1,f);
        idx->names[i]=(char*)calloc(klen+1,1);
        fread(idx->names[i],1,klen,f);
        uint32_t nd; fread(&nd,4,1,f); fseek(f,nd*8,SEEK_CUR);
        fread(&idx->dtypes[i],4,1,f);
        fread(&idx->offsets[i],8,1,f);
    }
    idx->n_tensors = n;

    uint64_t file_end; fseek(f,0,SEEK_END); file_end=ftell(f); fclose(f);

    uint64_t *sorted=(uint64_t*)calloc(n*2,sizeof(uint64_t));
    for (uint64_t i = 0; i < n; i++) { sorted[i]=idx->offsets[i]; sorted[n+i]=i; }
    qsort(sorted,n,sizeof(uint64_t),cmp_u64_desc);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t off=sorted[i];
        uint64_t ti;
        for (ti=0;ti<n;ti++) if(idx->offsets[ti]==off) break;
        sorted[i]=ti;
    }
    for (uint64_t i = 0; i < n; i++) {
        uint64_t ti=sorted[i];
        uint64_t nd; if(i+1<n) nd=idx->offsets[sorted[i+1]]; else nd=file_end;
        int64_t d=(int64_t)(nd-idx->offsets[ti]);
        idx->sizes[ti]=(d>0)?(uint64_t)d:file_end-idx->offsets[ti];
    }
    free(sorted);
    return 0;
}

static void gguf_idx_close(GGUFTensorIndex *idx) {
    for (uint64_t i = 0; i < idx->n_tensors; i++) free(idx->names[i]);
    free(idx->names); free(idx->dtypes); free(idx->offsets); free(idx->sizes);
    memset(idx,0,sizeof(*idx));
}

#endif

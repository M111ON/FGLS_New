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

#define GGUF_ALIGNMENT 32

typedef struct {
    uint64_t n_tensors;
    char   **names;
    uint32_t *dtypes;
    uint64_t *offsets;      /* absolute file offset of tensor data */
    uint64_t *sizes;
    uint64_t  data_sec_off; /* file offset of tensor data section start */
} GGUFTensorIndex;

/* return absolute file offset for tensor i */
static inline uint64_t gguf_idx_tensor_abs_offset(const GGUFTensorIndex *idx, uint64_t i) {
    return idx->data_sec_off + idx->offsets[i];
}

static size_t ggi_type_size(uint32_t t) {
    switch(t) {
        case 0: return 4;    /* F32   */
        case 1: return 2;    /* F16   */
        case 2: return 18;   /* Q4_0  */
        case 3: return 20;   /* Q4_1  */
        case 6: return 22;   /* Q5_0  */
        case 7: return 24;   /* Q5_1  */
        case 8: return 34;   /* Q8_0  */
        case 9: return 36;   /* Q8_1  */
        case 10: return 84;  /* Q2_K  */
        case 11: return 110; /* Q3_K  */
        case 12: return 144; /* Q4_K  */
        case 13: return 176; /* Q5_K  */
        case 14: return 210; /* Q6_K  */
        case 15: return 292; /* Q8_K  */
        default: return 4;
    }
}
static int ggi_block_size(uint32_t t) {
    switch(t) {
        case 2: case 3: case 6: case 7: case 8: case 9: return 32;
        case 10: case 11: case 12: case 13: case 14: case 15: return 256;
        default: return 1;
    }
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

    uint64_t n = idx->n_tensors;
    idx->names=(char**)calloc(n,sizeof(char*));
    idx->dtypes=(uint32_t*)calloc(n,sizeof(uint32_t));
    idx->offsets=(uint64_t*)calloc(n,sizeof(uint64_t));
    idx->sizes=(uint64_t*)calloc(n,sizeof(uint64_t));

    for (uint64_t i = 0; i < n; i++) {
        uint64_t klen; fread(&klen,8,1,f);
        idx->names[i]=(char*)calloc(klen+1,1);
        fread(idx->names[i],1,klen,f);
        uint32_t nd; fread(&nd,4,1,f);
        uint64_t ndim[4]; uint64_t ne=1;
        for(uint32_t j=0;j<nd;j++){fread(&ndim[j],8,1,f);ne*=ndim[j];}
        fread(&idx->dtypes[i],4,1,f);
        fread(&idx->offsets[i],8,1,f);
        uint32_t dt=idx->dtypes[i];
        size_t ts=ggi_type_size(dt);
        int bs=ggi_block_size(dt);
        idx->sizes[i]=(ne/bs)*ts;
    }
    idx->n_tensors = n;

    // compute tensor data section offset (after all tensor info entries, 32-byte aligned)
    long pos_after_tensors = ftell(f);
    uint64_t align_mod = (uint64_t)pos_after_tensors % GGUF_ALIGNMENT;
    idx->data_sec_off = (align_mod == 0) ? (uint64_t)pos_after_tensors
                                         : (uint64_t)pos_after_tensors + (GGUF_ALIGNMENT - align_mod);

    fclose(f);
    return 0;
}

static void gguf_idx_close(GGUFTensorIndex *idx) {
    for (uint64_t i = 0; i < idx->n_tensors; i++) free(idx->names[i]);
    free(idx->names); free(idx->dtypes); free(idx->offsets); free(idx->sizes);
    memset(idx,0,sizeof(*idx));
}

#endif

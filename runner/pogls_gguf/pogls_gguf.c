#include "pogls_gguf.h"

static const struct { uint16_t tsz; uint16_t blck; } g_tinfo[31] = {
    {4,   1},   {2,   1},   {18,  32},  {20,  32},
    {0,   0},   {0,   0},   {22,  32},  {24,  32},
    {34,  32},  {36,  32},  {84,  256}, {110, 256},
    {144, 256}, {176, 256}, {210, 256}, {292, 256},
    {2,   256}, {2,   256}, {2,   256}, {1,   256},
    {2,   32},  {1,   256}, {1,   256}, {2,   256},
    {1,   1},   {2,   1},   {4,   1},   {8,   1},
    {8,   1},   {1,   256}, {2,   1},
};

size_t pogls_gguf_type_size(uint32_t dtype) {
    if (dtype < 31) return g_tinfo[dtype].tsz;
    return 4;
}

int pogls_gguf_block_size(uint32_t dtype) {
    if (dtype < 31) return g_tinfo[dtype].blck;
    return 1;
}

int pogls_gguf_is_q4(const PoglsGgufReader *r, uint32_t idx) {
    if (idx >= r->n_tensors) return 0;
    uint32_t t = r->dtypes ? r->dtypes[idx] : 0;
    return (t == 2 || t == 3 || t == 12);
}

int pogls_gguf_type_is_kquant(uint32_t dtype) {
    return (dtype >= 10 && dtype <= 15) ||
           (dtype == 16 || dtype == 17 || dtype == 18 ||
            dtype == 19 || dtype == 21 || dtype == 22 ||
            dtype == 23 || dtype == 29);
}

static int skip_kv(FILE *f, uint64_t n_kv) {
    for (uint64_t k = 0; k < n_kv; k++) {
        uint64_t klen;
        if (fread(&klen, 8, 1, f) != 1) return -1;
        if (klen > 1024) return -1;
        if (fseek(f, (long)klen, SEEK_CUR) != 0) return -1;
        uint32_t vtype;
        if (fread(&vtype, 4, 1, f) != 1) return -1;
        if (vtype == 9) {
            uint32_t arr_type;
            if (fread(&arr_type, 4, 1, f) != 1) return -1;
            uint64_t narr;
            if (fread(&narr, 8, 1, f) != 1) return -1;
            static const uint8_t esz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
            if (arr_type == 8) {
                for (uint64_t a = 0; a < narr; a++) {
                    uint64_t slen;
                    if (fread(&slen, 8, 1, f) != 1) return -1;
                    if (fseek(f, (long)slen, SEEK_CUR) != 0) return -1;
                }
            } else if (arr_type < 13) {
                if (fseek(f, (long)esz[arr_type] * (long)narr, SEEK_CUR) != 0) return -1;
            }
        } else {
            switch (vtype) {
                case 0: case 1: case 7: fseek(f, 1, SEEK_CUR); break;
                case 2: case 3: fseek(f, 2, SEEK_CUR); break;
                case 4: case 5: case 6: fseek(f, 4, SEEK_CUR); break;
                case 10: case 11: case 12: fseek(f, 8, SEEK_CUR); break;
                case 8: {
                    uint64_t slen;
                    if (fread(&slen, 8, 1, f) != 1) return -1;
                    if (fseek(f, (long)slen, SEEK_CUR) != 0) return -1;
                    break;
                }
                default: return -1;
            }
        }
    }
    return 0;
}

int pogls_gguf_open(const char *path, PoglsGgufReader *r) {
    memset(r, 0, sizeof(*r));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t magic, version;
    uint64_t n_tensors, n_kv;
    if (fread(&magic,4,1,f)!=1||magic!=POGLS_GGUF_MAGIC||
        fread(&version,4,1,f)!=1||
        fread(&n_tensors,8,1,f)!=1||
        fread(&n_kv,8,1,f)!=1) { fclose(f); return -1; }
    if (skip_kv(f, n_kv) != 0) { fclose(f); return -1; }
    r->n_tensors = (uint32_t)n_tensors;
    r->names  = (char**)calloc((size_t)n_tensors, sizeof(char*));
    r->offsets= (uint64_t*)calloc((size_t)n_tensors, sizeof(uint64_t));
    r->sizes  = (uint32_t*)calloc((size_t)n_tensors, sizeof(uint32_t));
    r->dtypes = (uint32_t*)calloc((size_t)n_tensors, sizeof(uint32_t));
    if (!r->names||!r->offsets||!r->sizes||!r->dtypes) { fclose(f); return -1; }
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nlen;
        if (fread(&nlen,8,1,f)!=1||nlen==0||nlen>1024) { fclose(f); return -1; }
        char *name = (char*)malloc((size_t)nlen+1);
        if (!name) { fclose(f); return -1; }
        if (fread(name,1,(size_t)nlen,f)!=(size_t)nlen) { free(name); fclose(f); return -1; }
        name[nlen]='\0';
        r->names[i]=name;
        uint32_t n_dims;
        if (fread(&n_dims,4,1,f)!=1) { fclose(f); return -1; }
        int64_t dims[4]={1,1,1,1};
        for(uint32_t d=0;d<n_dims;d++){int64_t v;fread(&v,8,1,f);dims[d]=v;}
        uint32_t dtype;
        if (fread(&dtype,4,1,f)!=1) { fclose(f); return -1; }
        r->dtypes[i]=dtype;
        uint64_t data_off;
        if (fread(&data_off,8,1,f)!=1) { fclose(f); return -1; }
        size_t n_elems=1;
        for(uint32_t d=0;d<n_dims;d++) n_elems*=(size_t)dims[d];
        uint32_t blck = (uint32_t)g_tinfo[dtype].blck;
        uint32_t tsz  = (uint32_t)g_tinfo[dtype].tsz;
        r->sizes[i] = (blck>0) ? (uint32_t)((n_elems/blck)*tsz) : (uint32_t)(n_elems*4);
        r->offsets[i]=data_off;
    }
    long pos = ftell(f);
    uint32_t pad = (POGLS_GGUF_ALIGN - (pos % POGLS_GGUF_ALIGN)) % POGLS_GGUF_ALIGN;
    r->data_offset = (uint64_t)pos + pad;
    fclose(f);
    return 0;
}

int pogls_gguf_read_tensor(const char *path, const PoglsGgufReader *r,
                            uint32_t idx, uint8_t *buf, uint32_t cap)
{
    if (idx >= r->n_tensors) return -1;
    if (r->sizes[idx] > cap) return -2;
    FILE *f = fopen(path, "rb");
    if (!f) return -3;
    int64_t off = (int64_t)(r->data_offset + r->offsets[idx]);
#ifdef _WIN32
    if (_fseeki64(f, off, SEEK_SET) != 0) { fclose(f); return -4; }
#else
    if (fseeko(f, off, SEEK_SET) != 0) { fclose(f); return -4; }
#endif
    if (fread(buf, r->sizes[idx], 1, f) != 1) { fclose(f); return -5; }
    fclose(f);
    return 0;
}

void pogls_gguf_close(PoglsGgufReader *r) {
    if (r->names) {
        for(uint32_t i=0;i<r->n_tensors;i++) free(r->names[i]);
        free(r->names);
    }
    free(r->offsets); free(r->sizes); free(r->dtypes);
    memset(r,0,sizeof(*r));
}

int pogls_gguf_idx_open(const char *path, PoglsGgufIndex *idx) {
    FILE *f = fopen(path,"rb");
    if(!f) return -1;
    memset(idx,0,sizeof(*idx));
    uint32_t magic, version;
    uint64_t n_kv;
    if(fread(&magic,4,1,f)!=1||magic!=POGLS_GGUF_MAGIC||
       fread(&version,4,1,f)!=1||
       fread(&idx->n_tensors,8,1,f)!=1||
       fread(&n_kv,8,1,f)!=1) { fclose(f); return -1; }
    if(skip_kv(f,n_kv)!=0){fclose(f);return -1;}
    uint64_t n=idx->n_tensors;
    idx->names=(char**)calloc((size_t)n,sizeof(char*));
    idx->dtypes=(uint32_t*)calloc((size_t)n,sizeof(uint32_t));
    idx->offsets=(uint64_t*)calloc((size_t)n,sizeof(uint64_t));
    idx->sizes=(uint64_t*)calloc((size_t)n,sizeof(uint64_t));
    for(uint64_t i=0;i<n;i++){
        uint64_t klen; fread(&klen,8,1,f);
        idx->names[i]=(char*)calloc((size_t)klen+1,1);
        fread(idx->names[i],1,(size_t)klen,f);
        uint32_t nd; fread(&nd,4,1,f);
        uint64_t ndim[4]={1,1,1,1}; uint64_t ne=1;
        for(uint32_t j=0;j<nd;j++){fread(&ndim[j],8,1,f);ne*=ndim[j];}
        fread(&idx->dtypes[i],4,1,f);
        fread(&idx->offsets[i],8,1,f);
        uint32_t dt=idx->dtypes[i];
        size_t ts=g_tinfo[dt<31?dt:0].tsz;
        int bs=g_tinfo[dt<31?dt:0].blck;
        if(bs<1)bs=1;
        idx->sizes[i]=(ne/(uint64_t)bs)*(uint64_t)ts;
    }
    long pos=ftell(f);
    uint64_t am = (uint64_t)pos % POGLS_GGUF_ALIGN;
    idx->data_sec_off = (am==0)?(uint64_t)pos:(uint64_t)pos+(POGLS_GGUF_ALIGN-am);
    fclose(f);
    return 0;
}

void pogls_gguf_idx_close(PoglsGgufIndex *idx) {
    for(uint64_t i=0;i<idx->n_tensors;i++) free(idx->names[i]);
    free(idx->names); free(idx->dtypes); free(idx->offsets); free(idx->sizes);
    memset(idx,0,sizeof(*idx));
}

uint64_t pogls_gguf_idx_tensor_off(const PoglsGgufIndex *idx, uint64_t i) {
    return idx->data_sec_off + idx->offsets[i];
}

int pogls_gguf_idx_meta_blob(const char *path,
                               uint8_t **blob_out, uint64_t *size_out)
{
    PoglsGgufIndex idx;
    memset(&idx,0,sizeof(idx));
    if(pogls_gguf_idx_open(path,&idx)!=0) return -1;
    uint64_t sz = idx.data_sec_off;
    uint8_t *buf = (uint8_t*)malloc((size_t)sz);
    if(!buf){pogls_gguf_idx_close(&idx);return -1;}
    FILE *f=fopen(path,"rb");
    if(!f){free(buf);pogls_gguf_idx_close(&idx);return -1;}
    if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){free(buf);fclose(f);pogls_gguf_idx_close(&idx);return -1;}
    fclose(f);
    *blob_out=buf; *size_out=sz;
    pogls_gguf_idx_close(&idx);
    return 0;
}

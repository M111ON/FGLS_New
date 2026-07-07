#ifndef GGUF_READER_H
#define GGUF_READER_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define GGUF_MAGIC   0x46554747u
#define GGUF_ALIGN   32u

typedef struct {
    uint32_t n_tensors;
    uint64_t data_offset;
    uint64_t *offsets;
    uint32_t *sizes;
    char   **names;
} GgufReader;

static inline size_t gguf_type_size(int type) {
    /* GGML type sizes: Q8_0=8, F32=4, Q4_K_M=2, etc */
    static const size_t tbl[] = {
        0, 2, 2, 2, 4, 4, 4, 4, 1, 1,  0 /* F32=0, F16=1, Q4_0=2, Q4_1=3, Q5_0=6, Q5_1=7, Q8_0=8, Q8_1=9 */
    };
    /* actual GGML type sizes are more complex */
    return 0; /* caller should compute from tensor dimensions */
}

static inline int gguf_open(const char *path, GgufReader *r) {
    memset(r, 0, sizeof(*r));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint32_t magic;
    if (fread(&magic, 4, 1, f) != 1 || magic != GGUF_MAGIC) { fclose(f); return -1; }

    uint32_t version;
    if (fread(&version, 4, 1, f) != 1) { fclose(f); return -1; }

    uint64_t n_tensors;
    if (fread(&n_tensors, 8, 1, f) != 1) { fclose(f); return -1; }

    uint64_t n_kv;
    if (fread(&n_kv, 8, 1, f) != 1) { fclose(f); return -1; }

    /* skip KV metadata */
    for (uint64_t k = 0; k < n_kv; k++) {
        uint64_t klen;
        if (fread(&klen, 8, 1, f) != 1) { fclose(f); return -1; }
        if (klen > 255) { fclose(f); return -1; }
        if (fseek(f, (long)klen, SEEK_CUR) != 0) { fclose(f); return -1; }

        uint32_t vtype;
        if (fread(&vtype, 4, 1, f) != 1) { fclose(f); return -1; }

        if (vtype == 9) {
            uint32_t arr_type;
            if (fread(&arr_type, 4, 1, f) != 1) { fclose(f); return -1; }
            uint64_t narr;
            if (fread(&narr, 8, 1, f) != 1) { fclose(f); return -1; }
            static const uint8_t esz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
            if (arr_type == 8) {
                for (uint64_t a = 0; a < narr; a++) {
                    uint64_t slen;
                    if (fread(&slen, 8, 1, f) != 1) { fclose(f); return -1; }
                    if (fseek(f, (long)slen, SEEK_CUR) != 0) { fclose(f); return -1; }
                }
            } else if (arr_type < 13) {
                size_t skip = (size_t)esz[arr_type] * (size_t)narr;
                if (fseek(f, (long)skip, SEEK_CUR) != 0) { fclose(f); return -1; }
            }
        } else {
            switch (vtype) {
                case 0: case 1: case 7:
                    if (fseek(f, 1, SEEK_CUR) != 0) { fclose(f); return -1; }
                    break;
                case 2: case 3:
                    if (fseek(f, 2, SEEK_CUR) != 0) { fclose(f); return -1; }
                    break;
                case 4: case 5: case 6:
                    if (fseek(f, 4, SEEK_CUR) != 0) { fclose(f); return -1; }
                    break;
                case 10: case 11: case 12:
                    if (fseek(f, 8, SEEK_CUR) != 0) { fclose(f); return -1; }
                    break;
                case 8: {
                    uint64_t slen;
                    if (fread(&slen, 8, 1, f) != 1) { fclose(f); return -1; }
                    if (fseek(f, (long)slen, SEEK_CUR) != 0) { fclose(f); return -1; }
                    break;
                }
                default:
                    fclose(f);
                    return -1;
            }
        }
    }

    /* tensor info starts immediately after KV metadata — NO alignment padding */
    r->n_tensors = (uint32_t)n_tensors;
    r->names = (char **)calloc(n_tensors, sizeof(char *));
    r->offsets = (uint64_t *)calloc(n_tensors, sizeof(uint64_t));
    r->sizes = (uint32_t *)calloc(n_tensors, sizeof(uint32_t));
    if (!r->names || !r->offsets || !r->sizes) { fclose(f); return -1; }

    /* GGML type sizes (type_size) and block sizes (blck_size) */
    /* Computed from ggml-common.h struct sizes */
    static const struct { uint16_t tsz; uint16_t blck; } tinfo[31] = {
        {4,   1},   /* GGML_TYPE_F32    = 0  */
        {2,   1},   /* GGML_TYPE_F16    = 1  */
        {18,  32},  /* GGML_TYPE_Q4_0   = 2  */
        {20,  32},  /* GGML_TYPE_Q4_1   = 3  */
        {0,   0},   /* 4 removed */
        {0,   0},   /* 5 removed */
        {22,  32},  /* GGML_TYPE_Q5_0   = 6  */
        {24,  32},  /* GGML_TYPE_Q5_1   = 7  */
        {34,  32},  /* GGML_TYPE_Q8_0   = 8  */
        {36,  32},  /* GGML_TYPE_Q8_1   = 9  */
        {84,  256}, /* GGML_TYPE_Q2_K   = 10 */
        {110, 256}, /* GGML_TYPE_Q3_K   = 11 */
        {144, 256}, /* GGML_TYPE_Q4_K   = 12 */
        {176, 256}, /* GGML_TYPE_Q5_K   = 13 */
        {210, 256}, /* GGML_TYPE_Q6_K   = 14 */
        {292, 256}, /* GGML_TYPE_Q8_K   = 15 */
        {2,   256}, /* GGML_TYPE_IQ2_XXS=16 */
        {2,   256}, /* GGML_TYPE_IQ2_XS =17 */
        {2,   256}, /* GGML_TYPE_IQ3_XXS=18 */
        {1,   256}, /* GGML_TYPE_IQ1_S =19 */
        {2,   32},  /* GGML_TYPE_IQ4_NL =20 */
        {1,   256}, /* GGML_TYPE_IQ3_S =21 */
        {1,   256}, /* GGML_TYPE_IQ2_S =22 */
        {2,   256}, /* GGML_TYPE_IQ4_XS =23 */
        {1,   1},   /* GGML_TYPE_I8    =24 */
        {2,   1},   /* GGML_TYPE_I16   =25 */
        {4,   1},   /* GGML_TYPE_I32   =26 */
        {8,   1},   /* GGML_TYPE_I64   =27 */
        {8,   1},   /* GGML_TYPE_F64   =28 */
        {1,   256}, /* GGML_TYPE_IQ1_M =29 */
        {2,   1},   /* GGML_TYPE_BF16  =30 */
    };

    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nlen;
        if (fread(&nlen, 8, 1, f) != 1) { fclose(f); return -1; }
        if (nlen == 0 || nlen > 1024) { fclose(f); return -1; }
        char *name = (char *)malloc((size_t)nlen + 1);
        if (!name) { fclose(f); return -1; }
        if (fread(name, (size_t)nlen, 1, f) != 1) { free(name); fclose(f); return -1; }
        name[nlen] = '\0';
        r->names[i] = name;

        /* n_dimensions is uint32_t */
        uint32_t n_dims;
        if (fread(&n_dims, 4, 1, f) != 1) { fclose(f); return -1; }

        /* dimensions are int64_t (8 bytes each) */
        int64_t dims[4] = {1,1,1,1};
        for (uint32_t d = 0; d < n_dims; d++) {
            int64_t v;
            if (fread(&v, 8, 1, f) != 1) { fclose(f); return -1; }
            dims[d] = v;
        }

        /* type is uint32_t (GGML type enum) */
        uint32_t dtype;
        if (fread(&dtype, 4, 1, f) != 1) { fclose(f); return -1; }

        /* offset is uint64_t (relative to data section) */
        uint64_t data_off;
        if (fread(&data_off, 8, 1, f) != 1) { fclose(f); return -1; }

        /* compute size from dims and type */
        size_t tsize = 0;
        if (dtype < 31 && tinfo[dtype].tsz > 0 && tinfo[dtype].blck > 0) {
            size_t n_elems = 1;
            for (uint32_t d = 0; d < n_dims; d++) n_elems *= (size_t)dims[d];
            tsize = (n_elems / tinfo[dtype].blck) * tinfo[dtype].tsz;
        }

        r->sizes[i] = (uint32_t)tsize;
        r->offsets[i] = data_off;
    }

    /* alignment to data section: align to 32 (default) */
    long pos = ftell(f);
    uint32_t pad = (GGUF_ALIGN - (pos % GGUF_ALIGN)) % GGUF_ALIGN;
    r->data_offset = (uint64_t)pos + pad;

    fclose(f);
    return 0;
}

static inline int gguf_read_tensor(const char *path, const GgufReader *r,
                                    uint32_t idx, uint8_t *buf, uint32_t cap)
{
    if (idx >= r->n_tensors) return -1;
    if (r->sizes[idx] > cap) return -2;

    FILE *f = fopen(path, "rb");
    if (!f) return -3;
    if (_fseeki64(f, (__int64)(r->data_offset + r->offsets[idx]), SEEK_SET) != 0) {
        fclose(f); return -4;
    }
    if (fread(buf, r->sizes[idx], 1, f) != 1) { fclose(f); return -5; }
    fclose(f);
    return 0;
}

static inline void gguf_close(GgufReader *r) {
    if (r->names) {
        for (uint32_t i = 0; i < r->n_tensors; i++) free(r->names[i]);
        free(r->names);
    }
    free(r->offsets);
    free(r->sizes);
    memset(r, 0, sizeof(*r));
}

#endif

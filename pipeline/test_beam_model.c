/*
 * test_beam_model.c — Test Beam Module with Real GGUF Model
 * ═══════════════════════════════════════════════════════════════════
 * อ่าน GGUF header จริง แล้วคำนวณ beam module size
 * 
 * GGUF format: https://github.com/ggerganov/llama.cpp/blob/master/llama.cpp
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ══════════════════════════════════════════════════════════════
   GGUF HEADER PARSING
   ══════════════════════════════════════════════════════════════ */

#define GGUF_MAGIC      0x46554747  /* "GGUF" */
#define GGUF_VERSION    3

/* GGUF value types */
#define GGUF_TYPE_UINT8     0
#define GGUF_TYPE_INT8      1
#define GGUF_TYPE_UINT16    2
#define GGUF_TYPE_INT16     3
#define GGUF_TYPE_UINT32    4
#define GGUF_TYPE_INT32     5
#define GGUF_TYPE_FLOAT32   6
#define GGUF_TYPE_BOOL      7
#define GGUF_TYPE_STRING    8
#define GGUF_TYPE_ARRAY     9
#define GGUF_TYPE_UINT64    10
#define GGUF_TYPE_INT64     11
#define GGUF_TYPE_FLOAT64   12

/* GGUF tensor types (quantization) */
#define GGML_TYPE_F32       0
#define GGML_TYPE_F16       1
#define GGML_TYPE_Q4_0      2
#define GGML_TYPE_Q4_1      3
#define GGML_TYPE_Q5_0      6
#define GGML_TYPE_Q5_1      7
#define GGML_TYPE_Q8_0      8
#define GGML_TYPE_Q8_1      9
#define GGML_TYPE_Q2_K      10
#define GGML_TYPE_Q3_K      11
#define GGML_TYPE_Q4_K      12
#define GGML_TYPE_Q5_K      13
#define GGML_TYPE_Q6_K      14
#define GGML_TYPE_Q8_K      15
#define GGML_TYPE_IQ2_XXS   16
#define GGML_TYPE_IQ2_XS    17
#define GGML_TYPE_IQ3_XXS   18
#define GGML_TYPE_IQ1_S     19
#define GGML_TYPE_IQ4_NL    20
#define GGML_TYPE_IQ3_S     21
#define GGML_TYPE_IQ2_S     22
#define GGML_TYPE_IQ4_XS    23

typedef struct {
    uint32_t n_dims;
    uint32_t *dims;
    uint32_t type;
    char *name;
    uint64_t offset;  /* offset from start of data section */
} GGUFTensor;

typedef struct {
    uint32_t n_tensors;
    GGUFTensor *tensors;
    uint64_t total_size;  /* total model size in bytes */
} GGUFModel;

/* read uint32 LE */
static inline uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | 
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* read uint64 LE */
static inline uint64_t read_u64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | 
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/* read string (u64 len + data) */
static char* read_string(const uint8_t *p, uint32_t *len_out) {
    uint64_t len = read_u64(p);
    char *str = (char *)malloc(len + 1);
    memcpy(str, p + 8, len);
    str[len] = 0;
    *len_out = (uint32_t)len;
    return str;
}

/* ══════════════════════════════════════════════════════════════
   QUANTIZATION SIZE TABLE
   ══════════════════════════════════════════════════════════════ */

static const char* quant_name(uint32_t type) {
    switch (type) {
        case GGML_TYPE_F32:    return "F32";
        case GGML_TYPE_F16:    return "F16";
        case GGML_TYPE_Q4_0:   return "Q4_0";
        case GGML_TYPE_Q4_1:   return "Q4_1";
        case GGML_TYPE_Q5_0:   return "Q5_0";
        case GGML_TYPE_Q5_1:   return "Q5_1";
        case GGML_TYPE_Q8_0:   return "Q8_0";
        case GGML_TYPE_Q8_1:   return "Q8_1";
        case GGML_TYPE_Q2_K:   return "Q2_K";
        case GGML_TYPE_Q3_K:   return "Q3_K";
        case GGML_TYPE_Q4_K:   return "Q4_K";
        case GGML_TYPE_Q5_K:   return "Q5_K";
        case GGML_TYPE_Q6_K:   return "Q6_K";
        case GGML_TYPE_Q8_K:   return "Q8_K";
        default:               return "UNKNOWN";
    }
}

/* bytes per element for quantized types */
static double bytes_per_elem(uint32_t type) {
    switch (type) {
        case GGML_TYPE_F32:    return 4.0;
        case GGML_TYPE_F16:    return 2.0;
        case GGML_TYPE_Q4_0:   return 0.5625;  /* 4.5 bits */
        case GGML_TYPE_Q4_1:   return 0.6875;  /* 5.5 bits */
        case GGML_TYPE_Q5_0:   return 0.6875;  /* 5.5 bits */
        case GGML_TYPE_Q5_1:   return 0.8125;  /* 6.5 bits */
        case GGML_TYPE_Q8_0:   return 1.0;
        case GGML_TYPE_Q8_1:   return 1.0625;  /* 8.5 bits */
        case GGML_TYPE_Q2_K:   return 0.3125;  /* 2.5 bits */
        case GGML_TYPE_Q3_K:   return 0.4375;  /* 3.5 bits */
        case GGML_TYPE_Q4_K:   return 0.5625;  /* 4.5 bits */
        case GGML_TYPE_Q5_K:   return 0.6875;  /* 5.5 bits */
        case GGML_TYPE_Q6_K:   return 0.8125;  /* 6.5 bits */
        case GGML_TYPE_Q8_K:   return 1.0;
        default:               return 1.0;
    }
}

/* ══════════════════════════════════════════════════════════════
   PARSE GGUF
   ══════════════════════════════════════════════════════════════ */

int parse_gguf(const char *filename, GGUFModel *model) {
    FILE *f = fopen(filename, "rb");
    if (!f) return -1;
    
    /* Read magic + version */
    uint8_t header[8];
    fread(header, 1, 8, f);
    uint32_t magic = read_u32(header);
    uint32_t version = read_u32(header + 4);
    
    if (magic != GGUF_MAGIC) {
        printf("ERROR: Not a GGUF file (magic: 0x%08X)\n", magic);
        fclose(f);
        return -1;
    }
    if (version != GGUF_VERSION) {
        printf("ERROR: Unsupported GGUF version %u\n", version);
        fclose(f);
        return -1;
    }
    
    /* Read n_tensors */
    uint8_t buf[8];
    fread(buf, 1, 8, f);
    uint64_t n_tensors = read_u64(buf);
    model->n_tensors = (uint32_t)n_tensors;
    model->tensors = (GGUFTensor *)calloc(n_tensors, sizeof(GGUFTensor));
    
    printf("  GGUF Version: %u\n", version);
    printf("  Tensors: %u\n", model->n_tensors);
    
    /* Skip metadata (read and discard) */
    uint64_t n_kv;
    fread(buf, 1, 8, f);
    n_kv = read_u64(buf);
    printf("  Metadata KV pairs: %llu\n", (unsigned long long)n_kv);
    
    for (uint64_t i = 0; i < n_kv; i++) {
        uint32_t slen;
        char *key = read_string(NULL, &slen);
        /* Read key from file */
        fseek(f, 0, SEEK_CUR);  /* reset */
        uint8_t key_len_buf[8];
        fread(key_len_buf, 1, 8, f);
        uint64_t key_len = read_u64(key_len_buf);
        fseek(f, key_len, SEEK_CUR);
        
        /* Skip value (read type + skip) */
        uint8_t type_buf[4];
        fread(type_buf, 1, 4, f);
        uint32_t val_type = read_u32(type_buf);
        
        /* Skip value based on type */
        switch (val_type) {
            case GGUF_TYPE_UINT8:
            case GGUF_TYPE_INT8:
            case GGUF_TYPE_BOOL:
                fseek(f, 1, SEEK_CUR);
                break;
            case GGUF_TYPE_UINT16:
            case GGUF_TYPE_INT16:
                fseek(f, 2, SEEK_CUR);
                break;
            case GGUF_TYPE_UINT32:
            case GGUF_TYPE_INT32:
            case GGUF_TYPE_FLOAT32:
                fseek(f, 4, SEEK_CUR);
                break;
            case GGUF_TYPE_UINT64:
            case GGUF_TYPE_INT64:
            case GGUF_TYPE_FLOAT64:
                fseek(f, 8, SEEK_CUR);
                break;
            case GGUF_TYPE_STRING: {
                uint8_t slen_buf[8];
                fread(slen_buf, 1, 8, f);
                uint64_t sl = read_u64(slen_buf);
                fseek(f, sl, SEEK_CUR);
                break;
            }
            case GGUF_TYPE_ARRAY: {
                /* Read array type + count, then skip elements */
                uint8_t arr_type_buf[4];
                fread(arr_type_buf, 1, 4, f);
                uint32_t arr_type = read_u32(arr_type_buf);
                uint8_t arr_count_buf[8];
                fread(arr_count_buf, 1, 8, f);
                uint64_t arr_count = read_u64(arr_count_buf);
                
                /* Skip based on element type */
                int elem_size = 0;
                switch (arr_type) {
                    case GGUF_TYPE_UINT8: case GGUF_TYPE_INT8: case GGUF_TYPE_BOOL:
                        elem_size = 1; break;
                    case GGUF_TYPE_UINT16: case GGUF_TYPE_INT16:
                        elem_size = 2; break;
                    case GGUF_TYPE_UINT32: case GGUF_TYPE_INT32: case GGUF_TYPE_FLOAT32:
                        elem_size = 4; break;
                    case GGUF_TYPE_UINT64: case GGUF_TYPE_INT64: case GGUF_TYPE_FLOAT64:
                        elem_size = 8; break;
                    case GGUF_TYPE_STRING:
                        /* Each string has its own length prefix */
                        for (uint64_t j = 0; j < arr_count; j++) {
                            uint8_t sl2[8];
                            fread(sl2, 1, 8, f);
                            uint64_t l = read_u64(sl2);
                            fseek(f, l, SEEK_CUR);
                        }
                        elem_size = 0;
                        break;
                    default:
                        break;
                }
                if (elem_size > 0) {
                    fseek(f, arr_count * elem_size, SEEK_CUR);
                }
                break;
            }
            default:
                break;
        }
    }
    
    /* Read tensor info */
    model->total_size = 0;
    for (uint32_t i = 0; i < model->n_tensors; i++) {
        GGUFTensor *t = &model->tensors[i];
        
        /* n_dims */
        uint8_t nd_buf[4];
        fread(nd_buf, 1, 4, f);
        t->n_dims = read_u32(nd_buf);
        
        /* dims */
        t->dims = (uint32_t *)malloc(t->n_dims * 4);
        for (uint32_t d = 0; d < t->n_dims; d++) {
            uint8_t dim_buf[4];
            fread(dim_buf, 1, 4, f);
            t->dims[d] = read_u32(dim_buf);
        }
        
        /* type */
        uint8_t type_buf[4];
        fread(type_buf, 1, 4, f);
        t->type = read_u32(type_buf);
        
        /* name */
        uint32_t name_len;
        uint8_t name_len_buf[8];
        fread(name_len_buf, 1, 8, f);
        uint64_t nl = read_u64(name_len_buf);
        t->name = (char *)malloc(nl + 1);
        fread(t->name, 1, nl, f);
        t->name[nl] = 0;
        
        /* offset (padding to GGUF alignment) */
        uint8_t offset_buf[8];
        fread(offset_buf, 1, 8, f);
        t->offset = read_u64(offset_buf);
        
        /* Calculate tensor size */
        uint64_t nelements = 1;
        for (uint32_t d = 0; d < t->n_dims; d++) {
            nelements *= t->dims[d];
        }
        uint64_t tensor_size = (uint64_t)(nelements * bytes_per_elem(t->type));
        
        /* Align to 32 bytes */
        tensor_size = (tensor_size + 31) & ~(uint64_t)31;
        
        model->total_size += tensor_size;
    }
    
    fclose(f);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   MAIN ANALYSIS
   ══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    const char *filename = "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    if (argc > 1) filename = argv[1];
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════════╗\n");
    printf("║  BEAM MODULE — REAL GGUF MODEL SIZE ANALYSIS                     ║\n");
    printf("║  Coordinate IS the data. No hash. No collision. No storage.     ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════╝\n\n");
    
    printf("Loading: %s\n\n", filename);
    
    GGUFModel model = {0};
    if (parse_gguf(filename, &model) != 0) {
        printf("Failed to parse GGUF\n");
        return 1;
    }
    
    /* ══════════════════════════════════════════════════════════════
       CALCULATE SIZES
       ══════════════════════════════════════════════════════════════ */
    
    uint64_t total_elements = 0;
    uint64_t total_weight_bytes = 0;
    uint64_t total_metadata = 0;
    
    /* Beam module calculations */
    uint64_t beam_length_bytes = 0;  /* 4 bytes per weight (|weight|) */
    uint64_t beam_sign_bits = 0;    /* 1 bit per weight */
    uint64_t beam_coord_overhead = 0; /* 0 (computed on-the-fly) */
    
    /* Q8 quantized */
    uint64_t q8_bytes = 0;
    
    printf("┌─────────────────────────────────────────────────────────────────┐\n");
    printf("│  TENSOR BREAKDOWN                                              │\n");
    printf("├──────────────────────────────┬──────────┬──────────┬────────────┤\n");
    printf("│  Tensor                      │  Params  │  Size    │  Quant     │\n");
    printf("├──────────────────────────────┼──────────┼──────────┼────────────┤\n");
    
    for (uint32_t i = 0; i < model.n_tensors; i++) {
        GGUFTensor *t = &model.tensors[i];
        
        uint64_t nelements = 1;
        for (uint32_t d = 0; d < t->n_dims; d++) {
            nelements *= t->dims[d];
        }
        
        uint64_t tensor_size = (uint64_t)(nelements * bytes_per_elem(t->type));
        tensor_size = (tensor_size + 31) & ~(uint64_t)31;
        
        total_elements += nelements;
        total_weight_bytes += tensor_size;
        
        /* Beam: 4 bytes (beam_length) + 1 bit (sign) per element */
        beam_length_bytes += nelements * 4;
        beam_sign_bits += nelements;
        
        /* Q8: 1 byte per element */
        q8_bytes += nelements * 1;
        
        /* Metadata: name (~50B) + offset (8B) + type (4B) + dims (16B) */
        total_metadata += strlen(t->name) + 8 + 4 + t->n_dims * 4 + 8;
        
        printf("│  %-27s │ %7llu  │ %5.1f MB │  %-8s  │\n",
               t->name, (unsigned long long)nelements, 
               tensor_size / 1048576.0, quant_name(t->type));
    }
    printf("└──────────────────────────────┴──────────┴──────────┴────────────┘\n\n");
    
    /* ══════════════════════════════════════════════════════════════
       SIZE COMPARISON
       ══════════════════════════════════════════════════════════════ */
    
    uint64_t total_normal = total_weight_bytes + total_metadata;
    uint64_t beam_total = beam_length_bytes + (beam_sign_bits / 8) + 100; /* +100 for container overhead */
    uint64_t q8_total = q8_bytes + 100;
    
    double ratio_beam = (double)total_normal / beam_total;
    double ratio_q8 = (double)total_normal / q8_total;
    double ratio_q8_vs_beam = (double)beam_total / q8_total;
    
    printf("┌─────────────────────────────────────────────────────────────────┐\n");
    printf("│  SIZE COMPARISON                                               │\n");
    printf("├─────────────────────────────────────────────────────────────────┤\n");
    printf("│                                                                 │\n");
    printf("│  MODEL: %s\n", filename);
    printf("│  Total params: %llu (%.1fM)\n", 
           (unsigned long long)total_elements, total_elements / 1000000.0);
    printf("│                                                                 │\n");
    printf("├─────────────────────────────────────────────────────────────────┤\n");
    printf("│                                                                 │\n");
    printf("│  1. GGUF ORIGINAL (quantized weights + metadata)                │\n");
    printf("│     Weight data:   %8.1f MB                              │\n", 
           total_weight_bytes / 1048576.0);
    printf("│     Metadata:      %8.1f KB  (names, offsets, types)    │\n",
           total_metadata / 1024.0);
    printf("│     TOTAL:         %8.1f MB                              │\n",
           total_normal / 1048576.0);
    printf("│                                                                 │\n");
    printf("├─────────────────────────────────────────────────────────────────┤\n");
    printf("│                                                                 │\n");
    printf("│  2. BEAM MODULE (beam_length + sign)                            │\n");
    printf("│     beam_length:   %8.1f MB  (4B × %llu params)     │\n",
           beam_length_bytes / 1048576.0, (unsigned long long)total_elements);
    printf("│     sign (packed): %8.1f KB  (1 bit × %llu params)   │\n",
           (beam_sign_bits / 8) / 1024.0, (unsigned long long)total_elements);
    printf("│     coord storage: %8.1f KB  (computed on-the-fly)     │\n",
           0.0);
    printf("│     Overhead:      %8.1f KB  (container)               │\n",
           100.0 / 1024.0);
    printf("│     TOTAL:         %8.1f MB                              │\n",
           beam_total / 1048576.0);
    printf("│                                                                 │\n");
    printf("├─────────────────────────────────────────────────────────────────┤\n");
    printf("│                                                                 │\n");
    printf("│  3. Q8 QUANTIZED BEAM (1 byte per weight)                       │\n");
    printf("│     Quantized:     %8.1f MB  (1B × %llu params)     │\n",
           q8_bytes / 1048576.0, (unsigned long long)total_elements);
    printf("│     Overhead:      %8.1f KB  (container)               │\n",
           100.0 / 1024.0);
    printf("│     TOTAL:         %8.1f MB                              │\n",
           q8_total / 1048576.0);
    printf("│                                                                 │\n");
    printf("└─────────────────────────────────────────────────────────────────┘\n\n");
    
    /* ══════════════════════════════════════════════════════════════
       REDUCTION RATIOS
       ══════════════════════════════════════════════════════════════ */
    
    printf("┌─────────────────────────────────────────────────────────────────┐\n");
    printf("│  REDUCTION RATIOS                                              │\n");
    printf("├─────────────────────────────────────────────────────────────────┤\n");
    printf("│                                                                 │\n");
    printf("│  GGUF → Beam:    %5.1f× reduction  (%.1f%% saved)           │\n",
           ratio_beam, (1.0 - 1.0/ratio_beam) * 100);
    printf("│  GGUF → Q8:      %5.1f× reduction  (%.1f%% saved)           │\n",
           ratio_q8, (1.0 - 1.0/ratio_q8) * 100);
    printf("│  Beam → Q8:      %5.1f× reduction  (%.1f%% saved)           │\n",
           ratio_q8_vs_beam, (1.0 - 1.0/ratio_q8_vs_beam) * 100);
    printf("│                                                                 │\n");
    printf("└─────────────────────────────────────────────────────────────────┘\n\n");
    
    /* ══════════════════════════════════════════════════════════════
       WHAT'S ACTUALLY STORED
       ══════════════════════════════════════════════════════════════ */
    
    printf("┌─────────────────────────────────────────────────────────────────┐\n");
    printf("│  WHAT'S ACTUALLY STORED (Beam Module)                          │\n");
    printf("├─────────────────────────────────────────────────────────────────┤\n");
    printf("│                                                                 │\n");
    printf("│  Per weight:                                                   │\n");
    printf("│    beam_length:  4 bytes  (|weight|, the data itself)           │\n");
    printf("│    sign:         1 bit    (+ or -)                              │\n");
    printf("│    ─────────────────────────────                                │\n");
    printf("│    TOTAL:        4.125 bytes per weight                         │\n");
    printf("│                                                                 │\n");
    printf("│  Coordinate: NOT STORED (computed on-the-fly)                   │\n");
    printf("│    capo_id:      computed from param_index                      │\n");
    printf("│    param_index:  = weight index (no storage needed)             │\n");
    printf("│    slot_index:   = beam_to_fibo_slot(coord)                     │\n");
    printf("│                                                                 │\n");
    printf("└─────────────────────────────────────────────────────────────────┘\n\n");
    
    /* ══════════════════════════════════════════════════════════════
       SUMMARY
       ══════════════════════════════════════════════════════════════ */
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  SUMMARY\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("\n");
    printf("  สำหรับโมเดล %.1fM params:\n", total_elements / 1000000.0);
    printf("\n");
    printf("  GGUF original:     %8.1f MB\n", total_normal / 1048576.0);
    printf("  Beam module:       %8.1f MB  (%.1f× smaller)\n", 
           beam_total / 1048576.0, ratio_beam);
    printf("  Q8 quantized:      %8.1f MB  (%.1f× smaller)\n",
           q8_total / 1048576.0, ratio_q8);
    printf("\n");
    printf("  สิ่งที่หายไป:\n");
    printf("    ✓ metadata (name, offset, type)     = 0 bytes\n");
    printf("    ✓ index/lookup table                = 0 bytes\n");
    printf("    ✓ hash                              = 0 bytes\n");
    printf("    ✓ position encoding                 = 0 bytes (coordinate = identity)\n");
    printf("\n");
    printf("  สิ่งที่เหลืออยู่:\n");
    printf("    • beam_length (|weight|)            = data เอง (บีบไม่ได้)\n");
    printf("    • sign (+/-)                        = 1 bit\n");
    printf("\n");
    printf("  สรุป: %.1f× reduction โดยไม่ต้อง compress อะไรเลย\n", ratio_beam);
    printf("         เพราะ metadata ทั้งหมดหายไป คงเหลือแค่ data จริงๆ\n");
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    
    /* Cleanup */
    for (uint32_t i = 0; i < model.n_tensors; i++) {
        free(model.tensors[i].name);
        free(model.tensors[i].dims);
    }
    free(model.tensors);
    
    return 0;
}

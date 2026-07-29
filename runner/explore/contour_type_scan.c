// contour_type_scan.c
// Scan GGUF model with Contour Mask — handles ANY dtype
// For vision model comparison (Qwen3-4B-Instruct vs Qwen3-0.6B)
//
// Compile: gcc -O2 -std=c11 -Wno-error=misleading-indentation -Wno-error=format -o contour_type_scan.exe contour_type_scan.c -lm
// Run:     contour_type_scan.exe model.gguf

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define GX  10
#define GY  10
#define GZ  6
#define GT  (GX * GY * GZ)
#define N_MASKS 3

// ============================================================
// GGUF
// ============================================================
#define GGUF_MAGIC 0x46554747u

static FILE *gguf_fopen(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) return f;
    if (path[0] == '/' && path[2] == '/') {
        char wp[512];
        snprintf(wp, sizeof(wp), "%c:\\%s", path[1], path + 3);
        for (char *p = wp; *p; p++) if (*p == '/') *p = '\\';
        f = fopen(wp, "rb");
    }
    return f;
}

static void skip_kv(FILE *f, uint64_t n_kv) {
    for (uint64_t i = 0; i < n_kv; i++) {
        uint64_t klen; fread(&klen, 8, 1, f); fseek(f, klen, SEEK_CUR);
        uint32_t vt; fread(&vt, 4, 1, f);
        switch(vt) {
            case 0: case 1: fseek(f,1,SEEK_CUR); break;
            case 2: case 3: fseek(f,2,SEEK_CUR); break;
            case 4: case 5: case 6: fseek(f,4,SEEK_CUR); break;
            case 7: fseek(f,1,SEEK_CUR); break;
            case 8: { uint64_t s; fread(&s,8,1,f); fseek(f,s,SEEK_CUR); break; }
            case 9: {
                uint32_t at; uint64_t al; fread(&at,4,1,f); fread(&al,8,1,f);
                if(at==8) for(uint64_t j=0;j<al;j++){uint64_t s;fread(&s,8,1,f);fseek(f,s,SEEK_CUR);}
                else fseek(f,al*(at<=3?2:at<=6?4:1),SEEK_CUR);
                break;
            }
            default: fseek(f,4,SEEK_CUR); break;
        }
    }
}

static const char *dtype_name(uint32_t dt) {
    switch(dt) {
        case 0: return "F32";
        case 1: return "F16";
        case 2: return "Q4_0";
        case 3: return "Q4_1";
        case 4: return "Q5_0";
        case 5: return "Q5_1";
        case 6: return "Q8_0";
        case 7: return "Q8_1";
        case 8: return "Q2_K";
        case 9: return "Q3_K_S";
        case 10: return "Q3_K_M";
        case 11: return "Q3_K_L";
        case 12: return "Q4_K_S";
        case 13: return "Q4_K_M";
        case 14: return "Q5_K_S";
        case 15: return "Q5_K_M";
        case 16: return "Q6_K";
        default: return "unknown";
    }
}

// Block sizes for different quant types
static int block_size(uint32_t dt) {
    switch(dt) {
        case 0: return 4;   // F32
        case 1: return 2;   // F16
        case 2: return 18;  // Q4_0
        case 3: return 20;  // Q4_1
        case 4: return 22;  // Q5_0
        case 5: return 24;  // Q5_1
        case 6: return 34;  // Q8_0
        case 7: return 36;  // Q8_1
        case 12: return 144; // Q4_K_S
        case 13: return 144; // Q4_K_M
        case 14: return 176; // Q5_K_S
        case 15: return 176; // Q5_K_M
        case 16: return 216; // Q6_K
        default: return 32;
    }
}

// Extract raw bytes from tensor (dequantize Q4_K_M to int8 approximation)
static int extract_values(FILE *f, uint64_t offset, uint64_t size, uint32_t dtype, int8_t *out, int max_out) {
    int bs = block_size(dtype);
    if (bs <= 0) bs = 32;
    int n_blocks = size / bs;
    int nr = 0;

    fseek(f, offset, SEEK_SET);

    for (int b = 0; b < n_blocks && nr < max_out; b++) {
        uint64_t bstart = ftell(f);
        if (bstart >= offset + size) break;

        if (dtype == 6) {
            // Q8_0: skip 2 bytes scale, read 32 int8
            fseek(f, 2, SEEK_CUR);
            for (int j = 0; j < 32 && nr < max_out; j++) {
                fread(&out[nr], 1, 1, f);
                nr++;
            }
        } else if (dtype == 13) {
            // Q4_K_M: read raw bytes as proxy for weight distribution
            // Block: [scale:2][min:2][32 bytes packed weights]
            // For contour mask, we just need the weight bytes (raw proxy)
            fseek(f, 4, SEEK_CUR); // skip scale+min
            for (int j = 0; j < 32 && nr < max_out; j++) {
                uint8_t byte;
                fread(&byte, 1, 1, f);
                // Q4_K_M stores two 4-bit values per byte
                // Extract high nibble as proxy
                out[nr] = (int8_t)((byte >> 4) - 8);  // center around 0
                nr++;
                if (nr < max_out) {
                    out[nr] = (int8_t)((byte & 0x0F) - 8);
                    nr++;
                }
            }
        } else if (dtype == 12) {
            // Q4_K_S: similar to Q4_K_M
            fseek(f, 4, SEEK_CUR);
            for (int j = 0; j < 32 && nr < max_out; j++) {
                uint8_t byte;
                fread(&byte, 1, 1, f);
                out[nr] = (int8_t)((byte >> 4) - 8);
                nr++;
                if (nr < max_out) {
                    out[nr] = (int8_t)((byte & 0x0F) - 8);
                    nr++;
                }
            }
        } else if (dtype == 2) {
            // Q4_0: 2 bytes scale + 16 bytes packed
            fseek(f, 2, SEEK_CUR);
            for (int j = 0; j < 16 && nr < max_out; j++) {
                uint8_t byte;
                fread(&byte, 1, 1, f);
                out[nr] = (int8_t)((byte >> 4) - 8);
                nr++;
                if (nr < max_out) {
                    out[nr] = (int8_t)((byte & 0x0F) - 8);
                    nr++;
                }
            }
        } else {
            // Unknown: read raw bytes
            int to_read = bs < 32 ? bs : 32;
            for (int j = 0; j < to_read && nr < max_out; j++) {
                fread(&out[nr], 1, 1, f);
                nr++;
            }
        }

        // Skip to next block
        fseek(f, offset + (b + 1) * bs, SEEK_SET);
    }
    return nr;
}

// ============================================================
// Contour Mask
// ============================================================
typedef struct {
    int8_t visible[N_MASKS][GT];
    int    pin_count[N_MASKS];
} QuickMask;

static void qm_init(QuickMask *qm) {
    for (int m = 0; m < N_MASKS; m++) {
        int count = 0;
        for (int x = 0; x < GX; x++)
            for (int y = 0; y < GY; y++)
                for (int z = 0; z < GZ; z++)
                    if ((x + y + z) % N_MASKS == m) count++;
        qm->pin_count[m] = count;
    }
}

static void qm_encode(QuickMask *qm, const int8_t *data) {
    for (int m = 0; m < N_MASKS; m++) {
        int idx = 0;
        for (int x = 0; x < GX; x++)
            for (int y = 0; y < GY; y++)
                for (int z = 0; z < GZ; z++)
                    if ((x + y + z) % N_MASKS == m)
                        qm->visible[m][idx++] = data[x*GY*GZ + y*GZ + z];
    }
}

static double qm_entropy(const int8_t *data, int len) {
    int counts[256] = {0};
    for (int i = 0; i < len; i++) counts[(uint8_t)data[i]]++;
    double ent = 0;
    for (int i = 0; i < 256; i++) {
        if (!counts[i]) continue;
        double p = (double)counts[i] / len;
        ent -= p * log2(p);
    }
    return ent;
}

static double qm_correlation(const int8_t *a, const int8_t *b, int n) {
    double sa=0,sb=0,sab=0,sa2=0,sb2=0;
    for (int i = 0; i < n; i++) {
        double va=a[i],vb=b[i]; sa+=va; sb+=vb; sab+=va*vb; sa2+=va*va; sb2+=vb*vb;
    }
    double ma=sa/n, mb=sb/n, cov=sab/n-ma*mb;
    double da=sqrt(sa2/n-ma*ma), db=sqrt(sb2/n-mb*mb);
    return (da>0&&db>0) ? cov/(da*db) : 0;
}

// ============================================================
// Tensor info
// ============================================================
typedef struct {
    char     name[256];
    uint32_t dtype;
    uint64_t offset;
    uint64_t size;
} TensorInfo;

// ============================================================
int main(int argc, char **argv) {
    if (argc < 2) { printf("Usage: contour_type_scan model.gguf\n"); return 1; }

    printf("============================================================\n");
    printf("  Contour Mask — Type-Aware Model Scan\n");
    printf("  Grid: %dx%dx%d = %d | Masks: %d\n", GX, GY, GZ, GT, N_MASKS);
    printf("============================================================\n\n");

    FILE *f = gguf_fopen(argv[1]);
    if (!f) { printf("Cannot open %s\n", argv[1]); return 1; }

    uint32_t magic, version; uint64_t n_tensors, n_kv;
    fread(&magic, 4, 1, f); fread(&version, 4, 1, f);
    fread(&n_tensors, 8, 1, f); fread(&n_kv, 8, 1, f);
    if (magic != GGUF_MAGIC) { fclose(f); printf("Bad magic\n"); return 1; }

    skip_kv(f, n_kv);

    TensorInfo *tinfos = (TensorInfo*)calloc(n_tensors, sizeof(TensorInfo));
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nlen; fread(&nlen, 8, 1, f);
        fread(tinfos[i].name, nlen, 1, f); tinfos[i].name[nlen] = 0;
        uint32_t nd; fread(&nd, 4, 1, f);
        for (uint32_t j=0;j<nd;j++){uint64_t d;fread(&d,8,1,f);}
        fread(&tinfos[i].dtype, 4, 1, f);
        fread(&tinfos[i].offset, 8, 1, f);
    }

    fseek(f, 0, SEEK_END);
    uint64_t file_end = ftell(f);
    for (uint64_t i = 0; i < n_tensors; i++) {
        if (i + 1 < n_tensors)
            tinfos[i].size = tinfos[i+1].offset - tinfos[i].offset;
        else
            tinfos[i].size = file_end - tinfos[i].offset;
    }

    printf("Model: %s\n", argv[1]);
    printf("Tensors: %llu\n\n", (unsigned long long)n_tensors);

    QuickMask qm;
    qm_init(&qm);

    int n_scanned = 0;
    double sum_corr = 0;
    double min_corr = 1, max_corr = -1;

    printf("%-45s %8s %10s %8s %8s\n",
           "Tensor", "Type", "Size", "Cor01", "Cor02");
    printf("%-45s %8s %10s %8s %8s\n",
           "-------", "----", "----", "-----", "-----");

    for (uint64_t i = 0; i < n_tensors; i++) {
        if (tinfos[i].size < 34) continue;

        int8_t *buf = (int8_t*)calloc(GT + 256, 1);
        int nr = extract_values(f, tinfos[i].offset, tinfos[i].size,
                                tinfos[i].dtype, buf, GT);

        if (nr < GT) { free(buf); continue; }

        qm_encode(&qm, buf);

        double ent[3], corr[3];
        for (int m = 0; m < N_MASKS; m++)
            ent[m] = qm_entropy(qm.visible[m], qm.pin_count[m]);
        corr[0] = qm_correlation(qm.visible[0], qm.visible[1], qm.pin_count[0]);
        corr[1] = qm_correlation(qm.visible[0], qm.visible[2], qm.pin_count[0]);
        corr[2] = qm_correlation(qm.visible[1], qm.visible[2], qm.pin_count[1]);

        double avg_c = (corr[0] + corr[1] + corr[2]) / 3.0;
        sum_corr += avg_c;
        if (avg_c < min_corr) min_corr = avg_c;
        if (avg_c > max_corr) max_corr = avg_c;

        printf("%-45s %8s %10llu %8.4f %8.4f\n",
               tinfos[i].name, dtype_name(tinfos[i].dtype),
               (unsigned long long)tinfos[i].size, corr[0], corr[1]);

        n_scanned++;
        free(buf);
    }

    if (n_scanned > 0) {
        printf("\n============================================================\n");
        printf("  AGGREGATE — %d tensors scanned\n", n_scanned);
        printf("============================================================\n");
        printf("  Avg cross-mask correlation: %.4f\n", sum_corr / n_scanned);
        printf("  Min: %.4f  Max: %.4f\n", min_corr, max_corr);

        if (sum_corr / n_scanned < 0.1)
            printf("  => LOW correlation — masks see genuinely different data\n");
        else if (sum_corr / n_scanned < 0.5)
            printf("  => MODERATE — some shared structure\n");
        else
            printf("  => HIGH — pattern dominated\n");
    }

    fclose(f);
    free(tinfos);
    return 0;
}

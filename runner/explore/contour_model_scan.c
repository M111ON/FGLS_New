// contour_model_scan.c
// Scan ALL Q8_0 tensors in a GGUF model with Contour Mask
// Reveal: what do mask patterns tell us about model structure?
//
// For each tensor: 3 masks → 3 profiles → cross-mask correlation
// Aggregate: which layers have high/low correlation?
// What does correlation distribution tell us about the model?
//
// Compile: gcc -O2 -std=c11 -Wno-error=misleading-indentation -Wno-error=format -o contour_model_scan.exe contour_model_scan.c -lm
// Run:     contour_model_scan.exe model.gguf

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

// ============================================================
// Contour Mask — quick inline
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
// Main scan
// ============================================================
int main(int argc, char **argv) {
    if (argc < 2) { printf("Usage: contour_model_scan model.gguf\n"); return 1; }

    printf("============================================================\n");
    printf("  Contour Model Scan — Mask Patterns Reveal Model Structure\n");
    printf("  Grid: %dx%dx%d = %d | Masks: %d (non-overlapping)\n", GX, GY, GZ, GT, N_MASKS);
    printf("============================================================\n\n");

    FILE *f = gguf_fopen(argv[1]);
    if (!f) { printf("Cannot open %s\n", argv[1]); return 1; }

    uint32_t magic, version; uint64_t n_tensors, n_kv;
    fread(&magic, 4, 1, f); fread(&version, 4, 1, f);
    fread(&n_tensors, 8, 1, f); fread(&n_kv, 8, 1, f);
    if (magic != GGUF_MAGIC) { fclose(f); printf("Bad magic\n"); return 1; }

    skip_kv(f, n_kv);

    // Read all tensor info
    TensorInfo *tinfos = (TensorInfo*)calloc(n_tensors, sizeof(TensorInfo));
    if (!tinfos) { fclose(f); printf("tinfos alloc failed\n"); return 1; }
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nlen; fread(&nlen, 8, 1, f);
        fread(tinfos[i].name, nlen, 1, f); tinfos[i].name[nlen] = 0;
        uint32_t nd; fread(&nd, 4, 1, f);
        for (uint32_t j=0;j<nd;j++){uint64_t d;fread(&d,8,1,f);}
        fread(&tinfos[i].dtype, 4, 1, f);
        fread(&tinfos[i].offset, 8, 1, f);
    }

    // Compute sizes
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

    // Scan Q8_0 tensors
    QuickMask qm;
    qm_init(&qm);

    int n_scanned = 0;
    double *all_corr01 = NULL, *all_corr02 = NULL, *all_corr12 = NULL;
    double *all_ent = NULL;
    char **all_names = NULL;
    int max_tensors = n_tensors;
    all_corr01 = (double*)calloc(max_tensors, sizeof(double));
    all_corr02 = (double*)calloc(max_tensors, sizeof(double));
    all_corr12 = (double*)calloc(max_tensors, sizeof(double));
    all_ent = (double*)calloc(max_tensors * N_MASKS, sizeof(double));
    all_names = (char**)calloc(max_tensors, sizeof(char*));
    if (!all_corr01 || !all_corr02 || !all_corr12 || !all_ent || !all_names) {
        printf("alloc failed\n");
        free(all_corr01); free(all_corr02); free(all_corr12);
        free(all_ent); free(all_names); free(tinfos); fclose(f);
        return 1;
    }

    printf("%-45s %10s %8s %8s %8s %8s %8s\n",
           "Tensor", "Size", "Ent M0", "Ent M1", "Ent M2", "Cor01", "Cor02");
    printf("%-45s %10s %8s %8s %8s %8s %8s\n",
           "-------", "----", "------", "------", "------", "-----", "-----");

    for (uint64_t i = 0; i < n_tensors; i++) {
        if (tinfos[i].dtype != 8) continue;  // Q8_0 only
        if (tinfos[i].size < 34) continue;   // need at least 1 block

        // Read first GT int8 values from Q8_0 blocks
        int8_t *buf = (int8_t*)calloc(GT + 256, 1);
        fseek(f, tinfos[i].offset, SEEK_SET);
        int nr = 0;
        uint64_t nb = tinfos[i].size / 34;
        for (uint64_t b = 0; b < nb && nr < GT; b++) {
            fseek(f, 2, SEEK_CUR);
            for (int j = 0; j < 32 && nr < GT; j++) { fread(&buf[nr], 1, 1, f); nr++; }
        }

        if (nr < GT) { free(buf); continue; }

        // Encode through contour mask
        qm_encode(&qm, buf);

        // Analyze
        double ent[3], corr[3];
        for (int m = 0; m < N_MASKS; m++)
            ent[m] = qm_entropy(qm.visible[m], qm.pin_count[m]);
        corr[0] = qm_correlation(qm.visible[0], qm.visible[1], qm.pin_count[0]);
        corr[1] = qm_correlation(qm.visible[0], qm.visible[2], qm.pin_count[0]);
        corr[2] = qm_correlation(qm.visible[1], qm.visible[2], qm.pin_count[1]);

        all_names[n_scanned] = tinfos[i].name;
        all_corr01[n_scanned] = corr[0];
        all_corr02[n_scanned] = corr[1];
        all_corr12[n_scanned] = corr[2];
        for (int m = 0; m < N_MASKS; m++) all_ent[n_scanned*N_MASKS+m] = ent[m];

        printf("%-45s %10llu %8.3f %8.3f %8.3f %8.4f %8.4f\n",
               tinfos[i].name, (unsigned long long)tinfos[i].size,
               ent[0], ent[1], ent[2], corr[0], corr[1]);

        n_scanned++;
        free(buf);
    }

    // Aggregate stats
    if (n_scanned > 0) {
        double avg_c01=0, avg_c02=0, avg_c12=0;
        double min_c01=1, min_c02=1, min_c12=1;
        double max_c01=-1, max_c02=-1, max_c12=-1;
        double avg_ent[3] = {0,0,0};

        for (int i = 0; i < n_scanned; i++) {
            avg_c01 += all_corr01[i]; avg_c02 += all_corr02[i]; avg_c12 += all_corr12[i];
            if (all_corr01[i] < min_c01) min_c01 = all_corr01[i];
            if (all_corr02[i] < min_c02) min_c02 = all_corr02[i];
            if (all_corr12[i] < min_c12) min_c12 = all_corr12[i];
            if (all_corr01[i] > max_c01) max_c01 = all_corr01[i];
            if (all_corr02[i] > max_c02) max_c02 = all_corr02[i];
            if (all_corr12[i] > max_c12) max_c12 = all_corr12[i];
            for (int m = 0; m < N_MASKS; m++) avg_ent[m] += all_ent[i*N_MASKS+m];
        }
        int n = n_scanned;
        avg_c01/=n; avg_c02/=n; avg_c12/=n;
        for (int m = 0; m < N_MASKS; m++) avg_ent[m] /= n;

        printf("\n============================================================\n");
        printf("  AGGREGATE — %d Q8_0 tensors scanned\n", n_scanned);
        printf("============================================================\n");

        printf("\n  Cross-mask correlation:\n");
        printf("    Mask 0-1: avg=%.4f min=%.4f max=%.4f\n", avg_c01, min_c01, max_c01);
        printf("    Mask 0-2: avg=%.4f min=%.4f max=%.4f\n", avg_c02, min_c02, max_c02);
        printf("    Mask 1-2: avg=%.4f min=%.4f max=%.4f\n", avg_c12, min_c12, max_c12);

        printf("\n  Entropy per mask:\n");
        for (int m = 0; m < N_MASKS; m++)
            printf("    Mask %d: avg=%.3f bits\n", m, avg_ent[m]);

        printf("\n  Interpretation:\n");
        if (avg_c01 < 0.1 && avg_c02 < 0.1 && avg_c12 < 0.1)
            printf("    LOW correlation → masks see genuinely different data\n");
        else if (avg_c01 < 0.5)
            printf("    MODERATE correlation → some shared structure\n");
        else
            printf("    HIGH correlation → masks see similar data (pattern dominated)\n");

        // Find tensors with lowest correlation (most "viewable")
        printf("\n  Top 5 most viewable tensors (lowest avg correlation):\n");
        // Build sorted index
        int *idx = (int*)calloc(n, sizeof(int));
        double *avg_c = (double*)calloc(n, sizeof(double));
        for (int i = 0; i < n; i++) {
            idx[i] = i;
            avg_c[i] = (all_corr01[i] + all_corr02[i] + all_corr12[i]) / 3.0;
        }
        // Simple sort
        for (int i = 0; i < n; i++)
            for (int j = i+1; j < n; j++)
                if (avg_c[idx[j]] < avg_c[idx[i]]) { int t=idx[i]; idx[i]=idx[j]; idx[j]=t; }

        for (int i = 0; i < 5 && i < n; i++) {
            int k = idx[i];
            printf("    %d. %s (avg_corr=%.4f)\n", i+1, all_names[k], avg_c[k]);
        }

        free(idx); free(avg_c);
    }

    free(all_corr01); free(all_corr02); free(all_corr12);
    free(all_ent); free(all_names);
    fclose(f);
    free(tinfos);

    return 0;
}

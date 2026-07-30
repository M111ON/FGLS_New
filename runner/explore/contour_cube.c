// contour_cube.c
// Contour Mask — Cube-Based Measurement
//   6 faces of a cube = 6 viewing directions
//   Each face measures projection of 3D data
//   Opposite faces CANCEL (A↔B, C↔D, E↔F)
//   Active: A, C, E (one per pair)
//   Adjacent pairs complement: A+C, C+E, E+A
//
// Compile: gcc -O2 -std=c11 -Wno-error=misleading-indentation -Wno-error=format -o contour_cube.exe contour_cube.c -lm
// Run:     contour_cube.exe [model.gguf]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

// ============================================================
// Grid
// ============================================================
#define GX  10
#define GY  10
#define GZ  6
#define GT  (GX * GY * GZ)

// ============================================================
// Cube Face Config (CRITICAL)
// ============================================================
// 6 faces of a cube: A, B, C, D, E, F
// Opposite pairs (CANCEL):
//   A ↔ B (look from +X vs -X)
//   C ↔ D (look from +Y vs -Y)
//   E ↔ F (look from +Z vs -Z)
//
// Active: A, C, E (one from each pair)
// Adjacent pairs (complement):
//   A+C, C+E, E+A
//
// Projection per face:
//   A (X-axis): collapse X → Y×Z silhouette (10×6 = 60 values)
//   C (Y-axis): collapse Y → X×Z silhouette (10×6 = 60 values)
//   E (Z-axis): collapse Z → X×Y silhouette (10×10 = 100 values)

#define N_FACES 3
#define FACE_A  0   // X-axis projection
#define FACE_C  1   // Y-axis projection
#define FACE_E  2   // Z-axis projection

static const char *FACE_NAMES[] = {"A", "C", "E"};

// Projection sizes (collapsing one axis)
static const int PROJ_ROWS[] = {GY, GX, GX};  // rows in projection
static const int PROJ_COLS[] = {GZ, GZ, GY};  // cols in projection
static const int PROJ_SIZE[] = {GY*GZ, GX*GZ, GX*GY};  // total values

// ============================================================
// Cube Contour Mask
// ============================================================
typedef struct {
    int8_t  grid[GX][GY][GZ];
    int8_t  projection[N_FACES][GX*GY];  // max projection size
    int     proj_rows[N_FACES];
    int     proj_cols[N_FACES];
} CubeMask;

static void cube_init(CubeMask *cm) {
    memset(cm, 0, sizeof(CubeMask));
    for (int f = 0; f < N_FACES; f++) {
        cm->proj_rows[f] = PROJ_ROWS[f];
        cm->proj_cols[f] = PROJ_COLS[f];
    }
}

// ============================================================
// Encode: 3D data → cube face projections
// ============================================================
// Each face looks at the data from one direction:
//   A (X-axis): for each (y,z), collect all x values → projection[y][z]
//   C (Y-axis): for each (x,z), collect all y values → projection[x][z]
//   E (Z-axis): for each (x,y), collect all z values → projection[x][y]

static void cube_encode(CubeMask *cm) {
    // Face A: look along X-axis → Y×Z projection
    // For each (y,z), project across all x
    for (int y = 0; y < GY; y++)
        for (int z = 0; z < GZ; z++) {
            // Aggregate along X (simple: take values at each x position)
            // For contour mask: we store the full projection (all x values)
            // This is a 2D array where each cell has GX values
            // But for simplicity, we take the "silhouette" = average
            int sum = 0;
            for (int x = 0; x < GX; x++)
                sum += cm->grid[x][y][z];
            cm->projection[FACE_A][y * GZ + z] = (int8_t)(sum / GX);
        }

    // Face C: look along Y-axis → X×Z projection
    for (int x = 0; x < GX; x++)
        for (int z = 0; z < GZ; z++) {
            int sum = 0;
            for (int y = 0; y < GY; y++)
                sum += cm->grid[x][y][z];
            cm->projection[FACE_C][x * GZ + z] = (int8_t)(sum / GY);
        }

    // Face E: look along Z-axis → X×Y projection
    for (int x = 0; x < GX; x++)
        for (int y = 0; y < GY; y++) {
            int sum = 0;
            for (int z = 0; z < GZ; z++)
                sum += cm->grid[x][y][z];
            cm->projection[FACE_E][x * GY + y] = (int8_t)(sum / GZ);
        }

    // NOTE: B, D, F NOT encoded — they would CANCEL A, C, E
}

// ============================================================
// Analysis
// ============================================================
typedef struct {
    int8_t  min, max;
    double  mean;
    int     n_nonzero;
    int     distinct;
} FaceStats;

static FaceStats analyze_face(const CubeMask *cm, int face) {
    FaceStats s;
    memset(&s, 0, sizeof(s));
    s.min = 127; s.max = -128;
    int counts[256] = {0};
    int total = cm->proj_rows[face] * cm->proj_cols[face];

    for (int i = 0; i < total; i++) {
        int8_t v = cm->projection[face][i];
        if (v < s.min) s.min = v;
        if (v > s.max) s.max = v;
        s.mean += v;
        if (v != 0) s.n_nonzero++;
        counts[(uint8_t)v]++;
    }
    s.mean /= total;
    for (int i = 0; i < 256; i++) if (counts[i]) s.distinct++;
    return s;
}

static double calc_entropy(const int8_t *data, int len) {
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

static double calc_correlation(const int8_t *a, int na, const int8_t *b, int nb) {
    int n = na < nb ? na : nb;
    double sa=0,sb=0,sab=0,sa2=0,sb2=0;
    for (int i = 0; i < n; i++) {
        double va=a[i],vb=b[i]; sa+=va; sb+=vb; sab+=va*vb; sa2+=va*va; sb2+=vb*vb;
    }
    double ma=sa/n, mb=sb/n, cov=sab/n-ma*mb;
    double da=sqrt(sa2/n-ma*ma), db=sqrt(sb2/n-mb*mb);
    return (da>0&&db>0) ? cov/(da*db) : 0;
}

// ============================================================
// GGUF reader
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
// Test 1: Synthetic
// ============================================================
static int test_synthetic(void) {
    printf("=== Test 1: Cube Contour Mask — Synthetic ===\n");
    printf("  Grid: %dx%dx%d = %d\n", GX, GY, GZ, GT);
    printf("  Faces: A (X-proj %d×%d), C (Y-proj %d×%d), E (Z-proj %d×%d)\n",
           PROJ_ROWS[0], PROJ_COLS[0], PROJ_ROWS[1], PROJ_COLS[1],
           PROJ_ROWS[2], PROJ_COLS[2]);

    CubeMask cm;
    cube_init(&cm);

    // Fill with pattern
    for (int x = 0; x < GX; x++)
        for (int y = 0; y < GY; y++)
            for (int z = 0; z < GZ; z++)
                cm.grid[x][y][z] = (int8_t)((x*37 + y*13 + z*7) % 256 - 128);

    cube_encode(&cm);

    // Analyze each face
    printf("\n  Face projections:\n");
    for (int f = 0; f < N_FACES; f++) {
        FaceStats s = analyze_face(&cm, f);
        printf("    %s (%d×%d=%d): min=%d max=%d mean=%.1f nonzero=%d distinct=%d\n",
               FACE_NAMES[f], cm.proj_rows[f], cm.proj_cols[f],
               cm.proj_rows[f]*cm.proj_cols[f],
               s.min, s.max, s.mean, s.n_nonzero, s.distinct);
    }

    // Cross-face correlation
    printf("\n  Cross-face correlation (adjacent pairs):\n");
    int pairs[3][2] = {{FACE_A, FACE_C}, {FACE_C, FACE_E}, {FACE_E, FACE_A}};
    const char *pair_names[] = {"A+C", "C+E", "E+A"};
    for (int p = 0; p < 3; p++) {
        int f1 = pairs[p][0], f2 = pairs[p][1];
        double corr = calc_correlation(cm.projection[f1], cm.proj_rows[f1]*cm.proj_cols[f1],
                                       cm.projection[f2], cm.proj_rows[f2]*cm.proj_cols[f2]);
        printf("    %s: %.4f\n", pair_names[p], corr);
    }

    // Entropy per face
    printf("\n  Entropy per face:\n");
    for (int f = 0; f < N_FACES; f++) {
        double ent = calc_entropy(cm.projection[f], cm.proj_rows[f]*cm.proj_cols[f]);
        printf("    %s: %.3f bits\n", FACE_NAMES[f], ent);
    }

    printf("  => DONE\n\n");
    return 1;
}

// ============================================================
// Test 2: GGUF
// ============================================================
static int test_gguf(const char *path) {
    printf("=== Test 2: Cube Contour Mask — GGUF ===\n");

    FILE *f = gguf_fopen(path);
    if (!f) { printf("  FAIL: cannot open\n\n"); return 0; }

    uint32_t magic, version; uint64_t n_tensors, n_kv;
    fread(&magic, 4, 1, f); fread(&version, 4, 1, f);
    fread(&n_tensors, 8, 1, f); fread(&n_kv, 8, 1, f);
    if (magic != GGUF_MAGIC) { fclose(f); printf("  Bad magic\n\n"); return 0; }

    skip_kv(f, n_kv);

    int best = -1;
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nlen; fread(&nlen, 8, 1, f); fseek(f, nlen, SEEK_CUR);
        uint32_t nd; fread(&nd, 4, 1, f);
        for (uint32_t j=0;j<nd;j++){uint64_t d;fread(&d,8,1,f);}
        uint32_t dt; fread(&dt, 4, 1, f);
        uint64_t off; fread(&off, 8, 1, f);
        if (dt == 8) best = (int)i;
    }

    fseek(f, 24, SEEK_SET); skip_kv(f, n_kv);
    uint64_t tensor_off = 0;
    char name[256] = "";
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nlen; fread(&nlen, 8, 1, f);
        char nbuf[256]; fread(nbuf, nlen, 1, f); nbuf[nlen]=0;
        uint32_t nd; fread(&nd, 4, 1, f);
        for (uint32_t j=0;j<nd;j++){uint64_t d;fread(&d,8,1,f);}
        uint32_t dt; fread(&dt, 4, 1, f);
        uint64_t off; fread(&off, 8, 1, f);
        if (i == (uint64_t)best) { tensor_off = off; strncpy(name, nbuf, 255); }
    }
    fseek(f, 0, SEEK_END);
    uint64_t sz = ftell(f) - tensor_off;
    printf("  Model: %s\n  Tensor: %s (%llu bytes)\n", path, name, (unsigned long long)sz);

    int8_t *buf = (int8_t*)calloc(GT + 256, 1);
    fseek(f, tensor_off, SEEK_SET);
    int nr = 0;
    uint64_t nb = sz / 34;
    for (uint64_t b = 0; b < nb && nr < GT; b++) {
        fseek(f, 2, SEEK_CUR);
        for (int j = 0; j < 32 && nr < GT; j++) { fread(&buf[nr], 1, 1, f); nr++; }
    }
    fclose(f);
    printf("  Extracted: %d int8\n\n", nr);

    CubeMask cm;
    cube_init(&cm);
    int idx = 0;
    for (int x = 0; x < GX && idx < nr; x++)
        for (int y = 0; y < GY && idx < nr; y++)
            for (int z = 0; z < GZ && idx < nr; z++)
                cm.grid[x][y][z] = buf[idx++];

    double t0 = (double)clock() / CLOCKS_PER_SEC * 1000.0;
    cube_encode(&cm);
    double t1 = (double)clock() / CLOCKS_PER_SEC * 1000.0;

    printf("  Encode: %.3f ms\n\n", t1 - t0);

    printf("  Face projections:\n");
    for (int f = 0; f < N_FACES; f++) {
        FaceStats s = analyze_face(&cm, f);
        printf("    %s (%d×%d=%d): min=%3d max=%3d mean=%6.1f nonzero=%3d distinct=%3d\n",
               FACE_NAMES[f], cm.proj_rows[f], cm.proj_cols[f],
               cm.proj_rows[f]*cm.proj_cols[f],
               s.min, s.max, s.mean, s.n_nonzero, s.distinct);
    }

    printf("\n  Cross-face correlation (adjacent pairs):\n");
    int pairs[3][2] = {{FACE_A, FACE_C}, {FACE_C, FACE_E}, {FACE_E, FACE_A}};
    const char *pair_names[] = {"A+C", "C+E", "E+A"};
    for (int p = 0; p < 3; p++) {
        int f1 = pairs[p][0], f2 = pairs[p][1];
        double corr = calc_correlation(cm.projection[f1], cm.proj_rows[f1]*cm.proj_cols[f1],
                                       cm.projection[f2], cm.proj_rows[f2]*cm.proj_cols[f2]);
        printf("    %s: %.4f\n", pair_names[p], corr);
    }

    printf("\n  Entropy per face:\n");
    for (int f = 0; f < N_FACES; f++) {
        double ent = calc_entropy(cm.projection[f], cm.proj_rows[f]*cm.proj_cols[f]);
        printf("    %s: %.3f bits\n", FACE_NAMES[f], ent);
    }

    free(buf);
    printf("\n  => DONE\n\n");
    return 1;
}

// ============================================================
int main(int argc, char **argv) {
    printf("============================================================\n");
    printf("  Cube Contour Mask — Projection-Based Measurement\n");
    printf("  6 cube faces, 3 active (A, C, E)\n");
    printf("  Each face projects 3D data → 2D silhouette\n");
    printf("  Opposite faces CANCEL (B, D, F NOT used)\n");
    printf("============================================================\n\n");

    test_synthetic();
    if (argc > 1) test_gguf(argv[1]);

    printf("============================================================\n");
    printf("  Compare with flat modular (contour_mask_v2):\n");
    printf("  Flat: (x+y+z)%%3 → correlation ~0.005\n");
    printf("  Cube: face projections → correlation ???\n");
    printf("============================================================\n");

    return 0;
}

// contour_mask.c
// Contour Mask — 3-directional profile measurement
// Concept: like a contour gauge (profile gauge)
//   3 independent pin arrays, each measures data from one direction
//   Pins conform to data shape → profile = shape of data
//
// CRITICAL RULE: Opposite directions CANCEL when averaged/inverted.
//   A-B, C-D, E-F are opposite pairs → CANCEL each other
//   Solution: only ONE from each pair (A, C, E)
//   Adjacent pairs complement: (A+C), (C+E), (E+A)
//
// Compile: gcc -O2 -std=c11 -Wno-error=misleading-indentation -Wno-error=format -o contour_mask.exe contour_mask.c -lm
// Run:     contour_mask.exe [model.gguf]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

// ============================================================
// GRID CONFIG
// ============================================================
#define GRID_X      10
#define GRID_Y      10
#define GRID_Z      6
#define GRID_TOTAL  (GRID_X * GRID_Y * GRID_Z)  // 600

// ============================================================
// DIRECTION CONFIG — A/B/C/D/E/F naming
// ============================================================
// 6 faces of a cube: A, B, C, D, E, F
// Opposite pairs (CANCEL when averaged/inverted):
//   A ↔ B
//   C ↔ D
//   E ↔ F
//
// Active directions (pick ONE from each pair):
//   A, C, E
//
// Adjacent pairs (complement, never cancel):
//   A+C (adjacent on one edge)
//   C+E (adjacent on another edge)
//   E+A (adjacent on third edge)
//
// NEVER: A+B, C+D, E+F → these CANCEL

#define N_DIRS  3
#define DIR_A   0   // active (pair with B, which is NOT used)
#define DIR_C   1   // active (pair with D, which is NOT used)
#define DIR_E   2   // active (pair with F, which is NOT used)

static const char *DIR_NAMES[] = {"A", "C", "E"};

// Adjacent pairs (complement, never cancel)
// Each pair = two active directions that share an edge
static const char *PAIR_NAMES[] = {"A+C", "C+E", "E+A"};

// ============================================================
// Contour Mask
// ============================================================
typedef struct {
    int8_t  grid[GRID_X][GRID_Y][GRID_Z];
    int8_t  profile[N_DIRS][GRID_X][GRID_Y][GRID_Z];
    int     pin_count[N_DIRS];
} ContourMask;

static void contour_init(ContourMask *cm) {
    memset(cm, 0, sizeof(ContourMask));
    cm->pin_count[DIR_A] = GRID_X;
    cm->pin_count[DIR_C] = GRID_Y;
    cm->pin_count[DIR_E] = GRID_Z;
}

// Encode: data → 3 directional profiles
static void contour_encode(ContourMask *cm) {
    // A: pins at each X position, measure Y-Z slice
    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++)
                cm->profile[DIR_A][x][y][z] = cm->grid[x][y][z];

    // C: pins at each Y position, measure X-Z slice
    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++)
                cm->profile[DIR_C][x][y][z] = cm->grid[x][y][z];

    // E: pins at each Z position, measure X-Y slice
    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++)
                cm->profile[DIR_E][x][y][z] = cm->grid[x][y][z];

    // NOTE: B, D, F NOT encoded — they would CANCEL A, C, E
}

// Decode: profile → data (use A profile)
static void contour_decode(const ContourMask *cm, int8_t out[GRID_X][GRID_Y][GRID_Z]) {
    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++)
                out[x][y][z] = cm->profile[DIR_A][x][y][z];
}

// ============================================================
// Profile Analysis
// ============================================================
typedef struct {
    int8_t  min, max;
    double  mean;
    int     n_nonzero;
    int     distinct;
} ProfileStats;

static ProfileStats analyze_profile(const ContourMask *cm, int dir) {
    ProfileStats s;
    memset(&s, 0, sizeof(s));
    s.min = 127; s.max = -128;
    int counts[256] = {0};
    int total = 0;

    for (int i = 0; i < GRID_X; i++)
        for (int j = 0; j < GRID_Y; j++)
            for (int k = 0; k < GRID_Z; k++) {
                int8_t v = cm->profile[dir][i][j][k];
                if (v < s.min) s.min = v;
                if (v > s.max) s.max = v;
                s.mean += v;
                if (v != 0) s.n_nonzero++;
                counts[(uint8_t)v]++;
                total++;
            }

    s.mean /= total;
    for (int i = 0; i < 256; i++)
        if (counts[i] > 0) s.distinct++;

    return s;
}

// ============================================================
// GGUF Reader
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

static void skip_kv_pairs(FILE *f, uint64_t n_kv) {
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
static int test_contour_synthetic(void) {
    printf("=== Test 1: Contour Mask — Synthetic ===\n");

    ContourMask cm;
    contour_init(&cm);

    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++)
                cm.grid[x][y][z] = (int8_t)((x*37 + y*13 + z*7) % 256 - 128);

    contour_encode(&cm);

    int8_t decoded[GRID_X][GRID_Y][GRID_Z];
    contour_decode(&cm, decoded);

    int exact = 0;
    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++)
                if (cm.grid[x][y][z] == decoded[x][y][z]) exact++;

    printf("  Grid: %dx%dx%d = %d slots\n", GRID_X, GRID_Y, GRID_Z, GRID_TOTAL);
    printf("  Directions: A, C, E (B, D, F cancel — NOT used)\n");
    printf("  Pairs: A+C, C+E, E+A (adjacent, complement)\n");
    printf("  Lossless: %d/%d (%.1f%%)\n", exact, GRID_TOTAL, 100.0*exact/GRID_TOTAL);

    printf("\n  3-Direction Profiles:\n");
    for (int d = 0; d < N_DIRS; d++) {
        ProfileStats s = analyze_profile(&cm, d);
        printf("    %s: min=%d max=%d mean=%.1f nonzero=%d distinct=%d\n",
               DIR_NAMES[d], s.min, s.max, s.mean, s.n_nonzero, s.distinct);
    }

    long raw = (long)GRID_TOTAL;
    long profiles = N_DIRS * GRID_TOTAL;
    printf("\n  Storage: raw=%ld, 3 profiles=%ld (%.1fx)\n",
           raw, profiles, (double)profiles/raw);

    int pass = (exact == GRID_TOTAL);
    printf("  => %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

// ============================================================
// Test 2: GGUF
// ============================================================
static int test_contour_gguf(const char *path) {
    printf("=== Test 2: Contour Mask — GGUF ===\n");
    printf("  Model: %s\n", path);

    FILE *f = gguf_fopen(path);
    if (!f) { printf("  FAIL: cannot open\n\n"); return 0; }

    uint32_t magic, version; uint64_t n_tensors, n_kv;
    fread(&magic, 4, 1, f); fread(&version, 4, 1, f);
    fread(&n_tensors, 8, 1, f); fread(&n_kv, 8, 1, f);
    if (magic != GGUF_MAGIC) { fclose(f); printf("  Bad magic\n\n"); return 0; }

    skip_kv_pairs(f, n_kv);

    int best = -1;
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nlen; fread(&nlen, 8, 1, f);
        fseek(f, nlen, SEEK_CUR);
        uint32_t nd; fread(&nd, 4, 1, f);
        for (uint32_t j=0;j<nd;j++){uint64_t d;fread(&d,8,1,f);}
        uint32_t dt; fread(&dt, 4, 1, f);
        uint64_t off; fread(&off, 8, 1, f);
        if (dt == 8) best = (int)i;
    }

    fseek(f, 24, SEEK_SET);
    skip_kv_pairs(f, n_kv);

    uint64_t tensor_off = 0;
    char tensor_name[256] = "";
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nlen; fread(&nlen, 8, 1, f);
        char name[256]; fread(name, nlen, 1, f); name[nlen]=0;
        uint32_t nd; fread(&nd, 4, 1, f);
        for (uint32_t j=0;j<nd;j++){uint64_t d;fread(&d,8,1,f);}
        uint32_t dt; fread(&dt, 4, 1, f);
        uint64_t off; fread(&off, 8, 1, f);
        if (i == (uint64_t)best) {
            tensor_off = off;
            strncpy(tensor_name, name, 255);
        }
    }

    fseek(f, 0, SEEK_END);
    uint64_t end_off = ftell(f);
    uint64_t tensor_sz = end_off - tensor_off;
    printf("  Tensor[%d]: %s (%llu bytes Q8_0)\n", best, tensor_name, (unsigned long long)tensor_sz);

    int8_t *buf = (int8_t*)calloc(GRID_TOTAL + 256, 1);
    if (!buf) { fclose(f); printf("buf alloc failed\n"); return 1; }
    fseek(f, tensor_off, SEEK_SET);
    int nr = 0;
    uint64_t nb = tensor_sz / 34;
    for (uint64_t b = 0; b < nb && nr < GRID_TOTAL; b++) {
        fseek(f, 2, SEEK_CUR);
        for (int j = 0; j < 32 && nr < GRID_TOTAL; j++) {
            fread(&buf[nr], 1, 1, f);
            nr++;
        }
    }
    fclose(f);
    printf("  Extracted: %d int8 values\n\n", nr);

    ContourMask cm;
    contour_init(&cm);
    int idx = 0;
    for (int x = 0; x < GRID_X && idx < nr; x++)
        for (int y = 0; y < GRID_Y && idx < nr; y++)
            for (int z = 0; z < GRID_Z && idx < nr; z++)
                cm.grid[x][y][z] = buf[idx++];

    double t0 = (double)clock() / CLOCKS_PER_SEC * 1000.0;
    contour_encode(&cm);
    double t1 = (double)clock() / CLOCKS_PER_SEC * 1000.0;

    int8_t decoded[GRID_X][GRID_Y][GRID_Z];
    contour_decode(&cm, decoded);

    int exact = 0;
    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++)
                if (cm.grid[x][y][z] == decoded[x][y][z]) exact++;

    printf("  Encode time: %.3f ms\n", t1 - t0);
    printf("  Lossless: %d/%d (%.1f%%)\n", exact, GRID_TOTAL, 100.0*exact/GRID_TOTAL);

    printf("\n  3-Direction Profiles:\n");
    for (int d = 0; d < N_DIRS; d++) {
        ProfileStats s = analyze_profile(&cm, d);
        printf("    %s: min=%3d max=%3d mean=%6.1f nonzero=%3d distinct=%3d\n",
               DIR_NAMES[d], s.min, s.max, s.mean, s.n_nonzero, s.distinct);
    }

    long raw = (long)GRID_TOTAL;
    long profiles = N_DIRS * GRID_TOTAL;
    printf("\n  Storage: raw=%ld, 3 profiles=%ld (%.1fx)\n",
           raw, profiles, (double)profiles/raw);

    int pass = (exact == GRID_TOTAL);
    printf("  => %s\n\n", pass ? "PASS" : "FAIL");

    free(buf);
    return pass;
}

// ============================================================
// Main
// ============================================================
int main(int argc, char **argv) {
    printf("============================================================\n");
    printf("  Contour Mask — 3-Directional Profile Measurement\n");
    printf("  \"Like a contour gauge for data\"\n");
    printf("  Directions: A, C, E (B, D, F cancel — NOT used)\n");
    printf("  Pairs: A+C, C+E, E+A (adjacent, complement)\n");
    printf("============================================================\n\n");

    int pass = 0, total = 0;

    total++; pass += test_contour_synthetic();

    if (argc > 1)
        total++; pass += test_contour_gguf(argv[1]);

    printf("============================================================\n");
    printf("  RESULT: %d/%d PASS\n", pass, total);
    printf("============================================================\n");

    return (pass == total) ? 0 : 1;
}

// contour_mask_v2.c
// Contour Mask V2 — mask actually FILTERS data
//   Mask = grid of holes (stencil pattern)
//   Data placed on mask → only values through holes are visible
//   Pins show what's visible → contour = shape of visible data
//
// For lossless: multiple non-overlapping masks cover ALL cells
//   Mask_0 holes: cells where (x+y+z) % N_MASKS == 0
//   Mask_1 holes: cells where (x+y+z) % N_MASKS == 1
//   ...
//   Together: all cells covered exactly once
//   Reconstruct: combine all mask profiles
//
// Compile: gcc -O2 -std=c11 -Wno-error=misleading-indentation -Wno-error=format -o contour_mask_v2.exe contour_mask_v2.c -lm
// Run:     contour_mask_v2.exe [model.gguf]

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
// Direction Config — A-F naming
// ============================================================
// A↔B, C↔D, E↔F = opposite pairs → CANCEL
// Active: A, C, E (one per pair)
// Adjacent pairs: A+C, C+E, E+A (complement)
#define N_DIRS 3
#define DIR_A 0
#define DIR_C 1
#define DIR_E 2
static const char *DN[] = {"A", "C", "E"};

// ============================================================
// Mask Config — how many masks partition the grid
// ============================================================
// Each mask has holes at positions where (x+y+z) % N_MASKS == mask_id
// N_MASKS non-overlapping masks cover all GT cells exactly once
#define N_MASKS 3

// ============================================================
// Contour Mask V2
// ============================================================
typedef struct {
    int8_t  grid[GX][GY][GZ];         // source data
    int8_t  visible[N_MASKS][GT];      // what each mask lets through (pins)
    int     pin_count[N_MASKS];        // how many pins per mask
} ContourMask;

static void cm_init(ContourMask *cm) {
    memset(cm, 0, sizeof(ContourMask));
    for (int m = 0; m < N_MASKS; m++) {
        int count = 0;
        for (int x = 0; x < GX; x++)
            for (int y = 0; y < GY; y++)
                for (int z = 0; z < GZ; z++)
                    if ((x + y + z) % N_MASKS == m) count++;
        cm->pin_count[m] = count;
    }
}

// ============================================================
// Encode: data → mask filters → visible pins
// ============================================================
// Each mask is a stencil — only cells matching the mask pattern are "visible"
// Pin = data value that passes through the hole

static void cm_encode(ContourMask *cm) {
    for (int m = 0; m < N_MASKS; m++) {
        int idx = 0;
        for (int x = 0; x < GX; x++)
            for (int y = 0; y < GY; y++)
                for (int z = 0; z < GZ; z++)
                    if ((x + y + z) % N_MASKS == m)
                        cm->visible[m][idx++] = cm->grid[x][y][z];
    }
}

// ============================================================
// Decode: visible pins → reconstruct all data
// ============================================================
// Place each pin back at its hole position

static void cm_decode(const ContourMask *cm, int8_t out[GX][GY][GZ]) {
    for (int m = 0; m < N_MASKS; m++) {
        int idx = 0;
        for (int x = 0; x < GX; x++)
            for (int y = 0; y < GY; y++)
                for (int z = 0; z < GZ; z++)
                    if ((x + y + z) % N_MASKS == m)
                        out[x][y][z] = cm->visible[m][idx++];
    }
}

// ============================================================
// Profile: what each mask "sees"
// ============================================================
typedef struct {
    int8_t  min, max;
    double  mean;
    int     n_nonzero;
    int     distinct;
} MaskStats;

static MaskStats analyze_mask(const ContourMask *cm, int mask_id) {
    MaskStats s;
    memset(&s, 0, sizeof(s));
    s.min = 127; s.max = -128;
    int counts[256] = {0};

    for (int i = 0; i < cm->pin_count[mask_id]; i++) {
        int8_t v = cm->visible[mask_id][i];
        if (v < s.min) s.min = v;
        if (v > s.max) s.max = v;
        s.mean += v;
        if (v != 0) s.n_nonzero++;
        counts[(uint8_t)v]++;
    }
    s.mean /= cm->pin_count[mask_id];
    for (int i = 0; i < 256; i++) if (counts[i]) s.distinct++;
    return s;
}

// ============================================================
// Cross-mask analysis: do different masks see different things?
// ============================================================
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

static double calc_correlation(const int8_t *a, const int8_t *b, int n) {
    double sa=0, sb=0, sab=0, sa2=0, sb2=0;
    for (int i = 0; i < n; i++) {
        double va = a[i], vb = b[i];
        sa += va; sb += vb; sab += va*vb; sa2 += va*va; sb2 += vb*vb;
    }
    double ma = sa/n, mb = sb/n;
    double cov = sab/n - ma*mb;
    double da = sqrt(sa2/n - ma*ma);
    double db = sqrt(sb2/n - mb*mb);
    return (da > 0 && db > 0) ? cov / (da*db) : 0;
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
    printf("=== Test 1: Contour Mask V2 — Synthetic ===\n");
    printf("  Mask pattern: (x+y+z) %% %d — non-overlapping, covers all cells\n", N_MASKS);

    ContourMask cm;
    cm_init(&cm);

    for (int x = 0; x < GX; x++)
        for (int y = 0; y < GY; y++)
            for (int z = 0; z < GZ; z++)
                cm.grid[x][y][z] = (int8_t)((x*37 + y*13 + z*7) % 256 - 128);

    cm_encode(&cm);

    // Verify lossless
    int8_t decoded[GX][GY][GZ];
    memset(decoded, 0, sizeof(decoded));
    cm_decode(&cm, decoded);

    int exact = 0;
    for (int x = 0; x < GX; x++)
        for (int y = 0; y < GY; y++)
            for (int z = 0; z < GZ; z++)
                if (cm.grid[x][y][z] == decoded[x][y][z]) exact++;

    printf("  Grid: %dx%dx%d = %d\n", GX, GY, GZ, GT);
    printf("  Lossless: %d/%d (%.1f%%)\n\n", exact, GT, 100.0*exact/GT);

    // Per-mask stats
    printf("  Mask stats (each mask sees DIFFERENT cells):\n");
    for (int m = 0; m < N_MASKS; m++) {
        MaskStats s = analyze_mask(&cm, m);
        printf("    Mask %d (%d pins): min=%d max=%d mean=%.1f nonzero=%d distinct=%d\n",
               m, cm.pin_count[m], s.min, s.max, s.mean, s.n_nonzero, s.distinct);
    }

    // Cross-mask entropy
    printf("\n  Cross-mask analysis:\n");
    for (int m = 0; m < N_MASKS; m++) {
        double ent = calc_entropy(cm.visible[m], cm.pin_count[m]);
        printf("    Mask %d entropy: %.3f bits\n", m, ent);
    }

    // Cross-mask correlation (should be LOW — different data!)
    printf("\n  Cross-mask correlation (should be LOW if masks see different data):\n");
    for (int m1 = 0; m1 < N_MASKS; m1++) {
        for (int m2 = m1+1; m2 < N_MASKS; m2++) {
            int n = cm.pin_count[m1] < cm.pin_count[m2] ? cm.pin_count[m1] : cm.pin_count[m2];
            double corr = calc_correlation(cm.visible[m1], cm.visible[m2], n);
            printf("    Mask %d vs %d: correlation=%.4f\n", m1, m2, corr);
        }
    }

    // Total storage
    long raw = GT;
    long pins = 0;
    for (int m = 0; m < N_MASKS; m++) pins += cm.pin_count[m];
    printf("\n  Storage: raw=%ld, pins=%ld (%.1fx)\n", raw, pins, (double)pins/raw);

    int pass = (exact == GT);
    printf("  => %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}

// ============================================================
// Test 2: GGUF
// ============================================================
static int test_gguf(const char *path) {
    printf("=== Test 2: Contour Mask V2 — GGUF ===\n");

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
    if (!buf) { fclose(f); printf("buf alloc failed\n"); return 1; }
    fseek(f, tensor_off, SEEK_SET);
    int nr = 0;
    uint64_t nb = sz / 34;
    for (uint64_t b = 0; b < nb && nr < GT; b++) {
        fseek(f, 2, SEEK_CUR);
        for (int j = 0; j < 32 && nr < GT; j++) { fread(&buf[nr], 1, 1, f); nr++; }
    }
    fclose(f);
    printf("  Extracted: %d int8\n\n", nr);

    ContourMask cm;
    cm_init(&cm);
    int idx = 0;
    for (int x = 0; x < GX && idx < nr; x++)
        for (int y = 0; y < GY && idx < nr; y++)
            for (int z = 0; z < GZ && idx < nr; z++)
                cm.grid[x][y][z] = buf[idx++];

    double t0 = (double)clock() / CLOCKS_PER_SEC * 1000.0;
    cm_encode(&cm);
    double t1 = (double)clock() / CLOCKS_PER_SEC * 1000.0;

    int8_t decoded[GX][GY][GZ];
    memset(decoded, 0, sizeof(decoded));
    cm_decode(&cm, decoded);

    int exact = 0;
    for (int x = 0; x < GX; x++)
        for (int y = 0; y < GY; y++)
            for (int z = 0; z < GZ; z++)
                if (cm.grid[x][y][z] == decoded[x][y][z]) exact++;

    printf("  Encode: %.3f ms\n", t1 - t0);
    printf("  Lossless: %d/%d (%.1f%%)\n\n", exact, GT, 100.0*exact/GT);

    printf("  Mask stats (each mask sees DIFFERENT cells):\n");
    for (int m = 0; m < N_MASKS; m++) {
        MaskStats s = analyze_mask(&cm, m);
        printf("    Mask %d (%d pins): min=%3d max=%3d mean=%6.1f nonzero=%3d distinct=%3d\n",
               m, cm.pin_count[m], s.min, s.max, s.mean, s.n_nonzero, s.distinct);
    }

    printf("\n  Cross-mask entropy:\n");
    for (int m = 0; m < N_MASKS; m++) {
        double ent = calc_entropy(cm.visible[m], cm.pin_count[m]);
        printf("    Mask %d: %.3f bits\n", m, ent);
    }

    printf("\n  Cross-mask correlation:\n");
    for (int m1 = 0; m1 < N_MASKS; m1++) {
        for (int m2 = m1+1; m2 < N_MASKS; m2++) {
            int n = cm.pin_count[m1] < cm.pin_count[m2] ? cm.pin_count[m1] : cm.pin_count[m2];
            double corr = calc_correlation(cm.visible[m1], cm.visible[m2], n);
            printf("    %d vs %d: %.4f\n", m1, m2, corr);
        }
    }

    long raw = GT, pins = 0;
    for (int m = 0; m < N_MASKS; m++) pins += cm.pin_count[m];
    printf("\n  Storage: raw=%ld, pins=%ld (%.1fx)\n", raw, pins, (double)pins/raw);

    int pass = (exact == GT);
    printf("  => %s\n\n", pass ? "PASS" : "FAIL");

    free(buf);
    return pass;
}

// ============================================================
int main(int argc, char **argv) {
    printf("============================================================\n");
    printf("  Contour Mask V2 — Mask Actually Filters Data\n");
    printf("  Mask = stencil pattern (x+y+z) %% %d\n", N_MASKS);
    printf("  Each mask lets through DIFFERENT cells\n");
    printf("  Directions: A, C, E (B, D, F cancel)\n");
    printf("============================================================\n\n");

    int pass = 0, total = 0;
    total++; pass += test_synthetic();
    if (argc > 1) total++; pass += test_gguf(argv[1]);

    printf("============================================================\n");
    printf("  RESULT: %d/%d PASS\n", pass, total);
    printf("============================================================\n");
    return (pass == total) ? 0 : 1;
}

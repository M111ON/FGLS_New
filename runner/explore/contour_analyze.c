// contour_analyze.c
// Contour Mask — Adjacent Pair Analysis
// Do A+C, C+E, E+A provide genuinely different insights?
// Or are they just redundant copies of the same data?
//
// Compile: gcc -O2 -std=c11 -Wno-error=misleading-indentation -Wno-error=format -o contour_analyze.exe contour_analyze.c -lm
// Run:     contour_analyze.exe model.gguf

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

// ============================================================
// Config
// ============================================================
#define GRID_X  10
#define GRID_Y  10
#define GRID_Z  6
#define TOTAL   (GRID_X * GRID_Y * GRID_Z)

// A, C, E (active — B, D, F cancel)
#define N_DIRS  3
#define DIR_A   0
#define DIR_C   1
#define DIR_E   2
static const char *DN[] = {"A", "C", "E"};
static const char *PN[] = {"A+C", "C+E", "E+A"};

// ============================================================
// Slice analysis — what each direction reveals per slice
// ============================================================
typedef struct {
    double mean;
    double stddev;
    int    n_nonzero;
    int    n_transitions;  // sign changes between adjacent values
    double energy;         // sum of squares
} SliceStats;

static SliceStats analyze_slice(const int8_t *data, int len) {
    SliceStats s;
    memset(&s, 0, sizeof(s));
    if (len == 0) return s;

    double sum = 0, sum2 = 0;
    int prev_sign = (data[0] > 0) - (data[0] < 0);

    for (int i = 0; i < len; i++) {
        double v = (double)data[i];
        sum += v;
        sum2 += v * v;
        if (data[i] != 0) s.n_nonzero++;
        int cur_sign = (data[i] > 0) - (data[i] < 0);
        if (i > 0 && cur_sign != prev_sign && data[i] != 0 && data[i-1] != 0)
            s.n_transitions++;
        prev_sign = cur_sign;
    }

    s.mean = sum / len;
    s.stddev = sqrt(sum2 / len - s.mean * s.mean);
    s.energy = sum2;
    return s;
}

// ============================================================
// Contour Mask
// ============================================================
typedef struct {
    int8_t grid[GRID_X][GRID_Y][GRID_Z];
    int8_t profile[N_DIRS][GRID_X][GRID_Y][GRID_Z];
} CM;

static void cm_encode(CM *cm) {
    // A: X-slices (Y-Z plane at each X)
    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++)
                cm->profile[DIR_A][x][y][z] = cm->grid[x][y][z];

    // C: Y-slices (X-Z plane at each Y)
    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++)
                cm->profile[DIR_C][x][y][z] = cm->grid[x][y][z];

    // E: Z-slices (X-Y plane at each Z)
    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++)
                cm->profile[DIR_E][x][y][z] = cm->grid[x][y][z];
}

// ============================================================
// Analysis: what does each adjacent pair reveal?
// ============================================================
static void analyze_direction_slices(const CM *cm, int dir, const char *name) {
    printf("\n  Direction %s — slice-by-slice analysis:\n", name);
    printf("  %-6s %8s %8s %6s %6s %10s\n",
           "Slice", "Mean", "StdDev", "NonZ", "Trans", "Energy");

    int n_slices = (dir == DIR_A) ? GRID_X : (dir == DIR_C) ? GRID_Y : GRID_Z;
    int slice_len = TOTAL / n_slices;

    double total_energy = 0;
    int total_transitions = 0;

    for (int s = 0; s < n_slices && s < 8; s++) {
        int8_t buf[GRID_X * GRID_Y * GRID_Z];
        int idx = 0;
        if (dir == DIR_A) {
            for (int y = 0; y < GRID_Y; y++)
                for (int z = 0; z < GRID_Z; z++)
                    buf[idx++] = cm->profile[dir][s][y][z];
        } else if (dir == DIR_C) {
            for (int x = 0; x < GRID_X; x++)
                for (int z = 0; z < GRID_Z; z++)
                    buf[idx++] = cm->profile[dir][x][s][z];
        } else {
            for (int x = 0; x < GRID_X; x++)
                for (int y = 0; y < GRID_Y; y++)
                    buf[idx++] = cm->profile[dir][x][y][s];
        }

        SliceStats ss = analyze_slice(buf, idx);
        total_energy += ss.energy;
        total_transitions += ss.n_transitions;

        printf("  [%d]    %8.2f %8.2f %6d %6d %10.0f\n",
               s, ss.mean, ss.stddev, ss.n_nonzero, ss.n_transitions, ss.energy);
    }
    if (n_slices > 8) printf("  ... (%d slices total)\n", n_slices);

    printf("  TOTAL energy=%.0f transitions=%d\n", total_energy, total_transitions);
}

// ============================================================
// Adjacent pair: correlation between two directions
// ============================================================
static void analyze_pair(const CM *cm, int d1, int d2, const char *name) {
    printf("\n  Pair %s — cross-direction analysis:\n", name);

    // Flatten both profiles and compute correlation
    double sum1=0, sum2=0, sum12=0, sum1sq=0, sum2sq = 0;
    int n = 0;

    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++) {
                double v1 = (double)cm->profile[d1][x][y][z];
                double v2 = (double)cm->profile[d2][x][y][z];
                sum1 += v1; sum2 += v2;
                sum12 += v1 * v2;
                sum1sq += v1 * v1;
                sum2sq += v2 * v2;
                n++;
            }

    double mean1 = sum1 / n, mean2 = sum2 / n;
    double cov = sum12 / n - mean1 * mean2;
    double std1 = sqrt(sum1sq/n - mean1*mean1);
    double std2 = sqrt(sum2sq/n - mean2*mean2);
    double corr = (std1 > 0 && std2 > 0) ? cov / (std1 * std2) : 0;

    printf("  Mean %c=%.2f  Mean %c=%.2f\n", DN[d1], mean1, DN[d2], mean2);
    printf("  StdDev %c=%.2f  StdDev %c=%.2f\n", DN[d1], std1, DN[d2], std2);
    printf("  Correlation: %.4f\n", corr);
    printf("  Covariance:  %.2f\n", cov);

    if (corr > 0.99)
        printf("  => HIGHLY CORRELATED (≈ same data, different order)\n");
    else if (corr > 0.5)
        printf("  => MODERATELY CORRELATED (shared structure + differences)\n");
    else
        printf("  => LOW CORRELATION (genuinely different views!)\n");
}

// ============================================================
// Entropy comparison: which direction has most info?
// ============================================================
static double calc_entropy(const CM *cm, int dir) {
    int counts[256] = {0};
    int total = 0;
    for (int x = 0; x < GRID_X; x++)
        for (int y = 0; y < GRID_Y; y++)
            for (int z = 0; z < GRID_Z; z++) {
                counts[(uint8_t)cm->profile[dir][x][y][z]]++;
                total++;
            }
    double entropy = 0;
    for (int i = 0; i < 256; i++) {
        if (counts[i] == 0) continue;
        double p = (double)counts[i] / total;
        entropy -= p * log2(p);
    }
    return entropy;
}

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
// Main
// ============================================================
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: contour_analyze model.gguf\n");
        return 1;
    }

    printf("============================================================\n");
    printf("  Contour Mask — Adjacent Pair Analysis\n");
    printf("  Directions: A, C, E (B, D, F cancel)\n");
    printf("  Question: Do A+C, C+E, E+A provide different insights?\n");
    printf("============================================================\n");

    // Load GGUF
    FILE *f = gguf_fopen(argv[1]);
    if (!f) { printf("Cannot open %s\n", argv[1]); return 1; }

    uint32_t magic, version; uint64_t n_tensors, n_kv;
    fread(&magic, 4, 1, f); fread(&version, 4, 1, f);
    fread(&n_tensors, 8, 1, f); fread(&n_kv, 8, 1, f);
    if (magic != GGUF_MAGIC) { fclose(f); printf("Bad magic\n"); return 1; }

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

    // Get tensor info
    fseek(f, 24, SEEK_SET); skip_kv(f, n_kv);
    uint64_t tensor_off = 0;
    char tensor_name[256] = "";
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t nlen; fread(&nlen, 8, 1, f);
        char name[256]; fread(name, nlen, 1, f); name[nlen]=0;
        uint32_t nd; fread(&nd, 4, 1, f);
        for (uint32_t j=0;j<nd;j++){uint64_t d;fread(&d,8,1,f);}
        uint32_t dt; fread(&dt, 4, 1, f);
        uint64_t off; fread(&off, 8, 1, f);
        if (i == (uint64_t)best) { tensor_off = off; strncpy(tensor_name, name, 255); }
    }
    fseek(f, 0, SEEK_END);
    uint64_t end = ftell(f), sz = end - tensor_off;
    printf("\nModel: %s\nTensor: %s (%llu bytes)\n", argv[1], tensor_name, (unsigned long long)sz);

    // Extract
    int8_t *buf = (int8_t*)calloc(TOTAL + 256, 1);
    fseek(f, tensor_off, SEEK_SET);
    int nr = 0;
    uint64_t nb = sz / 34;
    for (uint64_t b = 0; b < nb && nr < TOTAL; b++) {
        fseek(f, 2, SEEK_CUR);
        for (int j = 0; j < 32 && nr < TOTAL; j++) { fread(&buf[nr], 1, 1, f); nr++; }
    }
    fclose(f);
    printf("Extracted: %d int8 values\n", nr);

    // Fill grid
    CM cm;
    memset(&cm, 0, sizeof(CM));
    int idx = 0;
    for (int x = 0; x < GRID_X && idx < nr; x++)
        for (int y = 0; y < GRID_Y && idx < nr; y++)
            for (int z = 0; z < GRID_Z && idx < nr; z++)
                cm.grid[x][y][z] = buf[idx++];

    cm_encode(&cm);

    // ============================================================
    // Analysis 1: Entropy per direction
    // ============================================================
    printf("\n--- Analysis 1: Entropy per Direction ---\n");
    double ent[N_DIRS];
    for (int d = 0; d < N_DIRS; d++) {
        ent[d] = calc_entropy(&cm, d);
        printf("  %s entropy: %.3f bits\n", DN[d], ent[d]);
    }
    if (ent[0] == ent[1] && ent[1] == ent[2])
        printf("  => SAME entropy (expected: same data, different ordering)\n");
    else
        printf("  => DIFFERENT entropy (unexpected: ordering affects info!)\n");

    // ============================================================
    // Analysis 2: Slice-by-slice for each direction
    // ============================================================
    printf("\n--- Analysis 2: Slice Structure ---\n");
    for (int d = 0; d < N_DIRS; d++)
        analyze_direction_slices(&cm, d, DN[d]);

    // ============================================================
    // Analysis 3: Adjacent pair correlation
    // ============================================================
    printf("\n--- Analysis 3: Adjacent Pair Correlation ---\n");
    int pairs[3][2] = {{DIR_A, DIR_C}, {DIR_C, DIR_E}, {DIR_E, DIR_A}};
    for (int p = 0; p < 3; p++)
        analyze_pair(&cm, pairs[p][0], pairs[p][1], PN[p]);

    // ============================================================
    // Analysis 4: Pair complementarity
    // ============================================================
    printf("\n--- Analysis 4: What Each Pair Captures ---\n");
    for (int p = 0; p < 3; p++) {
        int d1 = pairs[p][0], d2 = pairs[p][1];
        // Compute difference between pair members
        double max_diff = 0, sum_diff = 0;
        int n_diff = 0;
        for (int x = 0; x < GRID_X; x++)
            for (int y = 0; y < GRID_Y; y++)
                for (int z = 0; z < GRID_Z; z++) {
                    double diff = fabs((double)cm.profile[d1][x][y][z] -
                                       (double)cm.profile[d2][x][y][z]);
                    sum_diff += diff;
                    if (diff > max_diff) max_diff = diff;
                    if (diff > 0.5) n_diff++;
                }
        printf("  %s: avg_diff=%.2f max_diff=%.0f cells_differ=%d/%d (%.1f%%)\n",
               PN[p], sum_diff/TOTAL, max_diff, n_diff, TOTAL, 100.0*n_diff/TOTAL);
    }

    // ============================================================
    // Conclusion
    // ============================================================
    printf("\n============================================================\n");
    printf("  CONCLUSION:\n");
    printf("  - If all 3 profiles identical (entropy, correlation ≈ 1.0)\n");
    printf("    => Adjacent pairs are REDUNDANT (same data, different order)\n");
    printf("    => Contour Mask needs non-trivial pin conformation\n");
    printf("  - If profiles differ (entropy varies, correlation < 1.0)\n");
    printf("    => Different directions capture different structure\n");
    printf("    => Adjacent pairs genuinely complement\n");
    printf("============================================================\n");

    free(buf);
    return 0;
}

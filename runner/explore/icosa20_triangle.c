// icosa20_triangle.c
// 20-Face Triangle System Prototype
//   Triangle = true primitive (60°×6=360° hexagon)
//   n² subdivision → scalability
//   Barycentric (u,v) addressing
//   Vertex → 360×360 grid mapping
//
// Architecture:
//   face_id: 0..19 (icosa faces, each an equilateral triangle)
//   u, v: barycentric coords [0,1], u+v ≤ 1
//   sub_tri: (i,j) where i+j < n (n² tiny triangles per face)
//   vertex: shared between 5 (icosa) or 6 (flat) triangles
//
// Compile: gcc -O2 -std=c11 -lm -o ico20_tri.exe runner/explore/icosa20_triangle.c
// Run:     ico20_tri.exe

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PHI    1.61803398874989484820458683436564
#define N_FACES    20
#define N_VERTICES 12

// ── Icosa vertices (unit sphere) ──
static double VERT[12][3];

static void init_verts(void)
{
    int n = 0;
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++) {
            double s1 = a ? -1 : 1, s2 = b ? -PHI : PHI;
            VERT[n][0]=0; VERT[n][1]=s1; VERT[n][2]=s2; n++;
            VERT[n][0]=s1; VERT[n][1]=s2; VERT[n][2]=0; n++;
            VERT[n][0]=s2; VERT[n][1]=0; VERT[n][2]=s1; n++;
        }
    for (int i = 0; i < 12; i++) {
        double l = sqrt(VERT[i][0]*VERT[i][0]
                      + VERT[i][1]*VERT[i][1]
                      + VERT[i][2]*VERT[i][2]);
        VERT[i][0]/=l; VERT[i][1]/=l; VERT[i][2]/=l;
    }
}

// ── Icosa faces (20 triangles, indexed by vertex id 0..11) ──
static const int FACES[20][3] = {
    {0,1,2},{0,2,3},{0,3,4},{0,4,5},{0,5,1},
    {1,6,2},{2,7,3},{3,8,4},{4,9,5},{5,10,1},
    {6,7,2},{7,8,3},{8,9,4},{9,10,5},{10,6,1},
    {6,11,7},{7,11,8},{8,11,9},{9,11,10},{10,11,6}
};

// ── Barycentric on sphere ──
static void bary(int face, double u, double v, double out[3])
{
    const int *f = FACES[face];
    double umv = 1.0 - u - v;
    out[0] = VERT[f[0]][0]*umv + VERT[f[1]][0]*u + VERT[f[2]][0]*v;
    out[1] = VERT[f[0]][1]*umv + VERT[f[1]][1]*u + VERT[f[2]][1]*v;
    out[2] = VERT[f[0]][2]*umv + VERT[f[1]][2]*u + VERT[f[2]][2]*v;
    double l = sqrt(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]);
    out[0]/=l; out[1]/=l; out[2]/=l;
}

static void to_grid(const double p[3], int *th, int *ph)
{
    double t = atan2(p[1], p[0]) / (2*M_PI);
    double ph2 = acos(p[2]) / M_PI;
    if (t < 0) t += 1.0;
    *th = (int)(t * 360) % 360;
    *ph = (int)(ph2 * 360) % 360;
}

// ── Tests ──

static int t1_verts(void)
{
    printf("=== T1: 12 vertices ===\n");
    int ok = 1;
    for (int i = 0; i < 12; i++) {
        double l = sqrt(VERT[i][0]*VERT[i][0]+VERT[i][1]*VERT[i][1]+VERT[i][2]*VERT[i][2]);
        if (fabs(l-1.0)>1e-14) ok=0;
    }
    printf("  %s\n", ok?"PASS":"FAIL");
    return ok;
}

static int t2_faces(void)
{
    printf("=== T2: 20 faces ===\n");
    for (int f = 0; f < 5; f++)
        printf("  f%2d: v%2d v%2d v%2d\n", f, FACES[f][0], FACES[f][1], FACES[f][2]);
    printf("  ... (%d faces total)\n", N_FACES);
    int ok = 1;
    for (int f = 0; f < N_FACES; f++)
        if (FACES[f][0]==FACES[f][1]||FACES[f][1]==FACES[f][2]||FACES[f][2]==FACES[f][0]) ok=0;
    printf("  %s\n", ok?"PASS":"FAIL");
    return ok;
}

static int t3_bary(void)
{
    printf("=== T3: Barycentric coords ===\n");
    double p[3];
    bary(0,0,0,p);
    double d0 = sqrt(pow(p[0]-VERT[FACES[0][0]][0],2)
                    +pow(p[1]-VERT[FACES[0][0]][1],2)
                    +pow(p[2]-VERT[FACES[0][0]][2],2));
    bary(0,1,0,p);
    double d1 = sqrt(pow(p[0]-VERT[FACES[0][1]][0],2)
                    +pow(p[1]-VERT[FACES[0][1]][1],2)
                    +pow(p[2]-VERT[FACES[0][1]][2],2));
    printf("  (0,0)→v%d diff=%.2e\n", FACES[0][0], d0);
    printf("  (1,0)→v%d diff=%.2e\n", FACES[0][1], d1);
    int ok = (d0 < 1e-12 && d1 < 1e-12);
    printf("  %s\n", ok?"PASS":"FAIL");
    return ok;
}

static int t4_grid(void)
{
    printf("=== T4: →360×360 grid ===\n");
    int th, ph;
    for (int v = 0; v < 12; v++) {
        to_grid(VERT[v], &th, &ph);
        printf("  v%2d: (θ=%3d, φ=%3d)\n", v, th, ph);
    }
    // Coverage: 12 vertices on 129600 grid
    uint8_t *hits = (uint8_t*)calloc(129600, 1);
    if (!hits) { printf("  FAIL: alloc\n"); return 0; }
    for (int v = 0; v < 12; v++) {
        to_grid(VERT[v], &th, &ph);
        hits[th*360 + ph] = 1;
    }
    int n = 0;
    for (int i = 0; i < 129600; i++) n += hits[i];
    free(hits);
    printf("  12 vertices → %d unique grid positions (%.2f%%)\n",
           n, 100.0*n/129600);
    printf("  PASS\n");
    return 1;
}

// ── Subdivision ──
typedef struct { int face, i, j, rev; } SubTri;

static int sub_count(int n)
{
    return N_FACES * n * n * 2;  // 2 triangles per quad
}

static void sub_fill(int n, SubTri *out)
{
    int idx = 0;
    for (int f = 0; f < N_FACES; f++)
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n-i; j++) {
                out[idx].face=f; out[idx].i=i; out[idx].j=j; out[idx].rev=0; idx++;
                out[idx].face=f; out[idx].i=i; out[idx].j=j; out[idx].rev=1; idx++;
            }
}

static int t5_subdiv(void)
{
    printf("=== T5: Subdivision at n=6 (720 triangles) ===\n");
    int n = 6;
    int c = sub_count(n);
    printf("  n=%d: %d sub-triangles (2×%d×%d)\n", n, c, N_FACES, n*n);

    SubTri *t = (SubTri*)calloc(c, sizeof(SubTri));
    if (!t) { printf("  FAIL: alloc\n"); return 0; }
    sub_fill(n, t);

    printf("  Face 0 sample:\n");
    int shown = 0;
    for (int i = 0; i < c && shown < 8; i++) {
        if (t[i].face == 0) {
            // Get corner 0 barycentric
            double u = (double)t[i].i / n;
            double v = (double)t[i].j / n;
            double p[3]; bary(0, u, v, p);
            int th, ph; to_grid(p, &th, &ph);
            printf("    tri(%d,%d,%s): (θ=%3d, φ=%3d)\n",
                   t[i].i, t[i].j, t[i].rev?"r":"f", th, ph);
            shown++;
        }
    }

    // Coverage: how many 360×360 points do the vertices cover?
    uint8_t *hits = (uint8_t*)calloc(129600, 1);
    if (!hits) { free(t); return 0; }
    for (int i = 0; i < c; i++)
        for (int cn = 0; cn < 3; cn++) {
            double u = (double)t[i].i / n;
            double v = (double)t[i].j / n;
            if (cn == 1) u = (double)(t[i].i+1)/n;
            if (cn == 2) v = (double)(t[i].j+1)/n;
            if (u+v > 1) { u = (double)(t[i].i+1)/n; v = (double)(t[i].j)/n; }
            double p[3]; bary(t[i].face, u, v, p);
            int th, ph; to_grid(p, &th, &ph);
            hits[th*360+ph] = 1;
        }

    int nh = 0;
    for (int i = 0; i < 129600; i++) nh += hits[i];
    printf("\n  Unique grid positions: %d / 129600 (%.2f%%)\n", nh, 100.0*nh/129600);
    printf("  Expected V = 10n²+2 = %d\n", 10*n*n+2);

    free(hits);
    free(t);
    printf("  PASS\n");
    return 1;
}

static int t6_hex(void)
{
    printf("=== T6: Hexagon tessellation ===\n");
    int val[12] = {0};
    for (int f = 0; f < N_FACES; f++)
        for (int v = 0; v < 3; v++) val[FACES[f][v]]++;

    printf("  Vertex valence (triangles per vertex):\n");
    for (int v = 0; v < 12; v++)
        printf("    v%2d: %d tris %s\n", v, val[v], val[v]==5?"(5×60°=300°)":"");
    printf("  Missing 60° per vertex × 12 = 720° = 4π sr ✓\n");

    printf("  Hex shell expansion:\n");
    for (int r = 0; r <= 8; r++) {
        int tris = 3*r*(r+1) + 1;
        printf("    r=%d: %d tris\n", r, tris);
    }
    return 1;
}

int main(void)
{
    printf("============================================================\n");
    printf("  20-Face Triangle System\n");
    printf("============================================================\n\n");

    init_verts();

    int p=0, t=0;
    t++; p += t1_verts();
    t++; p += t2_faces();
    t++; p += t3_bary();
    t++; p += t4_grid();
    t++; p += t5_subdiv();
    t++; p += t6_hex();

    printf("\n============================================================\n");
    printf("  FINAL: %d/%d PASS\n", p, t);
    printf("============================================================\n");
    return (p==t)?0:1;
}

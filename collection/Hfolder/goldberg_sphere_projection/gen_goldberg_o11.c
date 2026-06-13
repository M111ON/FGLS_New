/*
 * gen_goldberg_o11.c — GP(2,0) Sphere → Flat Image Generator
 * ═══════════════════════════════════════════════════════════
 * O11 objective: project Goldberg GP(2,0) geometry onto 256×256 BMP
 * Two projections side-by-side in one 512×256 BMP:
 *   LEFT  [0..255]   = Equirectangular  (lon/lat → pixel)
 *   RIGHT [256..511] = Gnomonic         (sphere center projection)
 *
 * Color scheme:
 *   Pentagon zone  = bright blue  (R:60  G:120 B:220)
 *   Hexagon zone   = dark gray    (R:40  G:40  B:50 )
 *   Pentagon edge  = white        (R:255 G:255 B:255)  ← seam boundary
 *   Hex edge       = mid gray     (R:90  G:90  B:90 )
 *
 * GP(2,0): T=4, 12 pentagons + 30 hexagons = 42 faces
 * Icosahedron base: 12 vertices → pentagons, each tri subdivided 2×
 *
 * Build: gcc -O3 -o gen_goldberg gen_goldberg_o11.c -lm
 * Run:   ./gen_goldberg
 * Output: gp20_equirect.bmp, gp20_gnomonic.bmp, gp20_both.bmp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ── constants ─────────────────────────────────────────── */
#define W       256
#define H       256
#define W2      512   /* side-by-side output */
#define PI      3.14159265358979323846
#define TAU     (2.0*PI)
#define PHI     1.61803398874989484820  /* golden ratio */

/* ── BMP write ─────────────────────────────────────────── */
static void bmp_save(const char *path, const uint8_t *px, int w, int h) {
    int rs = (w * 3 + 3) & ~3;
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    *(uint32_t*)(hdr+2)  = 54 + rs * h;
    *(uint32_t*)(hdr+10) = 54;
    *(uint32_t*)(hdr+14) = 40;
    *(int32_t*) (hdr+18) = w;
    *(int32_t*) (hdr+22) = h;
    *(uint16_t*)(hdr+26) = 1;
    *(uint16_t*)(hdr+28) = 24;
    *(uint32_t*)(hdr+34) = rs * h;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = calloc(rs, 1);
    for (int y = h-1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            int d = (y * w + x) * 3;
            row[x*3+0] = px[d+2];
            row[x*3+1] = px[d+1];
            row[x*3+2] = px[d+0];
        }
        fwrite(row, 1, rs, f);
    }
    free(row); fclose(f);
}

/* ── 3D vector ops ─────────────────────────────────────── */
typedef struct { double x, y, z; } V3;

static inline V3   v3_norm(V3 v) {
    double r = sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    if (r < 1e-12) return v;
    return (V3){v.x/r, v.y/r, v.z/r};
}
static inline double v3_dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline V3   v3_add(V3 a, V3 b)   { return (V3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline V3   v3_scale(V3 v, double s) { return (V3){v.x*s, v.y*s, v.z*s}; }
static inline V3   v3_mid(V3 a, V3 b)   { return v3_norm(v3_scale(v3_add(a,b), 0.5)); }

/* ── Icosahedron 12 vertices ───────────────────────────── */
#define N_ICO_V  12
#define N_ICO_F  20

static V3 ico_v[N_ICO_V];
static int ico_f[N_ICO_F][3];

static void build_icosahedron(void) {
    /* standard icosahedron on unit sphere */
    double t = 1.0 / PHI;
    /* 3 orthogonal rectangles */
    V3 raw[12] = {
        {-1, t, 0},{1, t, 0},{-1,-t, 0},{ 1,-t, 0},
        { 0,-1, t},{0, 1, t},{ 0,-1,-t},{ 0, 1,-t},
        { t, 0,-1},{t, 0, 1},{-t, 0,-1},{-t, 0, 1}
    };
    for (int i = 0; i < 12; i++) ico_v[i] = v3_norm(raw[i]);

    /* 20 triangular faces */
    int f[20][3] = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
    };
    memcpy(ico_f, f, sizeof(f));
}

/* ── GP(2,0) subdivision ───────────────────────────────── */
/* Each icosahedron triangle → 4 sub-triangles (T=4, one step)
 * Pentagons: at original 12 icosahedron vertices
 * Hexagons: at edge midpoints (30 new vertices)
 * Total faces = 12 pent + 30 hex = 42
 *
 * For face classification: given a point P on sphere,
 * find which Goldberg face it belongs to by nearest-center.
 * Pentagon centers = original ico vertices (12)
 * Hexagon centers = edge midpoints normalized (30)
 */

#define N_PENT  12
#define N_HEX   30
#define N_FACES 42

static V3   face_centers[N_FACES];
static int  face_is_pent[N_FACES];   /* 1=pentagon, 0=hexagon */

/* edge midpoint dedup: 30 unique edges */
static int  edge_done[N_ICO_V][N_ICO_V];

static void build_gp20_faces(void) {
    int fc = 0;

    /* first 12: pentagon centers = icosahedron vertices */
    for (int i = 0; i < N_ICO_V; i++) {
        face_centers[fc] = ico_v[i];
        face_is_pent[fc] = 1;
        fc++;
    }

    /* next 30: hexagon centers = edge midpoints */
    memset(edge_done, -1, sizeof(edge_done));
    for (int f = 0; f < N_ICO_F; f++) {
        for (int e = 0; e < 3; e++) {
            int a = ico_f[f][e];
            int b = ico_f[f][(e+1)%3];
            if (a > b) { int t=a; a=b; b=t; }
            if (edge_done[a][b] < 0) {
                edge_done[a][b] = fc;
                face_centers[fc] = v3_mid(ico_v[a], ico_v[b]);
                face_is_pent[fc] = 0;
                fc++;
            }
        }
    }
    /* fc should be 42 */
}

/* ── classify point on sphere → face index ────────────── */
static int classify_sphere(V3 p) {
    double best = -2.0;
    int    bi   = 0;
    for (int i = 0; i < N_FACES; i++) {
        double d = v3_dot(p, face_centers[i]);
        if (d > best) { best = d; bi = i; }
    }
    return bi;
}

/* ── edge proximity: is point near any Goldberg face boundary? ── */
/* Returns distance to nearest face boundary (angular, radians)    */
static double edge_proximity(V3 p) {
    /* top-2 nearest face centers, angular gap = boundary measure */
    double d1 = -2.0, d2 = -2.0;
    for (int i = 0; i < N_FACES; i++) {
        double d = v3_dot(p, face_centers[i]);
        if (d > d1) { d2 = d1; d1 = d; }
        else if (d > d2) { d2 = d; }
    }
    /* smaller gap = closer to boundary */
    return (d1 - d2);
}

/* ── sphere → equirectangular ──────────────────────────── */
/* lon ∈ [-π,π], lat ∈ [-π/2,π/2] → (x,y) ∈ [0,W)×[0,H) */
static V3 equirect_to_sphere(int px, int py) {
    double lon = ((double)px / W) * TAU - PI;
    double lat = (1.0 - (double)py / H) * PI - PI/2.0;
    double cos_lat = cos(lat);
    return (V3){ cos_lat * cos(lon), cos_lat * sin(lon), sin(lat) };
}

/* ── sphere → gnomonic (tangent at 6 faces) ────────────── */
/* Project from 6 cube faces, stitch into square */
/* Simple version: project from north pole tangent plane, clip circle */
/* We use 6 gnomonic faces mapped into 3×2 grid within 256×256 */
typedef struct { int face; double u, v; } GnoCoord;

/* Gnomonic: project onto cube face planes, then into image */
/* 6 cube faces: +X,-X,+Y,-Y,+Z,-Z */
static V3 gnomonic_to_sphere(int px, int py) {
    /* Map 256×256 → 6 faces in a cross layout
     * We use equiangular cube face projection:
     * Divide 256×256 into 4×4 blocks of 64px each
     * Use face layout (row-major, 4 cols × 4 rows):
     *      col: 0   1   2   3
     * row 0: [  ] [+Y] [  ] [  ]
     * row 1: [-X] [+Z] [+X] [-Z]
     * row 2: [  ] [-Y] [  ] [  ]
     * row 3: [  ] [  ] [  ] [  ]
     */
    int BS = 64;  /* block size */
    int col = px / BS;
    int row = py / BS;
    double fu = ((px % BS) + 0.5) / BS * 2.0 - 1.0;  /* [-1,1] */
    double fv = ((py % BS) + 0.5) / BS * 2.0 - 1.0;

    V3 dir = {0,0,1};
    /* face layout */
    if      (row==0 && col==1) dir = v3_norm((V3){ fu,  1.0, -fv});  /* +Y */
    else if (row==1 && col==0) dir = v3_norm((V3){-1.0, -fu, -fv}); /* -X */
    else if (row==1 && col==1) dir = v3_norm((V3){ fu,  fv,   1.0}); /* +Z */
    else if (row==1 && col==2) dir = v3_norm((V3){ 1.0,  fu, -fv}); /* +X */
    else if (row==1 && col==3) dir = v3_norm((V3){-fu,  fv,  -1.0});/* -Z */
    else if (row==2 && col==1) dir = v3_norm((V3){ fu, -1.0,  fv}); /* -Y */
    else                       dir = (V3){0,0,0};  /* unused region */

    return dir;
}

/* ── color map ─────────────────────────────────────────── */
/* Pentagon zone: steel blue R:60 G:100 B:200 */
/* Hexagon zone:  dark navy  R:20 G:30  B:45  */
/* Boundary:      white flash near seam */
static void face_to_rgb(int face_id, double edge_gap, uint8_t *r, uint8_t *g, uint8_t *b) {
    int is_pent = face_is_pent[face_id];

    /* edge_gap: small = near boundary */
    /* threshold: ~0.05 radians angular separation */
    double edge_t = 1.0 - (edge_gap / 0.12);
    if (edge_t < 0) edge_t = 0;
    if (edge_t > 1) edge_t = 1;
    double edge_bright = edge_t * edge_t;  /* sharpen */

    if (is_pent) {
        /* Pentagon: blue spectrum */
        *r = (uint8_t)(40  + edge_bright * 215);
        *g = (uint8_t)(80  + edge_bright * 175);
        *b = (uint8_t)(200 + edge_bright * 55);
    } else {
        /* Hexagon: dark, subtle variation by face index */
        double var = (face_id % 7) / 7.0 * 15.0;
        *r = (uint8_t)(20  + var + edge_bright * 235);
        *g = (uint8_t)(28  + var + edge_bright * 227);
        *b = (uint8_t)(42  + var + edge_bright * 213);
    }
}

/* ── render ─────────────────────────────────────────────── */
static void render_equirect(uint8_t *buf, int ox) {
    for (int py = 0; py < H; py++) {
        for (int px = 0; px < W; px++) {
            V3 p = equirect_to_sphere(px, py);
            int fi = classify_sphere(p);
            double eg = edge_proximity(p);
            uint8_t r, g, b;
            face_to_rgb(fi, eg, &r, &g, &b);
            int d = (py * W2 + ox + px) * 3;
            buf[d+0] = r; buf[d+1] = g; buf[d+2] = b;
        }
    }
}

static void render_gnomonic(uint8_t *buf, int ox) {
    for (int py = 0; py < H; py++) {
        for (int px = 0; px < W; px++) {
            V3 p = gnomonic_to_sphere(px, py);
            uint8_t r, g, b;
            if (p.x == 0 && p.y == 0 && p.z == 0) {
                /* unused region — black */
                r = g = b = 8;
            } else {
                int fi = classify_sphere(p);
                double eg = edge_proximity(p);
                face_to_rgb(fi, eg, &r, &g, &b);
            }
            int d = (py * W2 + ox + px) * 3;
            buf[d+0] = r; buf[d+1] = g; buf[d+2] = b;
        }
    }
}

/* ── separator line ─────────────────────────────────────── */
static void draw_separator(uint8_t *buf) {
    int x = W; /* column 256 */
    for (int y = 0; y < H; y++) {
        int d = (y * W2 + x) * 3;
        buf[d+0] = 60; buf[d+1] = 100; buf[d+2] = 140;
    }
}

/* ── label burn (minimal, 5×7 font) ─────────────────────── */
/* Not implemented — labels in filename instead */

/* ── also write single 256×256 equirect for encoder ──────── */
static void render_equirect_solo(uint8_t *buf256) {
    for (int py = 0; py < H; py++) {
        for (int px = 0; px < W; px++) {
            V3 p = equirect_to_sphere(px, py);
            int fi = classify_sphere(p);
            double eg = edge_proximity(p);
            uint8_t r, g, b;
            face_to_rgb(fi, eg, &r, &g, &b);
            int d = (py * W + px) * 3;
            buf256[d+0] = r; buf256[d+1] = g; buf256[d+2] = b;
        }
    }
}

static void render_gnomonic_solo(uint8_t *buf256) {
    for (int py = 0; py < H; py++) {
        for (int px = 0; px < W; px++) {
            V3 p = gnomonic_to_sphere(px, py);
            uint8_t r, g, b;
            if (p.x == 0 && p.y == 0 && p.z == 0) {
                r = g = b = 8;
            } else {
                int fi = classify_sphere(p);
                double eg = edge_proximity(p);
                face_to_rgb(fi, eg, &r, &g, &b);
            }
            int d = (py * W + px) * 3;
            buf256[d+0] = r; buf256[d+1] = g; buf256[d+2] = b;
        }
    }
}

/* ── main ─────────────────────────────────────────────────── */
int main(void) {
    printf("gen_goldberg_o11 — GP(2,0) image generator\n");

    build_icosahedron();
    build_gp20_faces();

    printf("  faces: %d pent + %d hex = %d total\n",
           N_PENT, N_HEX, N_FACES);

    /* validate face count */
    int np=0, nh=0;
    for(int i=0;i<N_FACES;i++) { if(face_is_pent[i]) np++; else nh++; }
    printf("  classified: %d pent, %d hex\n", np, nh);

    /* ── render both projections → side-by-side ── */
    size_t buf_sz = (size_t)W2 * H * 3;
    uint8_t *both = calloc(buf_sz, 1);
    if (!both) { fprintf(stderr, "alloc failed\n"); return 1; }

    printf("  rendering equirectangular [0..255]...\n");
    render_equirect(both, 0);

    printf("  rendering gnomonic [256..511]...\n");
    render_gnomonic(both, W);

    draw_separator(both);

    bmp_save("gp20_both.bmp", both, W2, H);
    printf("  saved: gp20_both.bmp (%d×%d)\n", W2, H);
    free(both);

    /* ── individual 256×256 for encoder ── */
    uint8_t *solo = malloc((size_t)W * H * 3);

    render_equirect_solo(solo);
    bmp_save("gp20_equirect.bmp", solo, W, H);
    printf("  saved: gp20_equirect.bmp (256×256, ready for encoder)\n");

    render_gnomonic_solo(solo);
    bmp_save("gp20_gnomonic.bmp", solo, W, H);
    printf("  saved: gp20_gnomonic.bmp (256×256, ready for encoder)\n");

    free(solo);

    printf("\nNext step (O11):\n");
    printf("  cp gp20_equirect.bmp vault/\n");
    printf("  cp gp20_gnomonic.bmp vault/\n");
    printf("  ./geopixel_v18 vault/gp20_equirect.bmp\n");
    printf("  ./geopixel_v18 vault/gp20_gnomonic.bmp\n");
    printf("  → observe blob_sz pattern — seam should align pentagon boundary\n");

    return 0;
}

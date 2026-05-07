/*
 * gen_pent_o15.c — Pentagon Classifier O15: Pole FN Fix
 * ═══════════════════════════════════════════════════════
 * Problem: 8 FN at pole tiles ty=1,14 (equirect pixel compression)
 * Fix A: wrap-around neighbor voting + lat_weight correction
 *
 * Grid: 512×512 image → 16×32 tiles (32px each)
 * Algorithm:
 *   1. blob_sz > 52 AND max_delta > 5  (O14 base)
 *   2. POLE tiles (ty=1 or ty=14): relax threshold + neighbor wrap
 *      - tx wraps: tx=0 neighbors tx=31 (lon wrap)
 *      - lat_weight: pole tiles compressed → normalize by lat_scale
 *      - OR-vote: if any 2 neighbors are boundary → promote
 *
 * Build: gcc -O3 -o gen_pent_o15 gen_pent_o15.c -lm
 * Run:   ./gen_pent_o15
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ── geometry (same as O11) ────────────────────────────── */
#define W       512
#define H       512
#define TILE    32
#define TX      (W/TILE)   /* 16 */
#define TY      (H/TILE)   /* 16 */
#define N_TILES (TX*TY)    /* 256 */
#define PI      3.14159265358979323846
#define TAU     (2.0*PI)
#define PHI     1.61803398874989484820

typedef struct { double x, y, z; } V3;

static inline V3   v3_norm(V3 v) {
    double r = sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    if (r < 1e-12) return v;
    return (V3){v.x/r, v.y/r, v.z/r};
}
static inline double v3_dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline V3 v3_add(V3 a, V3 b) { return (V3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline V3 v3_scale(V3 v, double s) { return (V3){v.x*s, v.y*s, v.z*s}; }
static inline V3 v3_mid(V3 a, V3 b) { return v3_norm(v3_scale(v3_add(a,b), 0.5)); }

#define N_ICO_V  12
#define N_ICO_F  20
static V3  ico_v[N_ICO_V];
static int ico_f[N_ICO_F][3];

static void build_icosahedron(void) {
    double t = 1.0 / PHI;
    V3 raw[12] = {
        {-1, t, 0},{1, t, 0},{-1,-t, 0},{ 1,-t, 0},
        { 0,-1, t},{0, 1, t},{ 0,-1,-t},{ 0, 1,-t},
        { t, 0,-1},{t, 0, 1},{-t, 0,-1},{-t, 0, 1}
    };
    for (int i = 0; i < 12; i++) ico_v[i] = v3_norm(raw[i]);
    int f[20][3] = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
    };
    memcpy(ico_f, f, sizeof(f));
}

#define N_PENT  12
#define N_HEX   30
#define N_FACES 42
static V3  face_centers[N_FACES];
static int face_is_pent[N_FACES];
static int edge_done[N_ICO_V][N_ICO_V];

static void build_gp20_faces(void) {
    int fc = 0;
    for (int i = 0; i < N_ICO_V; i++) {
        face_centers[fc] = ico_v[i]; face_is_pent[fc] = 1; fc++;
    }
    memset(edge_done, -1, sizeof(edge_done));
    for (int f = 0; f < N_ICO_F; f++) {
        for (int e = 0; e < 3; e++) {
            int a = ico_f[f][e], b = ico_f[f][(e+1)%3];
            if (a > b) { int t=a; a=b; b=t; }
            if (edge_done[a][b] < 0) {
                edge_done[a][b] = fc;
                face_centers[fc] = v3_mid(ico_v[a], ico_v[b]);
                face_is_pent[fc] = 0;
                fc++;
            }
        }
    }
}

static int classify_sphere(V3 p) {
    double best = -2.0; int bi = 0;
    for (int i = 0; i < N_FACES; i++) {
        double d = v3_dot(p, face_centers[i]);
        if (d > best) { best = d; bi = i; }
    }
    return bi;
}

static double edge_proximity(V3 p) {
    double d1 = -2.0, d2 = -2.0;
    for (int i = 0; i < N_FACES; i++) {
        double d = v3_dot(p, face_centers[i]);
        if (d > d1) { d2 = d1; d1 = d; }
        else if (d > d2) { d2 = d; }
    }
    return (d1 - d2);
}

/* ── is point near pentagon seam on sphere? ─────────────── */
/* Returns 1 if within threshold of pentagon boundary */
static int is_near_pent_boundary(V3 p, double thr) {
    /* Find nearest face, check if it is pentagon or adjacent */
    double d_sorted[N_FACES];
    int    id_sorted[N_FACES];
    /* simple insertion: O(42²) fine */
    for (int i = 0; i < N_FACES; i++) {
        d_sorted[i] = v3_dot(p, face_centers[i]);
        id_sorted[i] = i;
    }
    /* get top-3 */
    for (int i = 0; i < 3; i++) {
        int mx = i;
        for (int j = i+1; j < N_FACES; j++)
            if (d_sorted[j] > d_sorted[mx]) mx = j;
        double td = d_sorted[i]; d_sorted[i] = d_sorted[mx]; d_sorted[mx] = td;
        int   ti = id_sorted[i]; id_sorted[i] = id_sorted[mx]; id_sorted[mx] = ti;
    }
    /* if top face is pentagon OR gap to 2nd is < thr → boundary */
    if (face_is_pent[id_sorted[0]]) return 1;
    if ((d_sorted[0] - d_sorted[1]) < thr) return 1;
    /* check if any top-3 is pentagon with small gap */
    for (int i = 1; i < 3; i++)
        if (face_is_pent[id_sorted[i]] && (d_sorted[0] - d_sorted[i]) < thr*1.5)
            return 1;
    return 0;
}

/* ── BMP write ──────────────────────────────────────────── */
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
            row[x*3+0] = px[d+2]; row[x*3+1] = px[d+1]; row[x*3+2] = px[d+0];
        }
        fwrite(row, 1, rs, f);
    }
    free(row); fclose(f);
}

/* ── tile feature extraction ─────────────────────────────
 * For each tile: sample TILE×TILE pixels → compute:
 *   pent_hit   = # pixels that land in/near pentagon boundary on sphere
 *   lat_scale  = equirect latitude compression factor (cos(lat) correction)
 * Then blob_sz proxy = pent_hit * lat_scale_correction
 */
typedef struct {
    int   tx, ty;
    int   pent_hit;       /* raw # pentagon-boundary pixels */
    int   total_px;       /* = TILE*TILE */
    double lat_center;    /* latitude of tile center */
    double lat_scale;     /* 1/cos(lat) correction factor */
    double corrected_hit; /* pent_hit * lat_scale */
    int   gt_boundary;    /* ground truth: is this a pentagon boundary tile? */
    int   predicted;      /* O15 prediction */
} TileInfo;

static TileInfo tiles[TY][TX];

/* ── equirect pixel → sphere ────────────────────────────── */
static V3 pixel_to_sphere(int px, int py) {
    double lon = ((double)px / W) * TAU - PI;
    double lat = (1.0 - (double)py / H) * PI - PI/2.0;
    double cl  = cos(lat);
    return (V3){ cl*cos(lon), cl*sin(lon), sin(lat) };
}

/* ── ground truth: known pentagon tile positions (O14 confirmed) ─
 * 12 pentagon centers on GP(2,0), mapped to 16×16 tile grid
 * FN from O14 = pole tiles ty=1,14 (equirect compression)
 */

/* ── lat scale: how compressed is equirect at this latitude ─ */
static double lat_scale_factor(double lat) {
    double cl = cos(lat);
    if (cl < 0.01) cl = 0.01;
    return 1.0 / cl;  /* pixels at pole span more sphere area */
}

/* ── analyze all tiles ──────────────────────────────────── */
static void analyze_tiles(double geo_thr) {
    for (int ty = 0; ty < TY; ty++) {
        for (int tx = 0; tx < TX; tx++) {
            TileInfo *t = &tiles[ty][tx];
            t->tx = tx; t->ty = ty;

            /* tile center pixel */
            int cx = tx * TILE + TILE/2;
            int cy = ty * TILE + TILE/2;
            double lon_c = ((double)cx / W) * TAU - PI;
            double lat_c = (1.0 - (double)cy / H) * PI - PI/2.0;
            t->lat_center = lat_c;
            t->lat_scale  = lat_scale_factor(lat_c);

            /* sample all pixels in tile */
            int pent_hit = 0;
            int total    = 0;
            for (int py = ty*TILE; py < (ty+1)*TILE; py++) {
                for (int px = tx*TILE; px < (tx+1)*TILE; px++) {
                    V3 p = pixel_to_sphere(px, py);
                    if (is_near_pent_boundary(p, geo_thr)) pent_hit++;
                    total++;
                }
            }
            t->pent_hit    = pent_hit;
            t->total_px    = total;
            t->corrected_hit = pent_hit * t->lat_scale;
        }
    }
}

/* ── classify a tile using O14 base + O15 pole fix ──────── */
/*
 * O14 base: blob_sz > 52 AND max_delta > 5
 * We model blob_sz ∝ pent_hit (# boundary pixels in tile)
 * lat_correction: pole tiles under-count because pixels stack
 *
 * O15 additions:
 *   1. Pole rows (ty=0,1,14,15): apply lat_scale to corrected_hit
 *   2. Wrap neighbor: tx wraps 0↔(TX-1) for lon continuity
 *   3. Neighbor vote: if ≥2 neighbors are positive → promote suspect
 */
static int classify_tile_o15(int tx, int ty,
                              double sz_thr,   /* base threshold on raw */
                              double sz_thr_pole) /* relaxed for poles */
{
    TileInfo *t = &tiles[ty][tx];
    int is_pole = (ty <= 1 || ty >= TY-2);

    /* base: raw pent_hit ≈ blob_sz surrogate */
    double effective_hit = t->pent_hit;
    if (is_pole) {
        /* lat correction: normalize by cos(lat) to get true sphere coverage */
        effective_hit = t->corrected_hit;
    }

    double thr = is_pole ? sz_thr_pole : sz_thr;
    return (effective_hit >= thr);
}

/* neighbor vote pass: promote tiles that have ≥2 positive neighbors */
static void neighbor_vote(int result[TY][TX], int vote_thr) {
    int promoted[TY][TX];
    memcpy(promoted, result, sizeof(promoted));

    for (int ty = 0; ty < TY; ty++) {
        for (int tx = 0; tx < TX; tx++) {
            if (result[ty][tx]) continue;  /* already positive */

            int count = 0;
            /* 4-neighbors with lon wrap */
            int neighbors[4][2] = {
                {(tx-1+TX)%TX, ty},   /* left  — wraps at tx=0 */
                {(tx+1)%TX,    ty},   /* right — wraps at tx=TX-1 */
                {tx, (ty-1+TY)%TY},   /* up */
                {tx, (ty+1)%TY}       /* down */
            };
            for (int n = 0; n < 4; n++) {
                int nx = neighbors[n][0], ny = neighbors[n][1];
                count += result[ny][nx];
            }
            if (count >= vote_thr) promoted[ty][tx] = 1;
        }
    }
    memcpy(result, promoted, sizeof(promoted));
}

/* ── evaluate predictions ───────────────────────────────── */
/*
 * Ground truth: pentagon boundary tiles
 * These are tiles that contain a significant portion of pentagon seam.
 * We determine GT by direct sphere sampling: if tile has > MIN_HIT
 * pixels near pentagon boundary (at fine threshold), it's GT=1.
 */
static void evaluate(int result[TY][TX], double gt_thr,
                     int *tp, int *fp, int *fn, int *tn) {
    *tp = *fp = *fn = *tn = 0;

    /* build GT from fine sphere sampling */
    int gt[TY][TX];
    for (int ty = 0; ty < TY; ty++) {
        for (int tx = 0; tx < TX; tx++) {
            /* GT: at least 20% of tile pixels are pentagon boundary */
            gt[ty][tx] = (tiles[ty][tx].pent_hit >= (int)(TILE*TILE * gt_thr));
        }
    }

    for (int ty = 0; ty < TY; ty++) {
        for (int tx = 0; tx < TX; tx++) {
            int p = result[ty][tx];
            int g = gt[ty][tx];
            if (p && g)  (*tp)++;
            else if (p && !g) (*fp)++;
            else if (!p && g) (*fn)++;
            else (*tn)++;
        }
    }
}

/* ── render diagnostic image ────────────────────────────── */
/* Color: TP=green, FP=red, FN=yellow, TN=dark */
static void render_result(uint8_t *buf, int result[TY][TX], double gt_thr) {
    int gt[TY][TX];
    for (int ty = 0; ty < TY; ty++)
        for (int tx = 0; tx < TX; tx++)
            gt[ty][tx] = (tiles[ty][tx].pent_hit >= (int)(TILE*TILE * gt_thr));

    for (int ty = 0; ty < TY; ty++) {
        for (int tx = 0; tx < TX; tx++) {
            int p = result[ty][tx];
            int g = gt[ty][tx];
            uint8_t r, gv, b;
            if      (p && g)  { r=50;  gv=200; b=50;  }  /* TP: green */
            else if (p && !g) { r=220; gv=40;  b=40;  }  /* FP: red   */
            else if (!p && g) { r=220; gv=200; b=40;  }  /* FN: yellow*/
            else              { r=20;  gv=25;  b=35;  }  /* TN: dark  */

            /* draw tile with 1px border */
            for (int py = ty*TILE+1; py < (ty+1)*TILE-1; py++) {
                for (int px = tx*TILE+1; px < (tx+1)*TILE-1; px++) {
                    int d = (py*W + px)*3;
                    buf[d+0]=r; buf[d+1]=gv; buf[d+2]=b;
                }
            }
            /* border: tile grid */
            for (int py = ty*TILE; py < (ty+1)*TILE; py++) {
                int d1 = (py*W + tx*TILE)*3;
                int d2 = (py*W + (tx+1)*TILE-1)*3;
                buf[d1]=buf[d2]=45; buf[d1+1]=buf[d2+1]=50; buf[d1+2]=buf[d2+2]=60;
            }
            for (int px = tx*TILE; px < (tx+1)*TILE; px++) {
                int d1 = (ty*TILE*W + px)*3;
                int d2 = ((ty+1)*TILE-1)*W*3 + px*3;
                buf[d1]=buf[d2]=45; buf[d1+1]=buf[d2+1]=50; buf[d1+2]=buf[d2+2]=60;
            }
        }
    }
}

/* ── main ────────────────────────────────────────────────── */
int main(void) {
    printf("gen_pent_o15 — Pentagon Classifier O15: Pole FN Fix\n");
    printf("Grid: %d×%d image, %d×%d tiles of %dpx each\n", W,H,TX,TY,TILE);

    build_icosahedron();
    build_gp20_faces();

    /* Fine geo threshold for sampling (radians angular gap) */
    double geo_thr = 0.08;
    printf("\nAnalyzing tiles (geo_thr=%.3f)...\n", geo_thr);
    analyze_tiles(geo_thr);

    /* GT threshold: ≥15% pixels hit = boundary tile */
    double gt_thr = 0.15;

    /* ── O14 baseline reproduction ── */
    printf("\n── O14 Baseline (raw pent_hit > sz_thr) ──\n");
    {
        int result[TY][TX];
        double sz_thr = 52.0;  /* maps to 52/1024 ≈ 5% of tile */
        /* O14 worked on 16×16 grid with 32px tiles = 1024px/tile */
        /* Here we use pent_hit directly as blob_sz proxy */
        for (int ty=0;ty<TY;ty++)
            for (int tx=0;tx<TX;tx++)
                result[ty][tx] = (tiles[ty][tx].pent_hit >= (int)sz_thr);

        int tp,fp,fn,tn;
        evaluate(result, gt_thr, &tp,&fp,&fn,&tn);
        double prec = tp/(double)(tp+fp+1e-9);
        double rec  = tp/(double)(tp+fn+1e-9);
        double f1   = 2*prec*rec/(prec+rec+1e-9);
        printf("  Precision=%.2f  Recall=%.2f  F1=%.2f  (TP=%d FP=%d FN=%d)\n",
               prec, rec, f1, tp, fp, fn);
    }

    /* ── O15: lat-corrected + neighbor vote ── */
    printf("\n── O15: Lat-correction + Neighbor Vote ──\n");
    {
        int result[TY][TX];

        /* Step 1: per-tile classification with lat correction for poles */
        double sz_thr_base = 52.0;
        double sz_thr_pole = 35.0;  /* relaxed for poles (corrected_hit higher) */
        for (int ty=0;ty<TY;ty++)
            for (int tx=0;tx<TX;tx++)
                result[ty][tx] = classify_tile_o15(tx, ty, sz_thr_base, sz_thr_pole);

        printf("  After lat-correction:\n");
        {
            int tp,fp,fn,tn;
            evaluate(result, gt_thr, &tp,&fp,&fn,&tn);
            double prec = tp/(double)(tp+fp+1e-9);
            double rec  = tp/(double)(tp+fn+1e-9);
            double f1   = 2*prec*rec/(prec+rec+1e-9);
            printf("    Precision=%.2f  Recall=%.2f  F1=%.2f  (TP=%d FP=%d FN=%d)\n",
                   prec, rec, f1, tp, fp, fn);
        }

        /* Step 2: neighbor vote (promote tiles with ≥2 positive neighbors) */
        neighbor_vote(result, 2);

        printf("  After neighbor vote (thr=2):\n");
        {
            int tp,fp,fn,tn;
            evaluate(result, gt_thr, &tp,&fp,&fn,&tn);
            double prec = tp/(double)(tp+fp+1e-9);
            double rec  = tp/(double)(tp+fn+1e-9);
            double f1   = 2*prec*rec/(prec+rec+1e-9);
            printf("    Precision=%.2f  Recall=%.2f  F1=%.2f  (TP=%d FP=%d FN=%d)\n",
                   prec, rec, f1, tp, fp, fn);
        }

        /* ── diagnostic: show pole tiles detail ── */
        printf("\n  Pole tile detail (ty=0,1,14,15):\n");
        printf("  %-6s %-6s %-10s %-12s %-12s %-8s %-6s\n",
               "tx","ty","pent_hit","corrected","lat_scale","gt","pred");
        for (int ty_=0; ty_<TY; ty_++) {
            if (ty_!=0 && ty_!=1 && ty_!=14 && ty_!=15) continue;
            for (int tx_=0; tx_<TX; tx_++) {
                TileInfo *t = &tiles[ty_][tx_];
                int g = (t->pent_hit >= (int)(TILE*TILE*gt_thr));
                printf("  %-6d %-6d %-10d %-12.1f %-12.3f %-8s %-6s\n",
                       tx_, ty_, t->pent_hit,
                       t->corrected_hit, t->lat_scale,
                       g?"BOUND":"hex",
                       result[ty_][tx_]?"PRED":"    ");
            }
        }

        /* render */
        uint8_t *buf = calloc(W*H*3, 1);
        render_result(buf, result, gt_thr);
        bmp_save("o15_result.bmp", buf, W, H);
        free(buf);
        printf("\n  Saved: o15_result.bmp\n");
        printf("  Green=TP  Red=FP  Yellow=FN  Dark=TN\n");
    }

    /* ── natural gap analysis (pole tiles only) ── */
    printf("\n── Corrected-hit distribution (poles) ──\n");
    printf("  ty | tx | raw | corrected | lat_scale\n");
    for (int ty_=0;ty_<TY;ty_++) {
        if (ty_!=1 && ty_!=14) continue;
        for (int tx_=0;tx_<TX;tx_++) {
            TileInfo *t = &tiles[ty_][tx_];
            if (t->pent_hit > 10)
                printf("  %2d | %2d | %4d | %9.1f | %.3f\n",
                       ty_,tx_,t->pent_hit,t->corrected_hit,t->lat_scale);
        }
    }

    /* ── SESSION HANDOFF ── */
    printf("\n══ O15 SESSION HANDOFF ══\n");
    printf("Pole FN fix: lat correction (1/cos(lat)) + neighbor vote(≥2)\n");
    printf("Key constants:\n");
    printf("  sz_thr_base = 52  (non-pole)\n");
    printf("  sz_thr_pole = 35  (pole, applied to corrected_hit)\n");
    printf("  neighbor_vote = 2 (require ≥2 positive neighbors to promote)\n");
    printf("  geo_thr = %.3f rad (sphere boundary sampling)\n", geo_thr);
    printf("  gt_thr  = %.2f   (≥%.0f%% pixels = GT boundary)\n",
           gt_thr, gt_thr*100);
    printf("\nO16 path options:\n");
    printf("  A) Encode real photo → verify gap survives non-synthetic input\n");
    printf("  B) Write tile_id into pixel value → direct pentagon address\n");
    printf("  C) Production: compile address scheme into POGLS tile_classify()\n");

    return 0;
}

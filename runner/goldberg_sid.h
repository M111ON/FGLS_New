#ifndef GOLDBERG_SID_H
#define GOLDBERG_SID_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "th_grid.h"
#include "tri_hex_tess.h"

/* goldberg-bond graph: same pattern as BondGraph in bond_discovery.h */
typedef struct {
    int a;
    int b;
    float weight;
} GoldbergBond;

typedef struct {
    GoldbergBond *bonds;
    int max_bonds;
    int n_bonds;
} GoldbergBondGraph;

static void goldberg_bond_graph_init(GoldbergBondGraph *g, int max_bonds) {
    if (max_bonds <= 0) max_bonds = 1024;
    g->bonds = (GoldbergBond*)calloc((size_t)max_bonds, sizeof(GoldbergBond));
    g->max_bonds = max_bonds;
    g->n_bonds = 0;
}

static void goldberg_bond_graph_free(GoldbergBondGraph *g) {
    free(g->bonds);
    g->bonds = NULL;
    g->n_bonds = 0;
    g->max_bonds = 0;
}

/* ── 12 face centers = icosahedron vertices (dual of dodecahedron) ── */
#define GOLDBERG_PHI 1.618033988749895
#define GOLDBERG_NORM 0.525731112119054  /* 1/sqrt(1+phi^2) */

static void goldberg_face_center(int face, double *x, double *y, double *z) {
    static const double verts[12][3] = {
        {0, 1, GOLDBERG_PHI}, {0, -1, GOLDBERG_PHI},
        {0, 1, -GOLDBERG_PHI}, {0, -1, -GOLDBERG_PHI},
        {1, GOLDBERG_PHI, 0}, {-1, GOLDBERG_PHI, 0},
        {1, -GOLDBERG_PHI, 0}, {-1, -GOLDBERG_PHI, 0},
        {GOLDBERG_PHI, 0, 1}, {-GOLDBERG_PHI, 0, 1},
        {GOLDBERG_PHI, 0, -1}, {-GOLDBERG_PHI, 0, -1},
    };
    int f = face % 12;
    *x = verts[f][0] * GOLDBERG_NORM;
    *y = verts[f][1] * GOLDBERG_NORM;
    *z = verts[f][2] * GOLDBERG_NORM;
}

/* ── tangent vectors for each face: two orthogonal directions in the tangent plane ── */
static void goldberg_tangent_basis(int face, double *u, double *v) {
    double cx, cy, cz;
    goldberg_face_center(face, &cx, &cy, &cz);
    double up[3] = {0, 0, 1};
    if (fabs(cz) > 0.99) { up[0] = 1; up[1] = 0; up[2] = 0; }
    double t1[3];
    double dot = cx*up[0] + cy*up[1] + cz*up[2];
    t1[0] = up[0] - dot*cx;
    t1[1] = up[1] - dot*cy;
    t1[2] = up[2] - dot*cz;
    double ln = sqrt(t1[0]*t1[0] + t1[1]*t1[1] + t1[2]*t1[2]);
    if (ln > 1e-12) { t1[0]/=ln; t1[1]/=ln; t1[2]/=ln; }
    else { t1[0]=1; t1[1]=0; t1[2]=0; }
    double t2[3] = {cy*t1[2] - cz*t1[1], cz*t1[0] - cx*t1[2], cx*t1[1] - cy*t1[0]};
    ln = sqrt(t2[0]*t2[0] + t2[1]*t2[1] + t2[2]*t2[2]);
    if (ln > 1e-12) { t2[0]/=ln; t2[1]/=ln; t2[2]/=ln; }
    u[0] = t1[0]; u[1] = t1[1]; u[2] = t1[2];
    v[0] = t2[0]; v[1] = t2[1]; v[2] = t2[2];
}

/* Map THCoord → 3D unit vector on the goldberg sphere */
static void goldberg_from_thcoord(THCoord c, double *ox, double *oy, double *oz) {
    int face = th_pentagon(c) - 1;
    double cx, cy, cz;
    goldberg_face_center(face, &cx, &cy, &cz);
    uint16_t local = th_local(c.node_id);
    /* decompose local: sector = local / 6, slot = local % 6 */
    uint8_t sector = (uint8_t)(local / TW_SLOTS_PER);
    uint8_t slot   = (uint8_t)(local % TW_SLOTS_PER);
    double tu[3], tv[3];
    goldberg_tangent_basis(face, &tu[0], &tv[0]);
    double angle = sector * 3.141592653589793 / 3.0;
    double dir_u = cos(angle), dir_v = sin(angle);
    double offset = (double)(slot + 1) * 0.02;
    double px = cx + offset * (dir_u * tu[0] + dir_v * tv[0]);
    double py = cy + offset * (dir_u * tu[1] + dir_v * tv[1]);
    double pz = cz + offset * (dir_u * tu[2] + dir_v * tv[2]);
    double ln = sqrt(px*px + py*py + pz*pz);
    if (ln > 1e-12) { px/=ln; py/=ln; pz/=ln; }
    *ox = px; *oy = py; *oz = pz;
}

/* Great-circle distance in radians between two THCoords */
static double goldberg_geodesic(THCoord a, THCoord b) {
    double ax, ay, az, bx, by, bz;
    goldberg_from_thcoord(a, &ax, &ay, &az);
    goldberg_from_thcoord(b, &bx, &by, &bz);
    double dot = ax*bx + ay*by + az*bz;
    if (dot > 1.0) dot = 1.0;
    if (dot < -1.0) dot = -1.0;
    return acos(dot);
}

/* Bond strength [0,1] from geodesic distance, using gaussian falloff */
static double goldberg_bond_strength(THCoord a, THCoord b) {
    double geo = goldberg_geodesic(a, b);
    double sigma = 0.5;
    return exp(-geo*geo / (2.0*sigma*sigma));
}

/* Cardioid hotness: same formula as hex_grid */
static float goldberg_cardioid_hotness(int layer, int n_layers) {
    if (n_layers <= 0) n_layers = 32;
    double pos = (double)layer * 720.0 / (double)n_layers;
    int ipos = (int)pos;
    double frac = pos - ipos;
    int a = ipos % 720, b = (ipos + 1) % 720;
    double ca = _th_cos_lut[a] / 255.0, cb = _th_cos_lut[b] / 255.0;
    double cos_val = ca + (cb - ca) * frac;
    double theta = 2.0 * 3.141592653589793 * pos / 720.0;
    double r = 1.0 + cos_val;
    double cx = r * cos(theta), cy = r * sin(theta);
    double card_dist = sqrt(cx*cx + cy*cy);
    if (card_dist < 0.5) return 0.1f;
    if (card_dist < 1.0) return 0.5f;
    return 1.0f;
}

/* Discover bonds between all pairs within geodesic threshold */
static void goldberg_discover_bonds(GoldbergBondGraph *g, const THCoord *coords, int n, double threshold) {
    for (int i = 0; i < n && g->n_bonds < g->max_bonds; i++) {
        for (int j = i+1; j < n && g->n_bonds < g->max_bonds; j++) {
            double geo = goldberg_geodesic(coords[i], coords[j]);
            if (geo > threshold) continue;
            GoldbergBond *b = &g->bonds[g->n_bonds++];
            b->a = i; b->b = j;
            b->weight = (float)exp(-geo*geo / (2.0*0.3*0.3));
        }
    }
}

/* 3-pass hotness propagation (same algorithm as hex_grid) */
static float* goldberg_predict_hotness(GoldbergBondGraph *g, const float *init_h, int n) {
    float *h = (float*)calloc((size_t)n, sizeof(float));
    for (int i = 0; i < n; i++) h[i] = init_h[i];
    for (int pass = 0; pass < 3; pass++) {
        float *next = (float*)calloc((size_t)n, sizeof(float));
        for (int i = 0; i < n; i++) {
            float self = h[i] * 0.7f;
            float sum_neighbor = 0.0f;
            int n_neighbor = 0;
            for (int b = 0; b < g->n_bonds; b++) {
                if (g->bonds[b].a == i) {
                    sum_neighbor += g->bonds[b].weight * h[g->bonds[b].b];
                    n_neighbor++;
                } else if (g->bonds[b].b == i) {
                    sum_neighbor += g->bonds[b].weight * h[g->bonds[b].a];
                    n_neighbor++;
                }
            }
            float neighbor_contrib = (n_neighbor > 0) ? (sum_neighbor / n_neighbor) * 0.3f : 0.3f * h[i];
            next[i] = self + neighbor_contrib;
            if (next[i] > 1.0f) next[i] = 1.0f;
        }
        memcpy(h, next, (size_t)n * sizeof(float));
        free(next);
    }
    return h;
}

/* Print goldberg coordinates for all tensors */
static void goldberg_print_coords(const char **names, int n, const THCoord *coords) {
    fprintf(stderr, "[goldberg] %d tensors on goldberg sphere\n", n);
    for (int i = 0; i < n && i < 16; i++) {
        double x, y, z;
        goldberg_from_thcoord(coords[i], &x, &y, &z);
        fprintf(stderr, "  pentagon=%d node=%u → (%.4f, %.4f, %.4f) %s\n",
                th_pentagon(coords[i]), coords[i].node_id, x, y, z, names[i]);
    }
    if (n > 16) fprintf(stderr, "  ... (%d more)\n", n - 16);
}

/* Print hotness predictions */
static void goldberg_predict_print(const char **names, int n, const float *h) {
    int hot=0, warm=0, cold=0;
    for (int i = 0; i < n; i++) {
        if (h[i] >= 0.9f) hot++;
        else if (h[i] >= 0.3f) warm++;
        else cold++;
    }
    fprintf(stderr, "[goldberg] hot=%d warm=%d cold=%d (%.1f%% %.1f%% %.1f%%)\n",
            hot, warm, cold,
            100.0*hot/n, 100.0*warm/n, 100.0*cold/n);
    fprintf(stderr, "[goldberg] top-5 hottest:\n");
    float *copy = (float*)calloc((size_t)n, sizeof(float));
    for (int i = 0; i < n; i++) copy[i] = h[i];
    for (int printed = 0; printed < 5 && printed < n; printed++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (copy[i] >= 0.0f && (best < 0 || copy[i] > copy[best]))
                best = i;
        }
        if (best >= 0) {
            fprintf(stderr, "  %.2f  %s\n", copy[best], names[best]);
            copy[best] = -1.0f;
        } else break;
    }
    free(copy);
}
#undef GOLDBERG_PHI
#undef GOLDBERG_NORM

#endif

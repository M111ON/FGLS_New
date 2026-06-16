/*
 * hex_grid.h — Isomorphic Hex Grid for SID Bond Discovery
 * ═══════════════════════════════════════════════════════
 *
 * ใช้ hex grid (axial q,r) เป็น universal spatial index
 * สำหรับทุก tensor. Aperture hierarchy ควบคุม resolution:
 *
 *   level 1 (ap-4 coarse) : norm+weight อยู่ cell เดียว
 *   level 2 (ap-7 mid)    : layer+slot separation
 *   level 3 (ap-3 fine)   : layer+slot+face separation
 *
 * Feature toggle: level==0 → hex grid off
 *
 * "ray + angle" concept:
 *   tensor → project ลง ray ที่ angle → snap to hex center
 *   → 1D priority ordering ตาม ray position
 *   → 2D hex distance = bond strength
 *
 * Isomorphic: ใช้ grid logic เดียวกันกับทุก model scale
 *   hex_radius = container_edge / (aperture * level_factor)
 *   container_edge = n_layers (default)
 */

#ifndef HEX_GRID_H
#define HEX_GRID_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ══════════════════════════════════════════════════════════════════
   HEX COORDINATE (axial q,r + cube s = -q-r)
   ══════════════════════════════════════════════════════════════════ */
typedef struct {
    int q, r;
} HexCoord;

static inline int hex_distance(HexCoord a, HexCoord b) {
    int dq = a.q - b.q;
    int dr = a.r - b.r;
    int ds = -dq - dr;   /* x + y + z = 0 constraint */
    int d = abs(dq);
    if (abs(dr) > d) d = abs(dr);
    if (abs(ds) > d) d = abs(ds);
    return d;
}

/* ─── ray position along angle: 1D priority ordering ─── */
static inline float hex_ray_pos(int layer, int slot, float angle) {
    return (float)layer * cosf(angle) + (float)slot * sinf(angle);
}

/* ══════════════════════════════════════════════════════════════════
   HEX GRID STATE
   ══════════════════════════════════════════════════════════════════ */
#define HEX_APERTURE_4  4
#define HEX_APERTURE_7  7
#define HEX_APERTURE_3  3

/* default aperture per level */
static inline int hex_aperture_for_level(int level) {
    switch (level) {
        case 1: return HEX_APERTURE_4;   /* coarse */
        case 2: return HEX_APERTURE_7;   /* mid (recommended default) */
        case 3: return HEX_APERTURE_3;   /* fine */
        default: return 0;               /* off */
    }
}

typedef struct {
    float radius;          /* hex cell radius (circumradius) */
    float angle;           /* ray rotation in radians */
    int   level;           /* 0=off, 1=coarse, 2=mid, 3=fine */
    int   aperture;        /* 4, 7, or 3 */
    float container_edge;  /* reference dimension (e.g. n_layers) */
    int   scale_divisor;   /* radius = container_edge / scale_divisor */
} HexGridState;

static HexGridState _hex_grid = {0};

/* ─── init: set level → aperture + radius from container_edge ─── */
static inline void hex_grid_init(HexGridState *g, int level, float container_edge) {
    if (level < 1) { memset(g, 0, sizeof(*g)); return; }
    g->level = level;
    g->container_edge = container_edge > 0 ? container_edge : 32.0f;
    g->aperture = hex_aperture_for_level(level);
    g->angle = 0.0f;  /* default: layer axis */

    /* scale_divisor = aperture * level */
    /* radius = container_edge / (aperture * level) */
    /* bigger divisor = smaller cells = higher resolution */
    g->scale_divisor = g->aperture * level;
    g->radius = g->container_edge / (float)g->scale_divisor;
}

/* ─── init from custom aperture (override level→aperture map) ─── */
static inline void hex_grid_init_ap(HexGridState *g, int level, int aperture,
                                     float container_edge, float angle) {
    if (level < 1) { memset(g, 0, sizeof(*g)); return; }
    g->level = level;
    g->container_edge = container_edge > 0 ? container_edge : 32.0f;
    g->aperture = aperture > 0 ? aperture : HEX_APERTURE_7;
    g->angle = angle;
    g->scale_divisor = g->aperture * level;
    g->radius = g->container_edge / (float)g->scale_divisor;
}

/* ══════════════════════════════════════════════════════════════════
   TENSOR → HEX COORD
   ══════════════════════════════════════════════════════════════════ */

/* ─── normalize slot name to integer index ─── */
static inline int hex_slot_index(const char *name) {
    /* attn weights */
    if (strstr(name, "attn_q"))      return 0;
    if (strstr(name, "attn_k"))      return 1;
    if (strstr(name, "attn_v"))      return 2;
    if (strstr(name, "attn_output")) return 3;
    /* ffn weights */
    if (strstr(name, "ffn_gate"))    return 4;
    if (strstr(name, "ffn_up"))      return 5;
    if (strstr(name, "ffn_down"))    return 6;
    if (strstr(name, "ffn_out"))     return 6;
    /* norm */
    if (strstr(name, "attn_norm"))   return 7;
    if (strstr(name, "ffn_norm"))    return 8;
    /* embedding / output */
    if (strstr(name, "tok_embd"))    return 0;
    if (strstr(name, "output"))      return 6;
    return -1;
}

/* ─── extract layer index from tensor name ─── */
static inline int hex_extract_layer(const char *name) {
    int layer = -1;
    if (sscanf(name, "blk.%d.", &layer) == 1) return layer;
    return -1;
}

/* ─── tensor → HexCoord with rotation + hex snap ─── */
static inline HexCoord tensor_to_hex(int layer, int slot_idx, const HexGridState *g) {
    HexCoord h = {0, 0};
    if (!g || g->level <= 0 || g->radius <= 0.0f) return h;

    /* rotate (layer, slot) by angle */
    float lr = (float)layer * cosf(g->angle) - (float)slot_idx * sinf(g->angle);
    float sr = (float)layer * sinf(g->angle) + (float)slot_idx * cosf(g->angle);

    /* convert to axial hex coordinates with 60° skew */
    /* standard axial: q = x, r = (x + 2*z) / 2 ... simplified */
    float skew = sr - lr * 0.5f;  /* shear for 60° */
    float qf = lr / g->radius;
    float rf = skew / (g->radius * 0.8660254f);  /* sqrt(3)/2 */

    /* snap: find nearest hex center */
    float rq = roundf(qf);
    float rr = roundf(rf);

    /* axial rounding to ensure cube constraint |dq+dr+ds| <= 0.5 */
    float dq = rq - qf;
    float dr = rr - rf;
    float ds = -dq - dr;

    if (fabsf(ds) > 0.5f) {
        /* adjust the farthest coordinate */
        if (fabsf(dq) >= fabsf(dr) && fabsf(dq) >= fabsf(ds))
            rq = qf - ds;
        else if (fabsf(dr) >= fabsf(dq) && fabsf(dr) >= fabsf(ds))
            rr = rf - ds;
    }

    h.q = (int)rq;
    h.r = (int)rr;
    return h;
}

/* ─── convenience: tensor name → HexCoord ─── */
static inline HexCoord hex_from_name(const char *name, const HexGridState *g) {
    int layer = hex_extract_layer(name);
    int slot = hex_slot_index(name);
    if (layer < 0) layer = 0;
    if (slot < 0) slot = 0;
    return tensor_to_hex(layer, slot, g);
}

/* ══════════════════════════════════════════════════════════════════
   BOND STRENGTH FROM HEX DISTANCE
   ══════════════════════════════════════════════════════════════════ */

#define HEX_BOND_LUT_SIZE 16

/* bond strength LUT: hex_distance → 0.0 .. 1.0 */
static const float _hex_bond_lut[HEX_BOND_LUT_SIZE] = {
    1.00f,   /* d=0: same cell */
    0.95f,   /* d=1: adjacent */
    0.85f,   /* d=2: neighbor's neighbor */
    0.70f,   /* d=3 */
    0.55f,   /* d=4 */
    0.40f,   /* d=5 */
    0.30f,   /* d=6 */
    0.20f,   /* d=7 */
    0.15f,   /* d=8 */
    0.10f,   /* d=9 */
    0.05f,   /* d=10 */
    0.02f,   /* d=11 */
    0.01f,   /* d=12 */
    0.005f,  /* d=13 */
    0.002f,  /* d=14 */
    0.001f,  /* d=15 */
};

static inline float hex_bond_strength(int distance) {
    if (distance < 0) distance = -distance;
    if (distance >= HEX_BOND_LUT_SIZE) return 0.001f;
    return _hex_bond_lut[distance];
}

/* ─── bond type label from distance ─── */
static inline const char* hex_bond_label(int distance) {
    if (distance == 0) return "HEX_COINCIDENT";
    if (distance == 1) return "HEX_ADJACENT";
    if (distance == 2) return "HEX_2STEP";
    if (distance <= 4) return "HEX_CLOSE";
    if (distance <= 8) return "HEX_MID";
    return "HEX_FAR";
}

/* ══════════════════════════════════════════════════════════════════
   HEX-BASED BOND DISCOVERY (replaces topology types)
   ══════════════════════════════════════════════════════════════════ */

/* Similar to Bond from bond_discovery.h but with hex distance */
typedef struct {
    int    a;          /* tensor index */
    int    b;
    int    distance;   /* hex_distance */
    float  strength;   /* hex_bond_strength(distance) */
    char   label[64];
    HexCoord ha, hb;   /* hex coords for debugging */
} HexBond;

typedef struct {
    HexBond *bonds;
    int max_bonds;
    int n_bonds;
} HexBondGraph;

static inline void hex_bond_graph_init(HexBondGraph *g, int max_bonds) {
    g->bonds = (HexBond*)calloc((size_t)max_bonds, sizeof(HexBond));
    g->max_bonds = max_bonds;
    g->n_bonds = 0;
}

static inline void hex_bond_graph_free(HexBondGraph *g) {
    free(g->bonds);
    g->bonds = NULL;
    g->n_bonds = 0;
    g->max_bonds = 0;
}

/* ─── discover all bonds via hex distance ───
   For every pair of tensors, compute hex_distance → bond.
   Only adds bond if distance ≤ max_dist.
   This REPLACES bond_is_weight filtering — hex distance
   naturally separates coincident (norm+weight) from distant. */
static inline void hex_discover_bonds(HexBondGraph *g,
                                       const char **names, int n_names,
                                       const HexGridState *grid,
                                       int max_dist) {
    HexCoord *coords = (HexCoord*)calloc((size_t)n_names, sizeof(HexCoord));
    for (int i = 0; i < n_names; i++) {
        coords[i] = hex_from_name(names[i], grid);
    }

    for (int i = 0; i < n_names; i++) {
        for (int j = i + 1; j < n_names; j++) {
            int d = hex_distance(coords[i], coords[j]);
            if (d > max_dist) continue;
            if (g->n_bonds >= g->max_bonds) goto done;
            HexBond *b = &g->bonds[g->n_bonds++];
            b->a = i; b->b = j;
            b->distance = d;
            b->strength = hex_bond_strength(d);
            b->ha = coords[i]; b->hb = coords[j];
            snprintf(b->label, sizeof(b->label), "%s↔%s d=%d %s",
                     names[i], names[j], d, hex_bond_label(d));
        }
    }
done:
    free(coords);
}

/* ─── hex bond summary ─── */
static inline void hex_bond_print_summary(const HexBondGraph *g) {
    int counts[16] = {0};
    for (int i = 0; i < g->n_bonds; i++) {
        int d = g->bonds[i].distance;
        if (d >= 16) d = 15;
        counts[d]++;
    }
    fprintf(stderr, "[hex] %d total bonds (max_dist=%d):\n", g->n_bonds, 15);
    for (int d = 0; d < 8; d++) {
        if (counts[d] > 0)
            fprintf(stderr, "  d=%d (%s): %d\n", d, hex_bond_label(d), counts[d]);
    }
}

/* ══════════════════════════════════════════════════════════════════
   HOTNESS PREDICTION VIA HEX BOND GRAPH
   ══════════════════════════════════════════════════════════════════ */

/* cardioid hotness (reused from bond_discovery.h but inline here) */
#define HEX_CARDIOID_LEN 720u
#define HEX_CARDIOID_SCALE 256
#define HEX_CARDIOID_A 256
#define HEX_CARDIOID_GEO_MIN 140

static int16_t _hex_cos_lut[HEX_CARDIOID_LEN];
static uint8_t _hex_cardioid_init = 0;

static inline void hex_cardioid_lut_init(void) {
    if (_hex_cardioid_init) return;
    for (uint32_t i = 0; i < HEX_CARDIOID_LEN; i++) {
        double theta = (2.0 * 3.141592653589793 * i) / HEX_CARDIOID_LEN;
        _hex_cos_lut[i] = (int16_t)(cos(theta) * 255.0);
    }
    _hex_cardioid_init = 1;
}

static inline float hex_cardioid_hotness(int layer, int n_layers) {
    uint16_t pos = (uint16_t)((uint32_t)layer * HEX_CARDIOID_LEN / (uint32_t)(n_layers > 0 ? n_layers : 1)) % HEX_CARDIOID_LEN;
    int32_t cos_val = _hex_cos_lut[pos];
    int32_t r_geo = HEX_CARDIOID_A * (HEX_CARDIOID_SCALE + cos_val);
    int32_t r_geo_q8 = r_geo >> 8;
    if (r_geo_q8 < HEX_CARDIOID_GEO_MIN) return 0.1f;
    uint16_t signal = (uint16_t)((uint32_t)(layer * 17 + 42) & 0xFF);
    int pass = (((int32_t)signal << 8) <= r_geo);
    return pass ? 1.0f : 0.5f;
}

/* ─── predict hotness using hex bond graph propagation ───
   Returns hotness array (caller frees). n_layers = total model layers. */
static inline float *hex_predict_hotness(const HexBondGraph *g,
                                          const char **names, int n_names,
                                          int n_layers) {
    hex_cardioid_lut_init();
    float *h = (float*)calloc((size_t)n_names, sizeof(float));
    for (int i = 0; i < n_names; i++) {
        int layer = hex_extract_layer(names[i]);
        h[i] = layer >= 0 ? hex_cardioid_hotness(layer, n_layers) : 0.5f;
        /* whitelist: norm.weight always ≥ 0.5 (critical for inference) */
        /* norm.weight critical — even if cardioid says cold, keep warm (0.5) */
        if (h[i] < 0.5f && strstr(names[i], "norm.weight"))
            h[i] = 0.5f;
    }
    /* propagate through hex bonds: 3 passes */
    for (int pass = 0; pass < 3; pass++) {
        float *acc = (float*)calloc((size_t)n_names, sizeof(float));
        float *wsum = (float*)calloc((size_t)n_names, sizeof(float));
        for (int b = 0; b < g->n_bonds; b++) {
            int a = g->bonds[b].a;
            int bi = g->bonds[b].b;
            float w = g->bonds[b].strength;
            acc[a]  += h[bi] * w;  wsum[a]  += w;
            acc[bi] += h[a]  * w;  wsum[bi] += w;
        }
        for (int i = 0; i < n_names; i++) {
            if (wsum[i] > 0.0f) {
                float avg = acc[i] / wsum[i];
                h[i] = h[i] * 0.7f + avg * 0.3f;
                if (h[i] < 0.0f) h[i] = 0.0f;
                if (h[i] > 1.0f) h[i] = 1.0f;
            }
        }
        free(acc);
        free(wsum);
    }
    return h;
}

static inline void hex_predict_print(const char **names, int n_names,
                                      const float *hotness) {
    int n_hot = 0, n_warm = 0, n_cold = 0;
    for (int i = 0; i < n_names; i++) {
        if (hotness[i] >= 0.9f) n_hot++;
        else if (hotness[i] >= 0.3f) n_warm++;
        else n_cold++;
    }
    fprintf(stderr, "[hex] hot=%.1f%% warm=%.1f%% cold=%.1f%%\n",
            100.0 * n_hot / n_names,
            100.0 * n_warm / n_names,
            100.0 * n_cold / n_names);
    fprintf(stderr, "[hex] top-5 hottest:\n");
    uint8_t *visited = (uint8_t*)calloc((size_t)n_names, 1);
    for (int rank = 0; rank < 5; rank++) {
        int best = -1; float best_h = -1.0f;
        for (int i = 0; i < n_names; i++) {
            if (visited[i]) continue;
            if (hotness[i] > best_h) { best_h = hotness[i]; best = i; }
        }
        if (best < 0) break;
        visited[best] = 1;
        fprintf(stderr, "  %.2f  %s\n", hotness[best], names[best]);
    }
    free(visited);
}

/* ══════════════════════════════════════════════════════════════════
   DEBUG: print hex coord for each tensor
   ══════════════════════════════════════════════════════════════════ */
static inline void hex_print_coords(const char **names, int n_names,
                                     const HexGridState *grid) {
    fprintf(stderr, "[hex] grid: level=%d aperture=%d radius=%.2f angle=%.2f° container=%.0f\n",
            grid->level, grid->aperture, grid->radius,
            grid->angle * 180.0f / 3.14159265f, grid->container_edge);
    for (int i = 0; i < n_names; i++) {
        HexCoord h = hex_from_name(names[i], grid);
        fprintf(stderr, "  (%2d,%2d)  %s\n", h.q, h.r, names[i]);
    }
}

#endif

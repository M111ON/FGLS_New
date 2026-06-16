#ifndef BOND_DISCOVERY_H
#define BOND_DISCOVERY_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_BONDS 8192
#define NAME_MAX 64

/* skip bias tensors only — norm.weight tensors are valid bond nodes */
static inline int bond_is_weight(const char *name) {
    if (strstr(name, "bias")) return 0;
    const char *nw = strstr(name, "norm");
    if (nw && !strstr(nw, ".weight")) return 0;
    return 1;
}

typedef enum {
    BOND_LAYER_SLOT,     /* same slot across consecutive layers */
    BOND_INTRA_LAYER,    /* different slots in same layer */
    BOND_MEM_ADJACENT,   /* adjacent in virtual memory */
    BOND_SIZE_MATCH,     /* same total byte size */
    BOND_COMPONENT,      /* same sub-module prefix */
    BOND_CARDIOID_PHASE, /* same cardioid express phase */
    BOND_METATRON_ORB,   /* metatron orbital: same face, next slot */
    BOND_METATRON_CHIRAL,/* metatron chiral: opposite face (diameter) */
    BOND_METATRON_CROSS, /* metatron cross: inter-ring bijection */
    BOND_METATRON_HUB,   /* metatron hub: any-face connection */
} BondType;

typedef struct {
    int a;               /* index into found_tensors */
    int b;               /* index into found_tensors */
    BondType type;
    float weight;        /* bond strength (0..1) */
    char label[64];
} Bond;

typedef struct {
    Bond *bonds;
    int max_bonds;
    int n_bonds;
} BondGraph;

typedef struct {
    void   *ptr;
    void   *orig_data;
    char    name[NAME_MAX];
    size_t  nbytes;
    int64_t ne[4];
    uint32_t dtype;
} BondTensorInfo;

typedef struct {
    BondTensorInfo *tensors;
    int n_tensors;
    BondGraph graph;
} BondCtx;

static void bond_graph_init(BondGraph *g, int max_bonds) {
    if (max_bonds <= 0) max_bonds = 1024;
    g->bonds = (Bond*)calloc((size_t)max_bonds, sizeof(Bond));
    g->max_bonds = max_bonds;
    g->n_bonds = 0;
}

static void bond_graph_free(BondGraph *g) {
    free(g->bonds);
    g->bonds = NULL;
    g->n_bonds = 0;
    g->max_bonds = 0;
}

static inline int bond_extract_layer(const char *name) {
    int layer = -1;
    if (sscanf(name, "blk.%d.", &layer) == 1) return layer;
    return -1;
}

/* extract slot e.g. "attn_q" from "blk.0.attn_q.weight" */
static inline int bond_extract_slot(const char *name, char *out, int max_out) {
    const char *last = strrchr(name, '.');
    if (!last) return -1;
    const char *prev = last - 1;
    while (prev >= name && *prev != '.') prev--;
    if (prev < name) return -1;
    int len = (int)(last - prev - 1);
    if (len >= max_out) len = max_out - 1;
    memcpy(out, prev + 1, len);
    out[len] = 0;
    return len;
}

static inline const char* bond_extract_module(const char *name) {
    const char *p = strstr(name, "attn_");
    if (p) { while (p > name && *(p-1) != '.') p--; return p; }
    p = strstr(name, "ffn_");
    if (p) { while (p > name && *(p-1) != '.') p--; return p; }
    return name;
}

static void bond_discover_layer_slot(BondCtx *ctx) {
    char slot_i[64], slot_j[64];
    for (int i = 0; i < ctx->n_tensors; i++) {
        if (!bond_is_weight(ctx->tensors[i].name)) continue;
        int li = bond_extract_layer(ctx->tensors[i].name);
        if (li < 0) continue;
        if (bond_extract_slot(ctx->tensors[i].name, slot_i, 64) < 0) continue;
        for (int j = i+1; j < ctx->n_tensors; j++) {
            if (!bond_is_weight(ctx->tensors[j].name)) continue;
            int lj = bond_extract_layer(ctx->tensors[j].name);
            if (lj != li + 1) continue;
            if (bond_extract_slot(ctx->tensors[j].name, slot_j, 64) < 0) continue;
            if (strcmp(slot_i, slot_j) != 0) continue;
            if (ctx->graph.n_bonds >= ctx->graph.max_bonds) return;
            Bond *b = &ctx->graph.bonds[ctx->graph.n_bonds++];
            b->a = i; b->b = j; b->type = BOND_LAYER_SLOT; b->weight = 0.9f;
            snprintf(b->label, sizeof(b->label), "%s→%s",
                ctx->tensors[i].name, ctx->tensors[j].name);
        }
    }
}

static void bond_discover_intra_layer(BondCtx *ctx) {
    char slot_i[64], slot_j[64];
    for (int i = 0; i < ctx->n_tensors; i++) {
        if (!bond_is_weight(ctx->tensors[i].name)) continue;
        int li = bond_extract_layer(ctx->tensors[i].name);
        if (li < 0) continue;
        if (bond_extract_slot(ctx->tensors[i].name, slot_i, 64) < 0) continue;
        for (int j = i+1; j < ctx->n_tensors; j++) {
            if (!bond_is_weight(ctx->tensors[j].name)) continue;
            int lj = bond_extract_layer(ctx->tensors[j].name);
            if (lj != li) continue;
            if (bond_extract_slot(ctx->tensors[j].name, slot_j, 64) < 0) continue;
            if (ctx->graph.n_bonds >= ctx->graph.max_bonds) return;
            Bond *b = &ctx->graph.bonds[ctx->graph.n_bonds++];
            b->a = i; b->b = j; b->type = BOND_INTRA_LAYER; b->weight = 0.6f;
            snprintf(b->label, sizeof(b->label), "%s↔%s",
                ctx->tensors[i].name, ctx->tensors[j].name);
        }
    }
}

static void bond_discover_mem_adjacent(BondCtx *ctx, uint64_t gap_max) {
    for (int i = 0; i < ctx->n_tensors; i++) {
        if (!bond_is_weight(ctx->tensors[i].name)) continue;
        uint8_t *end_a = (uint8_t*)ctx->tensors[i].ptr + ctx->tensors[i].nbytes;
        int best_j = -1, best_gap = 0x7FFFFFFF;
        for (int j = 0; j < ctx->n_tensors; j++) {
            if (i == j) continue;
            if (!bond_is_weight(ctx->tensors[j].name)) continue;
            uint8_t *start_b = (uint8_t*)ctx->tensors[j].ptr;
            if (start_b < end_a) continue; /* only forward adjacency */
            uint64_t gap = (uint64_t)(start_b - end_a);
            if (gap <= gap_max && (int)gap < best_gap) {
                best_gap = (int)gap; best_j = j;
            }
        }
        if (best_j >= 0 && ctx->graph.n_bonds < MAX_BONDS) {
            Bond *b = &ctx->graph.bonds[ctx->graph.n_bonds++];
            b->a = i; b->b = best_j; b->type = BOND_MEM_ADJACENT;
            b->weight = best_gap == 0 ? 0.8f : 0.5f;
            snprintf(b->label, sizeof(b->label), "%s--%s gap=%d",
                ctx->tensors[i].name, ctx->tensors[best_j].name, best_gap);
        }
    }
}

static void bond_discover_size_match(BondCtx *ctx) {
    for (int i = 0; i < ctx->n_tensors; i++) {
        for (int j = i+1; j < ctx->n_tensors; j++) {
            if (ctx->tensors[i].nbytes == ctx->tensors[j].nbytes) {
                if (ctx->graph.n_bonds >= ctx->graph.max_bonds) return;
                Bond *b = &ctx->graph.bonds[ctx->graph.n_bonds++];
                b->a = i; b->b = j; b->type = BOND_SIZE_MATCH; b->weight = 0.3f;
                snprintf(b->label, sizeof(b->label), "%s≈%s sz=%llu",
                    ctx->tensors[i].name, ctx->tensors[j].name,
                    (unsigned long long)ctx->tensors[i].nbytes);
            }
        }
    }
}

static void bond_discover_component(BondCtx *ctx) {
    for (int i = 0; i < ctx->n_tensors; i++) {
        const char *mod_i = bond_extract_module(ctx->tensors[i].name);
        int li = bond_extract_layer(ctx->tensors[i].name);
        for (int j = i+1; j < ctx->n_tensors; j++) {
            const char *mod_j = bond_extract_module(ctx->tensors[j].name);
            int lj = bond_extract_layer(ctx->tensors[j].name);
            if (li != lj) continue;
            if (strcmp(mod_i, mod_j) == 0) {
                if (ctx->graph.n_bonds >= ctx->graph.max_bonds) return;
                Bond *b = &ctx->graph.bonds[ctx->graph.n_bonds++];
                b->a = i; b->b = j; b->type = BOND_COMPONENT; b->weight = 0.7f;
                snprintf(b->label, sizeof(b->label), "%s←%s",
                    ctx->tensors[i].name, ctx->tensors[j].name);
            }
        }
    }
}

/* forward declarations */
static void bond_discover_cardioid_express(BondCtx *ctx, int n_layers);
static void bond_discover_metatron_topo(BondCtx *ctx, int n_layers);

static void bond_discover_all(BondCtx *ctx, int n_layers, int max_bonds) {
    bond_graph_init(&ctx->graph, max_bonds);
    bond_discover_layer_slot(ctx);
    bond_discover_intra_layer(ctx);
    bond_discover_mem_adjacent(ctx, 256);
    bond_discover_component(ctx);
    bond_discover_metatron_topo(ctx, n_layers);
    bond_discover_cardioid_express(ctx, n_layers);
}

static void bond_print_summary(const BondCtx *ctx) {
    int counts[10] = {0};
    for (int i = 0; i < ctx->graph.n_bonds; i++)
        counts[ctx->graph.bonds[i].type]++;
    fprintf(stderr, "[bond] %d total bonds:\n", ctx->graph.n_bonds);
    fprintf(stderr, "  LAYER_SLOT:    %d\n", counts[0]);
    fprintf(stderr, "  INTRA_LAYER:   %d\n", counts[1]);
    fprintf(stderr, "  MEM_ADJACENT:  %d\n", counts[2]);
    fprintf(stderr, "  SIZE_MATCH:    %d\n", counts[3]);
    fprintf(stderr, "  COMPONENT:     %d\n", counts[4]);
    fprintf(stderr, "  CARDIOID:      %d\n", counts[5]);
    fprintf(stderr, "  META_ORB:      %d\n", counts[6]);
    fprintf(stderr, "  META_CHIRAL:   %d\n", counts[7]);
    fprintf(stderr, "  META_CROSS:    %d\n", counts[8]);
    fprintf(stderr, "  META_HUB:      %d\n", counts[9]);
}

/* ══════════════════════════════════════════════════════════════════
   CARDIOID EXPRESS × SID BOND GRAPH
   Maps each tensor's layer index to a cardioid geometry position,
   scoring "hotness" (express=hot, linear=warm, cusp=cold).
   ══════════════════════════════════════════════════════════════════ */

#define BOND_CARDIOID_LEN 720u
#define BOND_CARDIOID_SCALE 256
#define BOND_CARDIOID_A 256
#define BOND_CARDIOID_GEO_MIN 140

static int16_t _bond_cos_lut[BOND_CARDIOID_LEN];
static uint8_t _bond_cardioid_init = 0;

static void bond_cardioid_lut_init(void) {
    if (_bond_cardioid_init) return;
    for (uint32_t i = 0; i < BOND_CARDIOID_LEN; i++) {
        double theta = (2.0 * 3.141592653589793 * i) / BOND_CARDIOID_LEN;
        _bond_cos_lut[i] = (int16_t)(cos(theta) * 255.0);
    }
    _bond_cardioid_init = 1;
}

/* cardioid_hotness(layer, n_layers) → 0.0..1.0
   Maps layer to cardioid position, returns hotness:
     1.0 = express (inside cardioid)
     0.5 = linear (outside cardioid, not in cusp)
     0.1 = cusp (near θ=π, always blocked)
   signal = (layer * 17 + 42) & 0xFF — deterministic per-layer */
static float bond_cardioid_hotness(int layer, int n_layers) {
    uint16_t pos = (uint16_t)((uint32_t)layer * BOND_CARDIOID_LEN / (uint32_t)(n_layers > 0 ? n_layers : 1)) % BOND_CARDIOID_LEN;
    int32_t cos_val = _bond_cos_lut[pos];
    int32_t r_geo = BOND_CARDIOID_A * (BOND_CARDIOID_SCALE + cos_val);
    int32_t r_geo_q8 = r_geo >> 8;
    if (r_geo_q8 < BOND_CARDIOID_GEO_MIN)
        return 0.1f;
    uint16_t signal = (uint16_t)((uint32_t)(layer * 17 + 42) & 0xFF);
    int pass = (((int32_t)signal << 8) <= r_geo);
    return pass ? 1.0f : 0.5f;
}

/* bond_discover_cardioid_express: connect same-tier tensors in same or
   adjacent layers. Avoids N² explosion by limiting layer distance to 1. */
static void bond_discover_cardioid_express(BondCtx *ctx, int n_layers) {
    bond_cardioid_lut_init();
    int n = ctx->n_tensors;
    float *hotness = (float*)calloc((size_t)n, sizeof(float));
    int *tier = (int*)calloc((size_t)n, sizeof(int));
    int *layer_arr = (int*)calloc((size_t)n, sizeof(int));
    for (int i = 0; i < n; i++) {
        int l = bond_extract_layer(ctx->tensors[i].name);
        layer_arr[i] = l;
        if (l < 0) { tier[i] = -1; continue; }
        hotness[i] = bond_cardioid_hotness(l, n_layers);
        tier[i] = (hotness[i] >= 0.9f) ? 0 : (hotness[i] >= 0.3f ? 1 : 2);
    }
    for (int i = 0; i < n; i++) {
        if (tier[i] < 0) continue;
        for (int j = i + 1; j < n; j++) {
            if (tier[j] < 0 || tier[i] != tier[j]) continue;
            if (abs(layer_arr[i] - layer_arr[j]) > 1) continue;
            if (ctx->graph.n_bonds >= ctx->graph.max_bonds) {
                free(hotness); free(tier); free(layer_arr); return;
            }
            Bond *b = &ctx->graph.bonds[ctx->graph.n_bonds++];
            b->a = i; b->b = j; b->type = BOND_CARDIOID_PHASE;
            b->weight = hotness[i] * hotness[j];
            snprintf(b->label, sizeof(b->label), "cardioid_t%d %.2f %s↔%s",
                tier[i], b->weight, ctx->tensors[i].name, ctx->tensors[j].name);
        }
    }
    free(hotness); free(tier); free(layer_arr);
}

/* ══════════════════════════════════════════════════════════════════
   METATRON ROUTE × SID BOND GRAPH
   Maps each tensor to a Metatron face (0..11) and slot (0..59)
   based on slot type + ring. Discovers orbital/chiral/cross/hub bonds.
   ══════════════════════════════════════════════════════════════════ */

#define BOND_TOPO_SZ 60u

/* map slot name → face within ring (0..5) */
static int bond_topo_face_in_ring(const char *name) {
    if (strstr(name, "attn_q"))      return 0;
    if (strstr(name, "attn_k"))      return 1;
    if (strstr(name, "attn_v"))      return 2;
    if (strstr(name, "attn_output")) return 3;
    if (strstr(name, "ffn_gate"))    return 4;
    if (strstr(name, "ffn_up"))      return 5;
    if (strstr(name, "ffn_down"))    return 5;
    if (strstr(name, "ffn_out"))     return 5;
    if (strstr(name, "tok_embd"))    return 4;
    if (strstr(name, "output"))      return 5;
    return -1;
}

/* build tensor → (face, slot) mapping; returns count mapped */
static int bond_build_topo_map(BondCtx *ctx, int n_layers,
                                int *faces, int *slots) {
    int half = n_layers / 2;
    if (half < 1) half = 1;
    int n_mapped = 0;
    for (int i = 0; i < ctx->n_tensors; i++) {
        faces[i] = -1; slots[i] = -1;
        if (!bond_is_weight(ctx->tensors[i].name)) continue;
        int layer = bond_extract_layer(ctx->tensors[i].name);
        if (layer < 0) continue;
        int fir = bond_topo_face_in_ring(ctx->tensors[i].name);
        if (fir < 0) continue;
        int ring = layer / half;
        if (ring > 1) ring = 1;
        faces[i] = fir + ring * 6;
        /* slot = layer index within ring (0..half-1), unspread */
        slots[i] = layer % half;
        n_mapped++;
    }
    return n_mapped;
}

/* bond_discover_metatron_topo: discover orbital, chiral, cross bonds
   Uses Metatron routing primitives on the tensor face+slot mapping */
static void bond_discover_metatron_topo(BondCtx *ctx, int n_layers) {
    int *faces = (int*)calloc((size_t)ctx->n_tensors, sizeof(int));
    int *slots = (int*)calloc((size_t)ctx->n_tensors, sizeof(int));
    int n_mapped = bond_build_topo_map(ctx, n_layers, faces, slots);
    fprintf(stderr, "[bond] metatron: %d/%d tensors mapped\n", n_mapped, ctx->n_tensors);
    if (n_mapped < 2) { free(faces); free(slots); return; }

    /* METATRON_CROSS[12]: ring1→ring2 and back */
    static const uint8_t CROSS_MAP[12] = {
        9,10,11,6,7,8, 3,4,5,0,1,2
    };

    for (int i = 0; i < ctx->n_tensors; i++) {
        if (faces[i] < 0) continue;
        for (int j = i + 1; j < ctx->n_tensors; j++) {
            if (faces[j] < 0) continue;
            if (ctx->graph.n_bonds >= ctx->graph.max_bonds) { free(faces); free(slots); return; }

            int fi = faces[i], fj = faces[j];
            int si = slots[i], sj = slots[j];

            /* orbital: same face, adjacent slot */
            if (fi == fj && (si + 1) % BOND_TOPO_SZ == (uint8_t)sj) {
                Bond *b = &ctx->graph.bonds[ctx->graph.n_bonds++];
                b->a = i; b->b = j; b->type = BOND_METATRON_ORB;
                b->weight = 0.85f;
                snprintf(b->label, sizeof(b->label), "meta_orb f%d %s↔%s",
                    fi, ctx->tensors[i].name, ctx->tensors[j].name);
            }
            /* chiral: opposite face (ring0↔ring1 same type) — face f ↔ f+6 */
            else if ((fi + 6) % 12 == fj && si == sj) {
                Bond *b = &ctx->graph.bonds[ctx->graph.n_bonds++];
                b->a = i; b->b = j; b->type = BOND_METATRON_CHIRAL;
                b->weight = 0.8f;
                snprintf(b->label, sizeof(b->label), "meta_chi f%d↔f%d %s↔%s",
                    fi, fj, ctx->tensors[i].name, ctx->tensors[j].name);
            }
            /* cross: inter-ring via CROSS_MAP */
            else if (CROSS_MAP[fi] == fj && si == sj) {
                Bond *b = &ctx->graph.bonds[ctx->graph.n_bonds++];
                b->a = i; b->b = j; b->type = BOND_METATRON_CROSS;
                b->weight = 0.7f;
                snprintf(b->label, sizeof(b->label), "meta_crs f%d→f%d %s↔%s",
                    fi, fj, ctx->tensors[i].name, ctx->tensors[j].name);
            }
            /* hub: skip — orbital/chiral/cross cover Metatron routing */
        }
    }
    free(faces); free(slots);
}

/* ══════════════════════════════════════════════════════════════════
   PREDICT: assign hotness to each tensor using cardioid + topo
   Returns hotness array (caller frees). n_layers = total model layers.
   ══════════════════════════════════════════════════════════════════ */

static float *bond_predict_hotness(BondCtx *ctx, int n_layers) {
    bond_cardioid_lut_init();
    float *h = (float*)calloc((size_t)ctx->n_tensors, sizeof(float));
    for (int i = 0; i < ctx->n_tensors; i++) {
        int layer = bond_extract_layer(ctx->tensors[i].name);
        if (layer >= 0)
            h[i] = bond_cardioid_hotness(layer, n_layers);
        else
            h[i] = 0.5f; /* non-layer tensors: neutral */
        /* whitelist: norm.weight always ≥ 0.5 (critical for inference) */
        if (h[i] < 0.5f && strstr(ctx->tensors[i].name, "norm.weight"))
            h[i] = 0.5f;
    }
    /* propagate hotness through bonds: sum neighbours → average → blend */
    for (int pass = 0; pass < 3; pass++) {
        float *acc = (float*)calloc((size_t)ctx->n_tensors, sizeof(float));
        float *wsum = (float*)calloc((size_t)ctx->n_tensors, sizeof(float));
        for (int b = 0; b < ctx->graph.n_bonds; b++) {
            int a = ctx->graph.bonds[b].a;
            int bi = ctx->graph.bonds[b].b;
            float w = ctx->graph.bonds[b].weight;
            acc[a]  += h[bi] * w;  wsum[a]  += w;
            acc[bi] += h[a]  * w;  wsum[bi] += w;
        }
        for (int i = 0; i < ctx->n_tensors; i++) {
            if (wsum[i] > 0.0f) {
                float avg = acc[i] / wsum[i];
                h[i] = h[i] * 0.7f + avg * 0.3f;
                if (h[i] < 0.0f) h[i] = 0.0f;
                if (h[i] > 1.0f) h[i] = 1.0f;
            }
        }
        free(acc); free(wsum);
    }
    return h;
}

static void bond_predict_print(const BondCtx *ctx, const float *hotness) {
    int n_hot = 0, n_warm = 0, n_cold = 0;
    for (int i = 0; i < ctx->n_tensors; i++) {
        if (hotness[i] >= 0.9f) n_hot++;
        else if (hotness[i] >= 0.3f) n_warm++;
        else n_cold++;
    }
    fprintf(stderr, "[predict] hot=%.1f%% warm=%.1f%% cold=%.1f%%\n",
        100.0 * n_hot / ctx->n_tensors,
        100.0 * n_warm / ctx->n_tensors,
        100.0 * n_cold / ctx->n_tensors);
    fprintf(stderr, "[predict] top-5 hottest:\n");
    uint8_t *visited = (uint8_t*)calloc((size_t)ctx->n_tensors, 1);
    for (int rank = 0; rank < 5; rank++) {
        int best = -1; float best_h = -1.0f;
        for (int i = 0; i < ctx->n_tensors; i++) {
            if (visited[i]) continue;
            if (hotness[i] > best_h) { best_h = hotness[i]; best = i; }
        }
        if (best < 0) break;
        visited[best] = 1;
        fprintf(stderr, "  %.2f  %s\n", hotness[best], ctx->tensors[best].name);
    }
    free(visited);
}

#endif

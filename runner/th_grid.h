/*
 * th_grid.h — TriHex Tessellation Coordinate Mapper
 *
 * Maps tensor name → node_id on the Y-triangle field.
 * This is the geometry/arena layer for SID — not a bond predictor.
 * Hex grid (hex_grid.h) handles bond discovery and hotness ranking.
 */

#ifndef TH_GRID_H
#define TH_GRID_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "tri_hex_tess.h"

/* ── Cardioid LUT ── */
#define TH_CARDIOID_LEN    720u

static int16_t _th_cos_lut[TH_CARDIOID_LEN];
static uint8_t _th_cardioid_init = 0;

static inline void th_cardioid_lut_init(void) {
    if (_th_cardioid_init) return;
    for (uint32_t i = 0; i < TH_CARDIOID_LEN; i++) {
        double theta = (2.0 * 3.141592653589793 * i) / TH_CARDIOID_LEN;
        _th_cos_lut[i] = (int16_t)(cos(theta) * 255.0);
    }
    _th_cardioid_init = 1;
}

/* ── State ── */
typedef struct {
    THGrid grid;
    int n_layers;
} THGridState;

static inline void th_grid_init(THGridState *gs, int toggle_level, int n_layers) {
    th_cardioid_lut_init();
    gs->grid.toggle_level = (toggle_level >= 1 && toggle_level <= 3) ? toggle_level : 0;
    gs->grid.aperture = toggle_level == 1 ? 4 : (toggle_level == 2 ? 7 : 3);
    gs->n_layers = n_layers > 0 ? n_layers : 32;
}

/* ── Tensor → THCoord ── */

static inline int th_extract_layer(const char *name) {
    int layer = -1;
    if (sscanf(name, "blk.%d.", &layer) == 1) return layer;
    return -1;
}

static inline int th_face_index(const char *name) {
    if (strstr(name, "attn_q"))      return 0;
    if (strstr(name, "attn_k"))      return 1;
    if (strstr(name, "attn_v"))      return 2;
    if (strstr(name, "attn_output")) return 3;
    if (strstr(name, "ffn_gate"))    return 4;
    if (strstr(name, "ffn_up"))      return 5;
    if (strstr(name, "ffn_down"))    return 6;
    if (strstr(name, "ffn_out"))     return 6;
    if (strstr(name, "attn_norm"))   return 7;
    if (strstr(name, "ffn_norm"))    return 8;
    if (strstr(name, "tok_embd"))    return 9;
    if (strstr(name, "output_norm")) return 11;
    if (strstr(name, "output"))      return 10;
    return -1;
}

/* (vx, vy) from cardioid position + variable radius + layer jitter.
   Layer jitter breaks positional degeneracy: same-face tensors at different
   layers get unique (sector, slot) assignments on the sphere. */
static inline void th_tensor_vxvy(const char *name, int n_layers,
                                   int32_t *vx, int32_t *vy) {
    int layer = th_extract_layer(name);
    if (layer < 0) layer = 0;
    if (n_layers <= 0) n_layers = 32;
    uint16_t pos = (uint16_t)((uint32_t)layer * TH_CARDIOID_LEN
                               / (uint32_t)n_layers) % TH_CARDIOID_LEN;
    int32_t radius = TW_SCALE / 2 + (int32_t)((int64_t)TW_SCALE * layer / n_layers);
    int64_t bx = (int64_t)_th_cos_lut[pos] * radius / 255;
    int64_t by = (int64_t)_th_cos_lut[(pos + 540) % TH_CARDIOID_LEN] * radius / 255;

    /* Per-layer rotation jitter: ~1° per layer, shifts sector+slot assignment
       without changing the overall cardioid direction. */
    double jitter = (double)(layer - n_layers / 2) * 0.035;  /* ±~0.54 rad total */
    double cj = cos(jitter), sj = sin(jitter);
    *vx = (int32_t)(bx * cj - by * sj);
    *vy = (int32_t)(bx * sj + by * cj);
}

static inline THCoord th_from_name(const char *name, THGridState *gs) {
    THCoord c = {0};
    if (gs->grid.toggle_level <= 0) return c;
    int face = th_face_index(name);
    if (face < 0) face = 0;
    int32_t vx, vy;
    th_tensor_vxvy(name, gs->n_layers, &vx, &vy);
    return th_snap((uint8_t)face, vx, vy, &gs->grid);
}

/* ── Debug: print coordinates ── */
static inline void th_print_coords(const char **names, int n_names,
                                    THGridState *gs) {
    fprintf(stderr, "[th] grid: toggle_level=%d aperture=%d n_layers=%d\n",
            gs->grid.toggle_level, gs->grid.aperture, gs->n_layers);
    for (int i = 0; i < n_names; i++) {
        THCoord c = th_from_name(names[i], gs);
        fprintf(stderr, "  pentagon=%d node=%u (shell=%d) %s\n",
                th_pentagon(c), c.node_id, th_shell(c.node_id), names[i]);
    }
}

#endif

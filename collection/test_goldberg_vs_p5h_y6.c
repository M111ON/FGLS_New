/*
 * test_goldberg_vs_p5h_y6.c
 * Goldberg GP(1,1) = 1 pentagon center + 5 hex ring
 * Compare shift on vision/text tensors vs Y6 and P5H
 * gcc test_goldberg_vs_p5h_y6.c -o test_gb -lm && ./test_gb
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "ctd_octa.h"
#include "ctd_pent5hex.h"

/* Goldberg GP(1,1) state:
 * center = pentagon Y3 (5-fold)
 * ring   = 5 hex Y3 instances (60° each, scaled)
 * final centroid = weighted avg (pentagon × 5 + hex × 1 each)
 */

#define GB_HEX_ROT   1.0471975f  /* 60° = π/3 between hex centers */
#define GB_HEX_SCALE 1.0f        /* hex same scale as pentagon */
#define GB_PENT_W    5.0f        /* pentagon weight = 5 */
#define GB_HEX_W     1.0f        /* each hex weight = 1 */

typedef struct {
    Y3State pent;        /* center pentagon */
    Y3State hex[5];      /* 5 hex ring */
    float   gb_center[3];
    float   gb_shift;
    float   conf;
    uint8_t face;
    uint8_t compound;
    uint8_t pad[2];
} GBState;

static inline void _rotate_dirs_z(Y3State *s, float angle) {
    float c = cosf(angle), si = sinf(angle);
    for (int i = 0; i < 3; i++) {
        float x = s->dirs[i][0], y = s->dirs[i][1];
        s->dirs[i][0] = x*c - y*si;
        s->dirs[i][1] = x*si + y*c;
    }
    _m33_invert(s->dirs_inv, s->dirs);
}

static inline void gb_init(GBState *s, const char *name) {
    char buf[128];
    /* pentagon center */
    snprintf(buf, 127, "%s_GB_P", name);
    y3_init(&s->pent, buf);
    s->pent.phase = 0;

    /* 5 hex ring: rotate 72°×i around pentagon */
    for (int i = 0; i < 5; i++) {
        snprintf(buf, 127, "%s_GB_H%d", name, i);
        y3_init(&s->hex[i], buf);
        s->hex[i].phase = 1;
        _rotate_dirs_z(&s->hex[i], i * 1.2566370f); /* 72° × i */
    }
    memset(s->gb_center, 0, sizeof(s->gb_center));
    s->gb_shift = 0; s->conf = 1.f; s->face = 0; s->compound = 0;
}

static inline void gb_snapshot(GBState *s) {
    y3_snapshot(&s->pent);
    for (int i = 0; i < 5; i++) y3_snapshot(&s->hex[i]);
    memset(s->gb_center, 0, sizeof(s->gb_center));
    s->gb_shift = 0;
}

/* Encode: f0-f2 → pentagon, f3-f5 → hex ring (same features, different dirs) */
static inline void gb_encode(GBState *s,
                              float f0, float f1, float f2,
                              float f3, float f4, float f5) {
    y3_encode(&s->pent, f0, f1, f2);
    for (int i = 0; i < 5; i++)
        y3_encode(&s->hex[i], f3, f4, f5);

    /* Weighted centroid: pentagon×5 + sum(hex)×1 each / 10 */
    float sum[3] = {0,0,0};
    /* pentagon contributes weight 5 */
    sum[0] += s->pent.centroid[0] * GB_PENT_W;
    sum[1] += s->pent.centroid[1] * GB_PENT_W;
    sum[2] += s->pent.centroid[2] * GB_PENT_W;
    /* 5 hex each weight 1 */
    for (int i = 0; i < 5; i++) {
        sum[0] += s->hex[i].centroid[0];
        sum[1] += s->hex[i].centroid[1];
        sum[2] += s->hex[i].centroid[2];
    }
    float w_total = GB_PENT_W + 5.f * GB_HEX_W;
    s->gb_center[0] = sum[0]/w_total;
    s->gb_center[1] = sum[1]/w_total;
    s->gb_center[2] = sum[2]/w_total;
    s->gb_shift = _v3_norm(s->gb_center);

    /* Conf: variance of hex shifts */
    float mean_hex = 0;
    for (int i=0;i<5;i++) mean_hex += s->hex[i].shift;
    mean_hex /= 5.f;
    float var = 0;
    for (int i=0;i<5;i++) { float d=s->hex[i].shift-mean_hex; var+=d*d; }
    var /= 5.f;
    s->conf = 1.f / (1.f + var);

    s->face = _vec_to_face(s->gb_center);
    s->compound = _shift_to_compound(s->gb_shift);
}

static inline void gb_recon(const GBState *s, float out[6]) {
    y3_recon(&s->pent, out);
    y3_recon(&s->hex[0], out+3);
}

/* ── Test tensors ── */
typedef struct { const char *name; float f[6]; } TSim;
static TSim T[] = {
    {"text_model.layers.0.attn.q_proj.weight",   {0.01f,0.02f,0.015f,0.5f,0.3f,0.2f}},
    {"text_model.layers.0.attn.k_proj.weight",   {0.01f,0.01f,0.012f,0.4f,0.4f,0.3f}},
    {"text_model.layers.1.ffn.gate_proj.weight", {0.05f,0.06f,0.04f, 1.2f,0.8f,0.6f}},
    {"text_model.layers.1.ffn.down_proj.weight", {0.08f,0.07f,0.09f, 2.1f,1.5f,1.2f}},
    {"vision_model.encoder.layers.0.attn.q",     {0.2f, 0.3f, 0.25f, 5.0f,4.0f,3.5f}},
    {"vision_model.encoder.layers.4.mlp.fc2",    {0.5f, 0.8f, 0.6f,  9.0f,7.0f,6.0f}},
    {"model.layers.0.attn.q_proj.weight",        {0.03f,0.04f,0.03f, 0.8f,0.6f,0.5f}},
    {"model.layers.20.ffn.down_proj.weight",     {0.1f, 0.12f,0.09f, 3.0f,2.5f,2.0f}},
    {"voice.encoder.layers.0.weight",            {0.02f,0.02f,0.02f, 0.3f,0.3f,0.3f}},
    {"voice.decoder.layers.5.weight",            {0.04f,0.05f,0.04f, 0.6f,0.5f,0.5f}},
};
#define NT 10

int main(void) {
    printf("=== Goldberg GP(1,1) vs P5H vs Y6 ===\n");
    printf("(1 pentagon center + 5 hex ring)\n\n");

    printf("%-42s  Y6_shift  P5H_shift  GB_shift  Best\n","Tensor");
    printf("%-42s  --------  ---------  --------  ----\n","------");

    float sum_y6=0, sum_p5h=0, sum_gb=0;
    int win_y6=0, win_p5h=0, win_gb=0;
    uint8_t faces_gb[12]={0};

    for (int i = 0; i < NT; i++) {
        /* Y6 */
        Y6State ys; y6_init(&ys, T[i].name); y6_snapshot(&ys);
        y6_encode(&ys, T[i].f[0],T[i].f[1],T[i].f[2],T[i].f[3],T[i].f[4],T[i].f[5]);

        /* P5H */
        P5HState ps; p5h_init(&ps, T[i].name); p5h_snapshot(&ps);
        p5h_encode(&ps, T[i].f[0],T[i].f[1],T[i].f[2],T[i].f[3],T[i].f[4],T[i].f[5]);

        /* Goldberg */
        GBState gs; gb_init(&gs, T[i].name); gb_snapshot(&gs);
        gb_encode(&gs, T[i].f[0],T[i].f[1],T[i].f[2],T[i].f[3],T[i].f[4],T[i].f[5]);

        float min_shift = ys.hex_shift;
        const char *best = "Y6 ";
        if (ps.pent_shift < min_shift) { min_shift=ps.pent_shift; best="P5H"; }
        if (gs.gb_shift   < min_shift) { min_shift=gs.gb_shift;   best="GB "; }

        if (strcmp(best,"Y6 ")==0) win_y6++;
        else if (strcmp(best,"P5H")==0) win_p5h++;
        else win_gb++;

        const char *nm = T[i].name;
        int nl=strlen(nm); if(nl>41) nm=nm+nl-41;
        printf("%-42s  %8.4f  %9.4f  %8.4f  %s\n",
               nm, ys.hex_shift, ps.pent_shift, gs.gb_shift, best);

        sum_y6  += ys.hex_shift;
        sum_p5h += ps.pent_shift;
        sum_gb  += gs.gb_shift;
        if(gs.face<12) faces_gb[gs.face]++;
    }

    printf("%-42s  %8.4f  %9.4f  %8.4f\n","AVERAGE",
           sum_y6/NT, sum_p5h/NT, sum_gb/NT);

    printf("\n--- Wins ---\n");
    printf("Y6 : %d/%d\n", win_y6,  NT);
    printf("P5H: %d/%d\n", win_p5h, NT);
    printf("GB : %d/%d\n", win_gb,  NT);

    int nf_gb=0; for(int i=0;i<12;i++) if(faces_gb[i]) nf_gb++;
    printf("\nGB face coverage: %d/12\n", nf_gb);

    printf("\n--- vision tensors specifically ---\n");
    for (int i = 4; i <= 5; i++) {
        Y6State ys; y6_init(&ys,T[i].name); y6_snapshot(&ys);
        y6_encode(&ys,T[i].f[0],T[i].f[1],T[i].f[2],T[i].f[3],T[i].f[4],T[i].f[5]);
        P5HState ps; p5h_init(&ps,T[i].name); p5h_snapshot(&ps);
        p5h_encode(&ps,T[i].f[0],T[i].f[1],T[i].f[2],T[i].f[3],T[i].f[4],T[i].f[5]);
        GBState gs; gb_init(&gs,T[i].name); gb_snapshot(&gs);
        gb_encode(&gs,T[i].f[0],T[i].f[1],T[i].f[2],T[i].f[3],T[i].f[4],T[i].f[5]);
        printf("%s\n  Y6=%.4f P5H=%.4f GB=%.4f  best=%s conf=%.3f\n",
               T[i].name, ys.hex_shift, ps.pent_shift, gs.gb_shift,
               (gs.gb_shift<=ps.pent_shift&&gs.gb_shift<=ys.hex_shift)?"GB":
               (ps.pent_shift<=ys.hex_shift)?"P5H":"Y6",
               gs.conf);
    }
    return 0;
}

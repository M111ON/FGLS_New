#ifndef SESSION_PROFILE_H
#define SESSION_PROFILE_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define SES_MAGIC   0x32534553
#define SES1_MAGIC  0x31534553
#define SES_N_FACES    8
#define SES_N_SPOKES  24
#define SES_N_SLOTS    5
#define SES_N_BINS    (SES_N_FACES * SES_N_SPOKES * SES_N_SLOTS)
#define SES_TIMELINE_MAX  4096

typedef struct {
    uint32_t magic;
    uint32_t n_frames;
    uint8_t  last_face;
    uint8_t  pad[3];
    uint32_t bins[SES_N_BINS];
    uint32_t trans[SES_N_FACES][SES_N_FACES];
} SessionProfile;

typedef struct {
    uint8_t  face;
    uint32_t step;
} TimelineEntry;

static inline uint32_t ses_fnv1a_ints(const int *tokens, int n) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < n; i++) {
        h ^= (uint32_t)tokens[i];
        h *= 0x01000193u;
    }
    return h;
}

static inline void ses_profile_init(SessionProfile *sp) {
    memset(sp, 0, sizeof(*sp));
    sp->magic = SES_MAGIC;
}

static inline void ses_profile_transition(SessionProfile *sp, uint8_t from, uint8_t to) {
    if (from < SES_N_FACES && to < SES_N_FACES)
        sp->trans[from][to]++;
}

static inline void ses_profile_push_timeline(SessionProfile *sp, uint8_t face, uint32_t step) {
    (void)sp; (void)face; (void)step;
}

static inline double ses_profile_face_entropy(SessionProfile *sp) {
    double total = 0;
    double face_sum[SES_N_FACES] = {0};
    for (int f = 0; f < SES_N_FACES; f++)
        for (int s = 0; s < SES_N_SPOKES; s++)
            for (int l = 0; l < SES_N_SLOTS; l++)
                face_sum[f] += (double)sp->bins[f * SES_N_SPOKES * SES_N_SLOTS + s * SES_N_SLOTS + l];
    for (int f = 0; f < SES_N_FACES; f++) total += face_sum[f];
    if (total == 0) return 0;
    double ent = 0;
    for (int f = 0; f < SES_N_FACES; f++) {
        if (face_sum[f] == 0) continue;
        double p = face_sum[f] / total;
        ent -= p * log2(p);
    }
    return ent;
}

static inline double ses_profile_face_balance(SessionProfile *sp) {
    double e = ses_profile_face_entropy(sp);
    double max_e = log2((double)SES_N_FACES);
    return max_e > 0 ? e / max_e : 0;
}

static inline double ses_profile_transition_entropy(SessionProfile *sp) {
    double total_ent = 0;
    int rows = 0;
    for (int f = 0; f < SES_N_FACES; f++) {
        double row_sum = 0;
        for (int t = 0; t < SES_N_FACES; t++)
            row_sum += (double)sp->trans[f][t];
        if (row_sum == 0) continue;
        double ent = 0;
        for (int t = 0; t < SES_N_FACES; t++) {
            if (sp->trans[f][t] == 0) continue;
            double p = (double)sp->trans[f][t] / row_sum;
            ent -= p * log2(p);
        }
        total_ent += ent;
        rows++;
    }
    return rows > 0 ? total_ent / rows : 0;
}

static inline double ses_profile_stability(SessionProfile *sp) {
    double total = 0, self = 0;
    for (int f = 0; f < SES_N_FACES; f++)
        for (int t = 0; t < SES_N_FACES; t++) {
            total += (double)sp->trans[f][t];
            if (f == t) self += (double)sp->trans[f][t];
        }
    return total > 0 ? self / total : 0;
}

static inline double ses_profile_quality(SessionProfile *sp) {
    double bal = ses_profile_face_balance(sp);
    double te = ses_profile_transition_entropy(sp);
    double te_max = log2((double)SES_N_FACES);
    double te_norm = te_max > 0 ? te / te_max : 0;
    double stab = ses_profile_stability(sp);
    return 0.40 * bal + 0.35 * (1.0 - te_norm) + 0.25 * stab;
}

static inline void ses_profile_print_quality(SessionProfile *sp) {
    fprintf(stderr, "[quality] entropy=%.4f balance=%.4f trans_entropy=%.4f stability=%.4f quality=%.4f\n",
        ses_profile_face_entropy(sp),
        ses_profile_face_balance(sp),
        ses_profile_transition_entropy(sp),
        ses_profile_stability(sp),
        ses_profile_quality(sp));
}

static inline uint8_t ses_select_face(SessionProfile *sp, uint32_t sem_hash,
                                       double sem_weight, double hist_weight, double rnd_weight)
{
    if (sp->n_frames == 0) {
        return (uint8_t)(sem_hash % SES_N_FACES);
    }
    double scores[SES_N_FACES];
    memset(scores, 0, sizeof(scores));
    double total = 0;
    for (int f = 0; f < SES_N_FACES; f++)
        for (int s = 0; s < SES_N_SPOKES; s++)
            for (int l = 0; l < SES_N_SLOTS; l++)
                total += (double)sp->bins[f * SES_N_SPOKES * SES_N_SLOTS + s * SES_N_SLOTS + l];
    for (int f = 0; f < SES_N_FACES; f++) {
        double face_total = 0;
        for (int s = 0; s < SES_N_SPOKES; s++)
            for (int l = 0; l < SES_N_SLOTS; l++)
                face_total += (double)sp->bins[f * SES_N_SPOKES * SES_N_SLOTS + s * SES_N_SLOTS + l];
        double hist = total > 0 ? face_total / total : 0;
        double sem = (double)((sem_hash >> (f * 4)) & 0xF) / 15.0;
        double rnd = (double)((sem_hash * (f + 1) * 0x9E3779B9u) & 0xFF) / 255.0;
        scores[f] = sem * sem_weight + hist * hist_weight + rnd * rnd_weight;
    }
    int best = 0;
    for (int f = 1; f < SES_N_FACES; f++)
        if (scores[f] > scores[best]) best = f;
    return (uint8_t)best;
}

static inline void ses_decay_transitions(SessionProfile *sp, double factor) {
    for (int f = 0; f < SES_N_FACES; f++)
        for (int t = 0; t < SES_N_FACES; t++)
            sp->trans[f][t] = (uint32_t)((double)sp->trans[f][t] * factor + 0.5);
}

static inline void ses_profile_print(SessionProfile *sp) {
    fprintf(stderr, "[ses] n_frames=%u last_face=%u\n", sp->n_frames, sp->last_face);
    fprintf(stderr, "[ses] face usage: ");
    for (int f = 0; f < SES_N_FACES; f++) {
        double sum = 0;
        for (int s = 0; s < SES_N_SPOKES; s++)
            for (int l = 0; l < SES_N_SLOTS; l++)
                sum += (double)sp->bins[f * SES_N_SPOKES * SES_N_SLOTS + s * SES_N_SLOTS + l];
        double total = 0;
        for (int i = 0; i < SES_N_BINS; i++) total += (double)sp->bins[i];
        fprintf(stderr, "F%d=%.1f%%%s", f, total > 0 ? sum/total*100 : 0, f < SES_N_FACES-1 ? " " : "\n");
    }
    if (sp->magic == SES_MAGIC) {
        fprintf(stderr, "[ses] transition matrix:\n");
        for (int f = 0; f < SES_N_FACES; f++) {
            fprintf(stderr, "       ");
            for (int t = 0; t < SES_N_FACES; t++)
                fprintf(stderr, "%4u%s", sp->trans[f][t], t < SES_N_FACES-1 ? " " : "\n");
        }
    }
}

static int ses_profile_save(const char *path, SessionProfile *sp) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t m = SES_MAGIC;
    fwrite(&m, 4, 1, f);
    fwrite(&sp->n_frames, 4, 1, f);
    fwrite(&sp->last_face, 1, 1, f);
    fwrite(sp->pad, 3, 1, f);
    fwrite(sp->bins, 4, SES_N_BINS, f);
    fwrite(sp->trans, 4, SES_N_FACES * SES_N_FACES, f);
    fclose(f);
    return 0;
}

static int ses_profile_load(const char *path, SessionProfile *sp) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t m;
    if (fread(&m, 4, 1, f) != 1) { fclose(f); return -1; }
    if (m != SES_MAGIC && m != SES1_MAGIC) { fclose(f); return -1; }
    sp->magic = SES_MAGIC;
    if (fread(&sp->n_frames, 4, 1, f) != 1) { fclose(f); return -1; }
    if (m == SES_MAGIC) {
    if (fread(&sp->last_face, 1, 1, f) != 1) { fclose(f); return -1; }
    if (fread(sp->pad, 3, 1, f) != 1) { fclose(f); return -1; }
    } else {
        sp->last_face = 0;
        memset(sp->pad, 0, 3);
    }
    if (fread(sp->bins, 4, SES_N_BINS, f) != SES_N_BINS) { fclose(f); return -1; }
    if (m == SES_MAGIC) {
        if (fread(sp->trans, 4, SES_N_FACES * SES_N_FACES, f) != (size_t)(SES_N_FACES * SES_N_FACES)) {
            memset(sp->trans, 0, sizeof(sp->trans));
        }
    } else {
        memset(sp->trans, 0, sizeof(sp->trans));
    }
    fclose(f);
    return 0;
}

#endif

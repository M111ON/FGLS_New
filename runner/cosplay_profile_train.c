/*
 * cosplay_profile_train.c — Profile-aware cosplay trainer
 * Uses .ses session profile to modulate per-tensor stride.
 * Usage: cosplay_profile_train <profile.ses> <store.gsten> <output.cpl>
 *
 * Compile:
 *   gcc -O2 -std=c11 -I. -Irunner -Icollection -Icollection/src -Icollection/core
 *       -Icollection/core/core -Icollection/core/pogls_engine/core
 *       -Icollection/core/geo_headers -Icollection/geo_jump_module/include
 *       -o runner/cosplay_profile_train.exe runner/cosplay_profile_train.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "cosplay.h"
#include "session_profile.h"

#define MIN_WEIGHT_BYTES (64 * 1024)
#define GEO_OCTANT_SZ 20

static const uint8_t GEO_OCTANT[GEO_OCTANT_SZ] = {
    0,1,2,3,4,5,6,7, 0,1,2,3,4,5,6,7, 0,1,2,3
};

static inline int hash_to_face(const char *name) {
    uint32_t h = cosplay_fnv1a(name);
    return GEO_OCTANT[h % GEO_OCTANT_SZ];
}

static int stride_from_usage(double usage) {
    if (usage > 0.20) return 32;
    if (usage > 0.10) return 48;
    if (usage > 0.04) return 64;
    if (usage > 0.005) return 96;
    return 128;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <profile.ses> <store.gsten> <output.cpl>\n", argv[0]);
        return 1;
    }
    const char *ses_path = argv[1];
    const char *gsten_path = argv[2];
    const char *cpl_path = argv[3];

    SessionProfile sp;
    if (ses_profile_load(ses_path, &sp) != 0) {
        fprintf(stderr, "ERROR: cannot load %s\n", ses_path); return 1;
    }
    fprintf(stderr, "[cpt] profile: %u frames\n", sp.n_frames);

    double face_usage[8] = {0};
    double total = 0;
    for (int f = 0; f < 8; f++)
        for (int s = 0; s < 24; s++)
            for (int l = 0; l < 5; l++) {
                double v = (double)sp.bins[f * 24 * 5 + s * 5 + l];
                face_usage[f] += v;
                total += v;
            }
    for (int f = 0; f < 8; f++) {
        face_usage[f] /= total;
        fprintf(stderr, "[cpt] face %d usage: %.4f\n", f, face_usage[f]);
    }

    FILE *f = fopen(gsten_path, "rb");
    if (!f) { fprintf(stderr, "ERROR: cannot open %s\n", gsten_path); return 1; }

    uint32_t magic, ver, n_tensors, n_freeze;
    char ts[32];
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x474E5453) {
        fprintf(stderr, "ERROR: bad gsten magic\n"); fclose(f); return 1;
    }
    fread(&ver, 4, 1, f);
    fread(&n_tensors, 4, 1, f);
    fread(&n_freeze, 4, 1, f);
    fread(ts, 32, 1, f);

    CosplayProfile cp;
    memset(&cp, 0, sizeof(cp));
    cp.n = 0;

    for (uint32_t i = 0; i < n_tensors && cp.n < CP_MAX_ENTRIES; i++) {
        uint16_t name_len;
        if (fread(&name_len, 2, 1, f) != 1) break;
        if (name_len > 255) name_len = 255;
        char name[256];
        if (fread(name, 1, name_len, f) != name_len) break;
        name[name_len] = 0;
        uint64_t nbytes;
        if (fread(&nbytes, 8, 1, f) != 1) break;
        if (nbytes < MIN_WEIGHT_BYTES) {
            fseek(f, (long)nbytes, SEEK_CUR); continue;
        }
        fseek(f, (long)nbytes, SEEK_CUR);

        int face = hash_to_face(name);
        int stride = stride_from_usage(face_usage[face]);

        CosplayEntry *e = &cp.entries[cp.n];
        e->name_hash = cosplay_fnv1a(name);
        e->mode = CP_XOR;
        e->arg = 0x01;
        e->stride = (uint16_t)stride;
        e->data_size = (uint32_t)(nbytes > 0xFFFFFFFF ? 0xFFFFFFFF : nbytes);
        e->tick = 0;
        cp.n++;
    }
    fclose(f);

    int counts[5] = {0};
    for (uint32_t i = 0; i < cp.n; i++) {
        int s = cp.entries[i].stride;
        if (s <= 32) counts[0]++;
        else if (s <= 48) counts[1]++;
        else if (s <= 64) counts[2]++;
        else if (s <= 96) counts[3]++;
        else counts[4]++;
    }
    fprintf(stderr, "[cpt] stride distribution: stride32=%d stride48=%d stride64=%d stride96=%d stride128=%d\n",
            counts[0], counts[1], counts[2], counts[3], counts[4]);

    if (cosplay_save(cpl_path, &cp) != 0) {
        fprintf(stderr, "ERROR: save failed\n"); return 1;
    }
    fprintf(stderr, "[cpt] saved %s (%u entries, %.1f KB)\n",
            cpl_path, cp.n, (double)(cp.n * sizeof(CosplayEntry) + 20) / 1024);
    return 0;
}

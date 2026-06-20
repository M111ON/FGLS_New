/*
 * cosplay_train.c — Train cosplay perturbation profile from .gsten tensor store
 * Usage: cosplay_train <store.gsten> <output.cpl>
 *
 * Compile:
 *   gcc -O2 -std=c11 -I. -Icollection -Icollection/src -Icollection/core
 *       -Icollection/core/core -Icollection/core/pogls_engine/core
 *       -Icollection/core/geo_headers -Icollection/geo_jump_module/include
 *       -II:/llama.cpp/include -II:/llama.cpp/ggml/include
 *       -o runner/cosplay_train.exe runner/cosplay_train.c
 *       collection/geo_jump_module/src/geo_jump.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cosplay.h"

#define MIN_WEIGHT_BYTES (64 * 1024)

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <store.gsten> <output.cpl>\n", argv[0]);
        return 1;
    }
    const char *gsten_path = argv[1];
    const char *cpl_path = argv[2];

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
    fprintf(stderr, "[cosplay_train] gsten: %u tensors, %u freeze entries\n",
            n_tensors, n_freeze);

    CosplayProfile cp;
    memset(&cp, 0, sizeof(cp));
    cp.n = 0;

    uint32_t skipped_small = 0;
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
            skipped_small++;
            fseek(f, (long)nbytes, SEEK_CUR);
            continue;
        }

        fseek(f, (long)nbytes, SEEK_CUR);

        CosplayEntry *e = &cp.entries[cp.n];
        e->name_hash = cosplay_fnv1a(name);
        e->mode = CP_XOR;
        e->arg = 0x01;
        e->stride = 64;
        e->data_size = (uint32_t)(nbytes > 0xFFFFFFFF ? 0xFFFFFFFF : nbytes);
        e->tick = 0;
        cp.n++;
    }

    fclose(f);

    fprintf(stderr, "[cosplay_train] %u weight tensors >= 64KB, %u skipped (small)\n",
            cp.n, skipped_small);

    if (cosplay_save(cpl_path, &cp) != 0) {
        fprintf(stderr, "ERROR: save failed\n"); return 1;
    }
    fprintf(stderr, "[cosplay_train] saved %s (%u entries)\n", cpl_path, cp.n);
    return 0;
}
